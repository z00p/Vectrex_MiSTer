// ============================================================================
// Self-checking testbench for pitrex_ddr on the real vfb_ddr_arbiter (Verilator).
// Not part of the Quartus project. Run with sim/run.sh (old = pre-fix RTL).
//
//   +case=0  reset (video_reset) during a header read       -> reader recovers
//   +case=1  reset during a status write burst              -> reader recovers, payload intact
//   +case=2  retained header with a static valid heartbeat  -> no takeover
//   +case=3  snapshot update while a status beat is stalled -> DDRAM_DIN stable
//   +case=4  live ARM: takeover and in-order command fetch  -> positive regression
//   +case=5  (NEW only) reset while live: drop, epoch++, needs re-attach, then retakes
//   +case=6  header validation: wrong version / ring size   -> no takeover
// A global checker also flags any change of DIN/BE/ADDR while WE && BUSY.
// ============================================================================
`timescale 1ns/1ps

module tb_ptx_ddr;

localparam [28:0] BASE  = 29'h06800000;
localparam [31:0] MAGIC = 32'h58525450;
localparam [31:0] SIG   = 32'h43585450;

int tcase;
initial if (!$value$plusargs("case=%d", tcase)) tcase = 0;

logic clk = 0;
always #4 clk = ~clk;                       // 125 MHz

logic rst_sys = 0;                          // arbiter / video reset (clk_125 domain)

// ---------------------------------------------------------------- DDRAM model
logic        DDRAM_BUSY = 0;
wire  [7:0]  DDRAM_BURSTCNT;
wire  [28:0] DDRAM_ADDR;
wire         DDRAM_RD, DDRAM_WE;
wire  [63:0] DDRAM_DIN;
wire  [7:0]  DDRAM_BE;
logic [63:0] DDRAM_DOUT = 0;
logic        DDRAM_DOUT_READY = 0;

logic [63:0] mem [0:'h13000-1];
int busy_mode = 0;                          // 0 none, 1 random, 2 forced, 3 stall a status burst at beat 2
logic [31:0] lfsr = 32'hACE1ACE1;
int rd_left = 0, rd_lat = 0, wr_left = 0;
logic [28:0] rd_ptr, wr_ptr;
int n_hdr_reads = 0, n_ring_reads = 0, n_wr_bursts = 0, n_wr_beats = 0;

always @(posedge clk) begin
	lfsr <= {lfsr[30:0], lfsr[31] ^ lfsr[21] ^ lfsr[1] ^ lfsr[0]};
	DDRAM_BUSY <= (busy_mode == 2) ? 1'b1 : (busy_mode == 1) ? lfsr[3] :
	                (busy_mode == 3) ? (int'(dut.state) == 6 && int'(dut.wbeat) >= 1 && int'(dut.wbeat) <= 2) : 1'b0;
	DDRAM_DOUT_READY <= 1'b0;
	if (DDRAM_RD && !DDRAM_BUSY) begin
		rd_ptr = DDRAM_ADDR; rd_left = DDRAM_BURSTCNT; rd_lat = 6;
		if (DDRAM_ADDR == BASE) n_hdr_reads++;
		else if (DDRAM_ADDR >= BASE + 29'h200 && DDRAM_ADDR < BASE + 29'h10200) n_ring_reads++;
	end else if (rd_left > 0) begin
		if (rd_lat > 0) rd_lat--;
		else if (!(busy_mode == 1 && lfsr[7])) begin
			DDRAM_DOUT <= mem[rd_ptr - BASE];
			DDRAM_DOUT_READY <= 1'b1;
			rd_ptr++; rd_left--;
		end
	end
	if (DDRAM_WE && !DDRAM_BUSY) begin
		if (wr_left == 0) begin wr_ptr = DDRAM_ADDR; wr_left = DDRAM_BURSTCNT; n_wr_bursts++; end
		for (int b = 0; b < 8; b++)
			if (DDRAM_BE[b]) mem[wr_ptr - BASE][b*8 +: 8] = DDRAM_DIN[b*8 +: 8];
		wr_ptr++; wr_left--; n_wr_beats++;
	end
end

// stability checker: once WE && BUSY, the beat must not change until accepted
int din_viol = 0;
logic p_we = 0, p_busy = 0;
logic [63:0] p_din; logic [7:0] p_be; logic [28:0] p_addr;
always @(posedge clk) begin
	if (p_we && p_busy && DDRAM_WE && (DDRAM_DIN !== p_din || DDRAM_BE !== p_be || DDRAM_ADDR !== p_addr))
		din_viol++;
	p_we <= DDRAM_WE; p_busy <= DDRAM_BUSY; p_din <= DDRAM_DIN; p_be <= DDRAM_BE; p_addr <= DDRAM_ADDR;
end

// ----------------------------------------------------------------- arbiter
wire        ptx_read_ready, ptx_read_grant, ptx_read_data_valid;
wire [28:0] ptx_read_addr;
wire [7:0]  ptx_read_burstcnt;
wire [63:0] ptx_read_data;
wire        ptx_write_ready, ptx_write_grant, ptx_write_done, ptx_write_advance;
wire [28:0] ptx_write_addr;
wire [7:0]  ptx_write_burstcnt, ptx_write_be;
wire [63:0] ptx_write_data;
wire        reset_busy, reset_busy_d;

vfb_ddr_arbiter arb (
	.clk_sys(clk), .rst_sys(rst_sys),
	.DDRAM_BUSY(DDRAM_BUSY), .DDRAM_BURSTCNT(DDRAM_BURSTCNT), .DDRAM_ADDR(DDRAM_ADDR),
	.DDRAM_RD(DDRAM_RD), .DDRAM_WE(DDRAM_WE), .DDRAM_DIN(DDRAM_DIN), .DDRAM_BE(DDRAM_BE),
	.DDRAM_DOUT(DDRAM_DOUT), .DDRAM_DOUT_READY(DDRAM_DOUT_READY),
	.readout_ready(1'b0), .readout_grant(), .readout_addr(29'd0), .readout_burstcnt(9'd0),
	.readout_data(), .readout_data_valid(),
	.fill_ready(1'b0), .fill_grant(), .fill_addr(29'd0), .fill_burstcnt(8'd0),
	.fill_data(), .fill_data_valid(),
	.flush_ready(1'b0), .flush_grant(), .flush_done(), .flush_addr(29'd0), .flush_burstcnt(8'd0),
	.flush_din(64'd0), .flush_be(8'd0), .flush_advance(),
	.compose_read_ready(1'b0), .compose_read_grant(), .compose_read_addr(29'd0),
	.compose_read_burstcnt(8'd0), .compose_read_data(), .compose_read_data_valid(),
	.compose_write_ready(1'b0), .compose_write_grant(), .compose_write_done(),
	.compose_write_addr(29'd0), .compose_write_burstcnt(8'd0), .compose_write_data(64'd0),
	.compose_write_be(8'd0), .compose_write_advance(),
	.upload_write_ready(1'b0), .upload_write_done(), .upload_write_addr(29'd0),
	.upload_write_burstcnt(8'd0), .upload_write_data(64'd0), .upload_write_be(8'd0),
	.upload_write_advance(),
	.ptx_read_ready(ptx_read_ready), .ptx_read_grant(ptx_read_grant), .ptx_read_addr(ptx_read_addr),
	.ptx_read_burstcnt(ptx_read_burstcnt), .ptx_read_data(ptx_read_data),
	.ptx_read_data_valid(ptx_read_data_valid),
	.ptx_write_ready(ptx_write_ready), .ptx_write_grant(ptx_write_grant),
	.ptx_write_done(ptx_write_done), .ptx_write_addr(ptx_write_addr),
	.ptx_write_burstcnt(ptx_write_burstcnt), .ptx_write_data(ptx_write_data),
	.ptx_write_be(ptx_write_be), .ptx_write_advance(ptx_write_advance),
	.artwork_read_ready(1'b0), .artwork_read_grant(), .artwork_read_addr(29'd0),
	.artwork_read_burstcnt(8'd0), .artwork_read_data(), .artwork_read_data_valid(),
	.reset_busy(reset_busy), .reset_busy_d(reset_busy_d)
);

// the arbiter-coordinated client reset, as exported by vfb_top in the fixed design
logic ptx_rst_q = 1'b1;
always @(posedge clk) ptx_rst_q <= rst_sys | reset_busy_d;
// user-level takeover drop (synchronised in Vectrex.sv in the real core)
logic drop_125 = 0, drop_only = 0, boot_busy = 0;
always @(posedge clk) drop_125 <= rst_sys | drop_only;

// -------------------------------------------------------------------- DUT
logic        enable = 0;
wire         present;
wire         fifo_wr_en, aud_fifo_wr_en;
wire  [63:0] fifo_wr_data, aud_fifo_wr_data;
logic [47:0] snap_cycles = 48'd1000;
logic [15:0] snap_frames = 16'd1, snap_underruns = 16'd0;
logic [31:0] snap_pots = 32'h01020304;
logic [7:0]  snap_ioa = 8'hFF;
logic        snap_toggle = 0;
logic        fifo_full = 0;

pitrex_ddr #(.BUILD(8'd7)) dut (
	.clk(clk), .enable(enable), .present(present),
`ifdef NEW
	.reset(ptx_rst_q), .drop(drop_125), .drop_cause(7'd0), .boot_busy(boot_busy),
`endif
	.rd_ready(ptx_read_ready), .rd_grant(ptx_read_grant), .rd_addr(ptx_read_addr),
	.rd_burstcnt(ptx_read_burstcnt), .rd_data(ptx_read_data), .rd_data_valid(ptx_read_data_valid),
	.wr_ready(ptx_write_ready), .wr_grant(ptx_write_grant), .wr_done(ptx_write_done),
	.wr_addr(ptx_write_addr), .wr_burstcnt(ptx_write_burstcnt), .wr_data(ptx_write_data),
	.wr_be(ptx_write_be), .wr_advance(ptx_write_advance),
	.fifo_wr_en(fifo_wr_en), .fifo_wr_data(fifo_wr_data), .fifo_almost_full(fifo_full),
	.aud_fifo_wr_en(aud_fifo_wr_en), .aud_fifo_wr_data(aud_fifo_wr_data), .aud_fifo_almost_full(1'b0),
	.snap_cycles(snap_cycles), .snap_frames(snap_frames), .snap_underruns(snap_underruns),
	.snap_pots(snap_pots), .snap_ioa(snap_ioa), .snap_toggle(snap_toggle)
);

// captured command stream (what would enter the FIFO)
logic [63:0] got [$];
always @(posedge clk) begin
	if (fifo_wr_en) got.push_back(fifo_wr_data);
	if (!present) begin end
end
int present_rise = 0;
logic present_q = 0;
always @(posedge clk) begin present_q <= present; if (present && !present_q) present_rise++; end

// -------------------------------------------------------------- ARM model
function automatic [63:0] cmd(input int i);
	cmd = {8'h01, 16'(i % 7), 16'(16'hD000 + (i % 16)), 8'(i), 16'h0000};
endfunction

task automatic arm_header(input [31:0] ver, input [31:0] log2n, input [63:0] wr, input [31:0] hb);
	mem[0] = {ver, MAGIC};
	mem[1] = {32'd0, log2n};
	mem[2] = wr;
	mem[3] = {~hb ^ MAGIC, hb};
endtask

task automatic arm_clear_magic();
	mem[0] = 64'd0;
endtask

task automatic arm_ring(input int from, input int to);
	for (int i = from; i < to; i++) mem[32'h200 + (i % 65536)] = cmd(i);
endtask

logic        arm_run = 0;                   // heartbeat + ring producer
logic [31:0] arm_hb = 32'd100;
int          arm_wr = 0, arm_max = 0;
initial begin
	forever begin
		#10000;                             // 10 us
		if (arm_run) begin
			arm_hb = arm_hb + 1;
			if (arm_wr < arm_max) begin
				int n; n = (arm_max - arm_wr > 40) ? 40 : (arm_max - arm_wr);
				arm_ring(arm_wr, arm_wr + n);
				arm_wr += n;
			end
			mem[2] = 64'(arm_wr);
			mem[3] = {~arm_hb ^ MAGIC, arm_hb};
		end
	end
end

task automatic snap_update(input int v);
	snap_cycles = 48'(v * 100); snap_frames = 16'(v); snap_underruns = 16'(v + 1);
	snap_pots = 32'(32'hA5000000 + v); snap_ioa = 8'(v);
	snap_toggle = ~snap_toggle;
endtask

// periodic snapshot updates like the player (every few us in sim)
int snap_n = 0;
initial forever begin #7000; snap_n++; snap_update(snap_n); end

// ------------------------------------------------------------------ checks
int fails = 0;
task automatic check(input bit ok, input string msg);
	if (!ok) begin fails++; $display("  check FAILED: %s", msg); end
	else $display("  check ok:     %s", msg);
endtask

function automatic int st(); st = int'(dut.state); endfunction

task automatic wait_clk(input int n); repeat (n) @(posedge clk); endtask

task automatic wait_state(input int s, input int tmo_clk);
	int t; t = 0;
	while (st() != s && t < tmo_clk) begin @(posedge clk); t++; end
endtask

task automatic pulse_reset(input int clks);
	rst_sys = 1; wait_clk(clks); rst_sys = 0;
endtask

// has the reader returned to normal operation? (headers polled and status updated)
task automatic check_recovered(input string who, input int window_clk);
	int h0, w0; logic [63:0] seq0;
	h0 = n_hdr_reads; w0 = n_wr_bursts; seq0 = mem[17];
	wait_clk(window_clk);
	check(n_hdr_reads >= h0 + 3, {who, ": header polling resumed"});
	check(n_wr_bursts >= w0 + 1 && mem[17] != seq0, {who, ": status writes resumed"});
endtask

initial begin
	for (int i = 0; i < 'h13000; i++) mem[i] = 64'd0;
	wait_clk(10);
	$display("CASE %0d", tcase);
	case (tcase)
	0: begin
		arm_max = 0; arm_header(0, 16, 0, 100); arm_run = 1;
		enable = 1;
		wait_clk(2000);
		wait_state(2, 20000);                                  // S_HDR_DATA
		wait_clk(2);
		check(st() == 2, "reset lands in S_HDR_DATA");
		pulse_reset(12);
		check_recovered("after reset in header read", 70000);
		check(st() != 2 || n_hdr_reads > 0, "not stuck in S_HDR_DATA");
	end
	1: begin
		arm_max = 0; arm_header(0, 16, 0, 100); arm_run = 1;
		enable = 1;
		wait_clk(2000);
		for (int i = 8; i < 18; i++) mem[i] = 64'd0;
		// stall the next status burst mid-way
		while (!(st() == 6 && int'(dut.wbeat) >= 2)) @(posedge clk);
		busy_mode = 2;
		wait_clk(6);
		rst_sys = 1;
		wait_clk(12);
		busy_mode = 0;                                         // memory frees up, arbiter drains
		wait_clk(200);                                         // ...while reset is still held
		check(mem[11][31:0] == SIG, "interrupted burst drained with intact payload (word 11 = PTXC)");
		check(mem[11][39:32] == 8'd7, "word 11 build = 7");
		rst_sys = 0;
		check_recovered("after reset in status write", 70000);
	end
	2: begin
		arm_max = 0; arm_header(0, 16, 8, 77); arm_run = 0;     // retained, never changes
		enable = 1;
		wait_clk(40000);
		check(present_rise == 0, "static valid heartbeat does not assert present");
		check(n_ring_reads == 0, "no ring fetches from a stale header");
	end
	3: begin
		arm_max = 0; arm_header(0, 16, 0, 100); arm_run = 1;
		enable = 1;
		wait_clk(2000);
		busy_mode = 3;                                         // stalls the next status burst at beat 2
		while (!(st() == 6 && int'(dut.wbeat) == 2 && DDRAM_BUSY && DDRAM_WE)) @(posedge clk);
		wait_clk(4);
		snap_update(999);                                      // new snapshot arrives while the beat is stalled
		wait_clk(300);                                         // > tog_sync latency
		busy_mode = 0;
		wait_clk(4000);
	end
	4: begin
		arm_max = 300; arm_header(0, 16, 0, 100); arm_run = 1;
		busy_mode = 1;                                         // random wait states
		enable = 1;
		wait_clk(80000);                                       // past the second status write
		check(present_rise >= 1, "takeover on a moving heartbeat");
		check(got.size() == 300, $sformatf("all 300 commands fetched (got %0d)", got.size()));
		begin
			bit ordered; ordered = 1;
			for (int i = 0; i < got.size() && i < 300; i++) if (got[i] !== cmd(i)) ordered = 0;
			check(ordered, "commands in order, none lost or duplicated");
		end
		check(mem[8] == 64'd300, $sformatf("rd_idx published = 300 (got %0d)", mem[8]));
	end
`ifdef NEW
	5: begin
		arm_max = 100; arm_header(0, 16, 0, 100); arm_run = 1;
		enable = 1;
		wait_clk(20000);
		check(present, "live before reset");
		check(mem[11][63:48] == 16'd0, "epoch 0 before reset");
		pulse_reset(10);
		wait_clk(2000);
		check(!present, "takeover dropped by reset");
		wait_clk(70000);
		check(mem[11][63:48] != 16'd0, "epoch advanced in word 11");                                       // ARM keeps bumping the heartbeat
		check(!present, "no silent re-takeover while magic stays set");
		// ARM re-attach: clear magic, hold it cleared, then rebuild the header at the core's rd_idx
		arm_run = 0; arm_clear_magic(); wait_clk(2500);        // 20 us cleared
		arm_wr = int'(mem[8]); arm_max = arm_wr + 50;
		arm_header(0, 16, mem[8], 5000); arm_hb = 5000; arm_run = 1;
		wait_clk(20000);
		check(present, "re-attach after magic clear retakes over");
	end
	7: begin                                                   // machine-only reset (menu / cartridge load)
		arm_max = 100; arm_header(0, 16, 0, 100); arm_run = 1;
		enable = 1;
		wait_clk(20000);
		check(present, "live before menu reset");
		drop_only = 1; wait_clk(20); drop_only = 0;
		wait_clk(2000);
		check(!present, "takeover dropped by a machine-only reset");
		check_recovered("FSM undisturbed by a machine-only reset", 70000);
		check(mem[11][63:48] != 16'd0, "epoch advanced in word 11");
		check(!present, "still detached while magic stays set");
	end
`endif
	6: begin
		arm_max = 0; arm_header(1, 16, 8, 100); arm_run = 1;   // version 1
		enable = 1;
		wait_clk(20000);
		check(present_rise == 0, "version != 0 is not taken over");
		arm_run = 0; arm_header(0, 15, 8, 100); arm_hb = 100; arm_run = 1;
		wait_clk(20000);
		check(present_rise == 0, "ring size != 65536 is not taken over");
		arm_run = 0; arm_header(0, 16, 8, 100); arm_hb = 100; arm_run = 1;
		arm_max = 0;
		wait_clk(20000);
		check(present_rise == 1, "valid header is taken over");
	end
	default: begin end
	endcase
	check(din_viol == 0, $sformatf("DDRAM_DIN/BE/ADDR stable while WE && BUSY (%0d violations)", din_viol));
	$display("RESULT case=%0d %s", tcase, fails == 0 ? "PASS" : "FAIL");
	$finish;
end

initial begin #3000000; $display("RESULT case=%0d TIMEOUT", tcase); $finish; end

endmodule
