// ============================================================================
// PiTrex mode: DDR ring reader (renderer clock domain, clk_125).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A read client and a write client on vfb_ddr_arbiter. The ARM (HPS) writes
// 64-bit commands into a ring in DDR3 and publishes a write index in a header;
// this module polls the header, fetches up to 32 commands per burst into the
// command FIFO, and writes back its read index, the machine's stream time and
// the input block. Layout in 64-bit words from PTX_BASE (byte = word * 8):
//   word 0      {version, magic "PTRX"}      ARM
//   word 1      ring size, log2 of entries   ARM
//   word 2      wr_idx                       ARM
//   word 3      {~hb ^ "PTRX", hb}           ARM heartbeat (coded; must keep changing)
//   word 8      rd_idx                       core
//   word 9      core_cycles (stream time)    core
//   word 10     {underruns, frames}          core
//   word 4      audio wr_idx (words)         ARM (B6)
//   word 11     {epoch, 0, build, "PTXC"}    core (identifies the core build; bits 63..48
//                                            count every takeover the core dropped, B7)
//   word 12     audio rd_idx (words fetched) core (B6)
//   word 16     {pad, psg_ioa, y2, x2, y1, x1}  core
//   word 17     {0, seq}                     core
//   word 0x200  ring, 65536 entries
//   word 0x10200 audio ring, 8192 words of four 16-bit samples (32 kHz) (B6)
//
// Takeover (B7): a header is taken over only if version = 0, ring size = 2^16,
// the producer/consumer distance is legal, and the coded heartbeat has been seen
// to MOVE: the first accepted value is only a baseline, so a header left in
// memory by a program that is no longer running never takes the machine over.
//
// Reset contract (B7). Two inputs take part in a core reset:
//   reset  arbiter-coordinated client reset: high from the start of a video reset
//          until vfb_ddr_arbiter has finished draining any burst it was serving
//          (vfb_top ptx_client_reset). The FSM is parked in S_IDLE with both
//          requests withdrawn, so it can never wait for beats the arbiter discards.
//          An abandoned write burst is left alone: wbeat keeps following wr_advance
//          and the status snapshot is frozen, so the payload is stable while it
//          drains, and no new status burst starts until the old one has finished.
//   drop   level, any machine reset (menu reset, cartridge load, ...): no arbiter
//          involvement, the FSM keeps running and status writes continue.
// Either one drops the takeover (present = 0: the caller flushes the FIFOs and the
// 6809 is released to the BIOS), counts a new epoch (word 11) and requires the ARM
// to re-attach: no takeover until a header with the magic cleared has been seen.
// ============================================================================

`default_nettype none

module pitrex_ddr #(
	parameter logic [28:0] PTX_BASE = 29'h06800000,
	parameter logic [7:0]  BUILD    = 8'd0
) (
	input  wire         clk,
	input  wire         enable,          // synchronised to clk by the caller
	input  wire         reset,           // arbiter-coordinated client reset (see above)
	input  wire         drop,            // machine reset level, synchronised to clk by the caller
	output logic        present = 1'b0,  // a live ARM program is streaming (coded heartbeat)

	// arbiter read client
	output logic        rd_ready = 1'b0,
	input  wire         rd_grant,
	output logic [28:0] rd_addr = '0,
	output logic [7:0]  rd_burstcnt = '0,
	input  wire  [63:0] rd_data,
	input  wire         rd_data_valid,

	// arbiter write client
	output logic        wr_ready = 1'b0,
	input  wire         wr_grant,
	input  wire         wr_done,
	output logic [28:0] wr_addr = '0,
	output logic [7:0]  wr_burstcnt = '0,
	output logic [63:0] wr_data,
	output logic [7:0]  wr_be,
	input  wire         wr_advance,

	// command FIFO write side
	output logic        fifo_wr_en = 1'b0,
	output logic [63:0] fifo_wr_data = '0,
	input  wire         fifo_almost_full,  // fewer than one burst free

	// audio FIFO write side (PCM ring, four 16-bit samples per word)
	output logic        aud_fifo_wr_en = 1'b0,
	output logic [63:0] aud_fifo_wr_data = '0,
	input  wire         aud_fifo_almost_full,

	// snapshot from the machine domain (held stable between toggles)
	input  wire [47:0]  snap_cycles,
	input  wire [15:0]  snap_frames,
	input  wire [15:0]  snap_underruns,
	input  wire [31:0]  snap_pots,
	input  wire [7:0]   snap_ioa,
	input  wire         snap_toggle
);

localparam logic [31:0] MAGIC     = 32'h58525450;   // "PTRX" little-endian
localparam logic [31:0] VERSION   = 32'd0;          // header word 0, bits 63..32
localparam logic [28:0] RING_BASE = PTX_BASE + 29'h200;
localparam integer      RING_LOG2 = 16;
localparam logic [15:0] POLL_GAP  = 16'd250;        // 2 us between idle polls
localparam logic [15:0] STATUS_GAP = 16'd62500;     // 0.5 ms between status writes

// PCM audio ring (B6): after the command ring, 8192 words of four samples each
localparam logic [28:0] AUD_BASE = PTX_BASE + 29'h10200;
localparam integer      AUD_LOG2 = 13;

typedef enum logic [3:0] {
	S_IDLE, S_HDR_REQ, S_HDR_DATA, S_RING_REQ, S_RING_DATA, S_STAT_REQ, S_STAT_DATA, S_SETTLE,
	S_AUD_REQ, S_AUD_DATA
} state_t;
state_t state = S_IDLE;

logic [63:0] rd_idx = '0;
logic [16:0] wr_lo = '0;          // low bits suffice: the ARM is never > 65536 ahead
logic        magic_ok = 1'b0;     // header valid: magic, version, ring size
logic        hdr_m = 1'b0;        // word 0 of the header in hand was valid
logic [31:0] hb_last = '0;
logic        hb_have = 1'b0;      // hb_last is a baseline sample; the next change means "alive"
logic        need_detach = 1'b0;  // a reset dropped the takeover: wait for the ARM to clear the magic
logic [23:0] hb_timer = '0;
localparam logic [23:0] HB_TIMEOUT = 24'd12_500_000;   // 100 ms at 125 MHz
logic [2:0]  beat = '0;
logic [5:0]  fetch_n = '0;
logic [5:0]  fetch_left = '0;
logic [15:0] poll_timer = '0;
logic [15:0] status_timer = '0;
logic        status_due = 1'b0;

wire kill = reset | drop;         // drop the takeover

// takeover epoch: counts every present 1 -> 0 (reset, hand-back, crash) for the ARM
logic        present_q = 1'b0;
logic [15:0] epoch = '0;
always_ff @(posedge clk) begin
	present_q <= present;
	if (present_q && !present) epoch <= epoch + 1'd1;
end

// snapshot capture: the toggle is synchronised, then the held snapshot is captured.
// Data and toggle change together in clk_sys; the data path is bounded in Vectrex.sdc
// (set_net_delay) to well under the toggle synchroniser's latency.
(* altera_attribute = {"-name SYNCHRONIZER_IDENTIFICATION FORCED_IF_ASYNCHRONOUS"} *)
logic [2:0]  tog_sync = '0;
logic [47:0] cap_cycles = '0;
logic [15:0] cap_frames = '0, cap_underruns = '0;
logic [31:0] cap_pots = '0;
logic [7:0]  cap_ioa = 8'hFF;
logic [31:0] seq = '0;

always_ff @(posedge clk) begin
	tog_sync <= {tog_sync[1:0], snap_toggle};
	if (tog_sync[2] != tog_sync[1]) begin
		cap_cycles    <= snap_cycles;
		cap_frames    <= snap_frames;
		cap_underruns <= snap_underruns;
		cap_pots      <= snap_pots;
		cap_ioa       <= snap_ioa;
		seq           <= seq + 1'd1;
	end
end

// ring fetch size: min(32, available, words before the ring wraps), computed
// one cycle ahead in registers to keep the 125 MHz paths short. A distance above
// the ring size is impossible for a sane producer and fetches nothing.
logic [16:0] avail_q = '0;
logic [5:0]  n_fetch_q = '0;
wire  [16:0] avail_d   = wr_lo - rd_idx[16:0];
wire  [16:0] to_wrap   = 17'h10000 - {1'b0, rd_idx[RING_LOG2-1:0]};
wire  [5:0]  n_avail   = (avail_d > 17'd32) ? 6'd32 : avail_d[5:0];
always_ff @(posedge clk) begin
	avail_q   <= (avail_d > 17'h10000) ? 17'd0 : avail_d;
	n_fetch_q <= ({11'd0, n_avail} > to_wrap) ? to_wrap[5:0] : n_avail;
end

// audio fetch size: min(16, available, words before the audio ring wraps), registered
logic [13:0] aud_rd = '0;          // words fetched
logic [13:0] aud_wr_lo = '0;       // ARM's audio write index (words), low bits
logic [13:0] aud_avail_q = '0;
logic [4:0]  aud_n_q = '0;
logic [4:0]  aud_n = '0, aud_left = '0;
wire  [13:0] aud_avail_d = aud_wr_lo - aud_rd;
wire  [13:0] aud_to_wrap = 14'h2000 - {1'b0, aud_rd[AUD_LOG2-1:0]};
wire  [4:0]  aud_n_avail = (aud_avail_d > 14'd16) ? 5'd16 : aud_avail_d[4:0];
always_ff @(posedge clk) begin
	aud_avail_q <= (aud_avail_d > 14'h2000) ? 14'd0 : aud_avail_d;
	aud_n_q     <= ({9'd0, aud_n_avail} > aud_to_wrap) ? aud_to_wrap[4:0] : aud_n_avail;
end

// status burst: words 8..17 (10 beats); byte enables off on words 13..15.
// The arbiter takes DIN combinationally and pulses wr_advance per accepted beat.
// Every field is snapshotted when the burst is requested (st_*), so DIN, BE and
// ADDR never change while a beat is stalled, and the fields are coherent.
logic [3:0]  wbeat = '0;
logic        wr_stale = 1'b0;          // an abandoned burst is still draining
logic [63:0] st_rd_idx = '0;
logic [47:0] st_cycles = '0;
logic [15:0] st_frames = '0, st_underruns = '0, st_epoch = '0;
logic [13:0] st_aud_rd = '0;
logic [31:0] st_pots = '0, st_seq = '0;
logic [7:0]  st_ioa = 8'hFF;
wire  [3:0]  wbeat_n = wr_advance ? wbeat + 1'd1 : wbeat;
always_comb begin
	wr_be   = 8'hFF;
	wr_data = 64'd0;
	case (wbeat)
		4'd0: wr_data = st_rd_idx;
		4'd1: wr_data = {16'd0, st_cycles};
		4'd2: wr_data = {16'd0, st_underruns, 16'd0, st_frames};
		4'd3: wr_data = {st_epoch, 8'd0, BUILD, 32'h43585450};   // "PTXC"
		4'd4: wr_data = {50'd0, st_aud_rd};                      // audio words fetched
		4'd8: wr_data = {24'd0, st_ioa, st_pots};
		4'd9: wr_data = {32'd0, st_seq};
		default: wr_be = 8'h00;
	endcase
end

always_ff @(posedge clk) begin
	fifo_wr_en <= 1'b0;
	aud_fifo_wr_en <= 1'b0;
	if (poll_timer != 0)   poll_timer <= poll_timer - 1'd1;
	if (status_timer != 0) status_timer <= status_timer - 1'd1;
	else                   status_due <= 1'b1;
	if (hb_timer != 0)     hb_timer <= hb_timer - 1'd1;
	else                   present <= 1'b0;     // heartbeat stopped: hand back

	if (wr_advance) wbeat <= wbeat_n;           // may fire in the grant cycle
	if (wbeat_n == 4'd10) wr_stale <= 1'b0;     // the abandoned burst has drained

	if (!enable) begin
		state       <= S_IDLE;
		rd_ready    <= 1'b0;
		wr_ready    <= 1'b0;
		if ((state == S_STAT_DATA || (state == S_STAT_REQ && wr_grant)) && wbeat_n != 4'd10)
			wr_stale <= 1'b1;
		rd_idx      <= '0;
		wr_lo       <= '0;
		aud_rd      <= '0;
		aud_wr_lo   <= '0;
		magic_ok    <= 1'b0;
		hdr_m       <= 1'b0;
		hb_have     <= 1'b0;
		need_detach <= 1'b0;
		present     <= 1'b0;
		hb_timer    <= '0;
		status_due  <= 1'b1;
	end else begin
		if (kill) begin
			present     <= 1'b0;
			hb_timer    <= '0;
			hb_have     <= 1'b0;
			need_detach <= 1'b1;
		end

		if (reset) begin
			// park: withdraw both requests, ignore whatever the arbiter still delivers
			state    <= S_IDLE;
			rd_ready <= 1'b0;
			wr_ready <= 1'b0;
			if ((state == S_STAT_DATA || (state == S_STAT_REQ && wr_grant)) && wbeat_n != 4'd10)
				wr_stale <= 1'b1;
		end else case (state)
			S_IDLE: begin
				if (status_due && !wr_stale) begin
					wr_addr       <= PTX_BASE + 29'd8;
					wr_burstcnt   <= 8'd10;
					wbeat         <= '0;
					st_rd_idx     <= rd_idx;
					st_cycles     <= cap_cycles;
					st_frames     <= cap_frames;
					st_underruns  <= cap_underruns;
					st_epoch      <= epoch;
					st_aud_rd     <= aud_rd;
					st_pots       <= cap_pots;
					st_ioa        <= cap_ioa;
					st_seq        <= seq;
					wr_ready      <= 1'b1;
					state         <= S_STAT_REQ;
				end else if (magic_ok && present && aud_avail_q != 0 && !aud_fifo_almost_full) begin
					rd_addr     <= AUD_BASE + {16'd0, aud_rd[AUD_LOG2-1:0]};
					rd_burstcnt <= {3'd0, aud_n_q};
					aud_n       <= aud_n_q;
					aud_left    <= aud_n_q;
					rd_ready    <= 1'b1;
					state       <= S_AUD_REQ;
				end else if (magic_ok && present && avail_q != 0 && !fifo_almost_full) begin
					rd_addr     <= RING_BASE + {13'd0, rd_idx[RING_LOG2-1:0]};
					rd_burstcnt <= {2'd0, n_fetch_q};
					fetch_n     <= n_fetch_q;
					fetch_left  <= n_fetch_q;
					rd_ready    <= 1'b1;
					state       <= S_RING_REQ;
				end else if (poll_timer == 0) begin
					rd_addr     <= PTX_BASE;
					rd_burstcnt <= 8'd5;          // words 0..4 (4 = audio write index)
					beat        <= '0;
					rd_ready    <= 1'b1;
					state       <= S_HDR_REQ;
				end
			end

			S_HDR_REQ: if (rd_grant) begin rd_ready <= 1'b0; state <= S_HDR_DATA; end
			S_HDR_DATA: if (rd_data_valid) begin
				beat <= beat + 1'd1;
				if (beat == 3'd0)                 // word 0: magic and version
					hdr_m <= (rd_data[31:0] == MAGIC) && (rd_data[63:32] == VERSION);
				if (beat == 3'd1) begin           // word 1: ring size; header verdict
					magic_ok <= hdr_m && (rd_data[31:0] == RING_LOG2);
					if (!(hdr_m && rd_data[31:0] == RING_LOG2)) begin
						present <= 1'b0;          // no (valid) program: exited, or unsupported
						hb_have <= 1'b0;
						if (!kill) need_detach <= 1'b0;   // magic seen cleared: the ARM may re-attach
					end
				end
				if (beat == 3'd2) wr_lo <= rd_data[16:0];
				if (beat == 3'd3) begin
					// accept a heartbeat only if it is coded correctly; the first one is a
					// baseline and takeover needs a later, different one (a header left in
					// memory by a program that is gone never moves)
					if (magic_ok && !kill && !need_detach &&
					    rd_data[63:32] == (~rd_data[31:0] ^ MAGIC)) begin
						if (!hb_have) begin
							hb_have <= 1'b1;
							hb_last <= rd_data[31:0];
						end else if (rd_data[31:0] != hb_last) begin
							hb_last  <= rd_data[31:0];
							hb_timer <= HB_TIMEOUT;
							present  <= 1'b1;
						end
					end
				end
				if (beat == 3'd4) begin
					aud_wr_lo  <= rd_data[13:0];
					poll_timer <= POLL_GAP;
					state      <= S_SETTLE;
				end
			end

			S_AUD_REQ: if (rd_grant) begin rd_ready <= 1'b0; state <= S_AUD_DATA; end
			S_AUD_DATA: if (rd_data_valid) begin
				aud_fifo_wr_en   <= 1'b1;
				aud_fifo_wr_data <= rd_data;
				aud_left         <= aud_left - 1'd1;
				if (aud_left == 5'd1) begin
					aud_rd <= aud_rd + {9'd0, aud_n};
					state  <= S_SETTLE;
				end
			end

			S_RING_REQ: if (rd_grant) begin rd_ready <= 1'b0; state <= S_RING_DATA; end
			S_RING_DATA: if (rd_data_valid) begin
				fifo_wr_en   <= 1'b1;
				fifo_wr_data <= rd_data;
				fetch_left   <= fetch_left - 1'd1;
				if (fetch_left == 6'd1) begin
					rd_idx <= rd_idx + {58'd0, fetch_n};
					state  <= S_SETTLE;         // re-poll only when the ring runs dry
				end
			end

			S_STAT_REQ: if (wr_grant) begin wr_ready <= 1'b0; state <= S_STAT_DATA; end
			S_STAT_DATA: begin
				if (wr_done) begin
					status_due   <= 1'b0;
					status_timer <= STATUS_GAP;
					state        <= S_IDLE;
				end
			end

			S_SETTLE: state <= S_IDLE;       // let avail_q/n_fetch_q catch up
			default: state <= S_IDLE;
		endcase
	end
end

endmodule

`default_nettype wire
