// ============================================================================
// Starvation testbench (B8): real pitrex_ddr + real pitrex_player + real
// vfb_async_fifo + real vfb_ddr_arbiter, with a mock composition client that
// hammers the arbiter with 16-beat read + 16-beat write bursts for +win=<us> out
// of every +period=<us> (like a composition pass), a DDR model with +lat=<clk>
// read latency (+hold=1: both composition requests held high for the whole window = worst case, PiTrex
// can only win if it has higher priority), and the ring filled from a recorded PiTrex stream (+hex=file,
// +n=commands; sim/run.sh builds it from build/calibrate.ptxs). The ARM is
// modelled as always ahead (worst case for the reader, best case for the ARM).
// Prints one "trial" line: underruns after start-up, pause episodes, longest pause.
// Not part of the Quartus project.
// ============================================================================
`timescale 1ns/1ps

module tb_ptx_starve;

localparam [28:0] BASE  = 29'h06800000;
localparam [31:0] MAGIC = 32'h58525450;

int off_us = 10000, hold_rdy = 0, win_us = 0, period_us = 20000, lat = 20, run_us = 100000, ncmd = 10000;
string hexfile;
initial begin
	void'($value$plusargs("win=%d", win_us));
	void'($value$plusargs("hold=%d", hold_rdy));
	void'($value$plusargs("off=%d", off_us));
	void'($value$plusargs("period=%d", period_us));
	void'($value$plusargs("lat=%d", lat));
	void'($value$plusargs("run_us=%d", run_us));
	void'($value$plusargs("n=%d", ncmd));
	if (!$value$plusargs("hex=%s", hexfile)) hexfile = "/tmp/ptxsim/cal.hex";
end

logic clk = 0, clk24 = 0;
always #4 clk = ~clk;                      // 125 MHz
always #20.8333 clk24 = ~clk24;            // 24 MHz

// ------------------------------------------------------------------ DDR model
logic        DDRAM_BUSY = 0;
wire  [7:0]  DDRAM_BURSTCNT;
wire  [28:0] DDRAM_ADDR;
wire         DDRAM_RD, DDRAM_WE;
wire  [63:0] DDRAM_DIN;
wire  [7:0]  DDRAM_BE;
logic [63:0] DDRAM_DOUT = 0;
logic        DDRAM_DOUT_READY = 0;
logic [63:0] mem [0:'h13000-1];
int rd_left = 0, rd_lat = 0, wr_left = 0;
logic [28:0] rd_ptr, wr_ptr;
always @(posedge clk) begin
	DDRAM_DOUT_READY <= 1'b0;
	if (DDRAM_RD && !DDRAM_BUSY) begin
		rd_ptr = DDRAM_ADDR; rd_left = DDRAM_BURSTCNT; rd_lat = lat;
	end else if (rd_left > 0) begin
		if (rd_lat > 0) rd_lat--;
		else begin
			DDRAM_DOUT <= (rd_ptr >= BASE && rd_ptr - BASE < 'h13000) ? mem[rd_ptr - BASE] : 64'd0;
			DDRAM_DOUT_READY <= 1'b1;
			rd_ptr++; rd_left--;
		end
	end
	if (DDRAM_WE && !DDRAM_BUSY) begin
		if (wr_left == 0) begin wr_ptr = DDRAM_ADDR; wr_left = DDRAM_BURSTCNT; end
		if (wr_ptr >= BASE && wr_ptr - BASE < 'h13000)
			for (int b = 0; b < 8; b++)
				if (DDRAM_BE[b]) mem[wr_ptr - BASE][b*8 +: 8] = DDRAM_DIN[b*8 +: 8];
		wr_ptr++; wr_left--;
	end
end

// ------------------------------------------------------------ composition mock
real now_us = 0;
always @(posedge clk) now_us <= $realtime / 1000.0;
real ph_us; always @(posedge clk) ph_us = now_us - $floor(now_us / period_us) * period_us;
wire comp_window = (win_us > 0) && (ph_us >= off_us) && (ph_us < off_us + win_us);

wire        compose_read_grant, compose_read_data_valid, compose_write_grant, compose_write_done, compose_write_advance;
logic       comp_rd_ready = 0, comp_wr_ready = 0;
int         comp_state = 0, comp_cnt = 0;
always @(posedge clk) begin
	case (comp_state)
	0: if (comp_window) begin comp_rd_ready <= 1; comp_state <= 1; end
	1: if (compose_read_grant) begin comp_rd_ready <= 0; comp_cnt <= 0; comp_state <= 2; end
	2: if (compose_read_data_valid) begin
		comp_cnt <= comp_cnt + 1;
		if (comp_cnt == 15) begin comp_wr_ready <= 1; comp_state <= 3; end
	end
	3: if (compose_write_grant) begin comp_wr_ready <= 0; comp_state <= 4; end
	4: if (compose_write_done) comp_state <= 0;
	default: comp_state <= 0;
	endcase
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
import vfb_layout_pkg::*;

vfb_ddr_arbiter arb (
	.clk_sys(clk), .rst_sys(1'b0),
	.DDRAM_BUSY(DDRAM_BUSY), .DDRAM_BURSTCNT(DDRAM_BURSTCNT), .DDRAM_ADDR(DDRAM_ADDR),
	.DDRAM_RD(DDRAM_RD), .DDRAM_WE(DDRAM_WE), .DDRAM_DIN(DDRAM_DIN), .DDRAM_BE(DDRAM_BE),
	.DDRAM_DOUT(DDRAM_DOUT), .DDRAM_DOUT_READY(DDRAM_DOUT_READY),
	.readout_ready(1'b0), .readout_grant(), .readout_addr(29'd0), .readout_burstcnt(9'd0),
	.readout_data(), .readout_data_valid(),
	.fill_ready(1'b0), .fill_grant(), .fill_addr(29'd0), .fill_burstcnt(8'd0),
	.fill_data(), .fill_data_valid(),
	.flush_ready(1'b0), .flush_grant(), .flush_done(), .flush_addr(29'd0), .flush_burstcnt(8'd0),
	.flush_din(64'd0), .flush_be(8'd0), .flush_advance(),
	.compose_read_ready(hold_rdy ? comp_window : comp_rd_ready), .compose_read_grant(compose_read_grant),
	.compose_read_addr(VFB_FRAMEBUFFER_BASE), .compose_read_burstcnt(8'd16),
	.compose_read_data(), .compose_read_data_valid(compose_read_data_valid),
	.compose_write_ready(hold_rdy ? comp_window : comp_wr_ready), .compose_write_grant(compose_write_grant),
	.compose_write_done(compose_write_done), .compose_write_addr(VFB_FRAMEBUFFER_BASE),
	.compose_write_burstcnt(8'd16), .compose_write_data(64'd0), .compose_write_be(8'hFF),
	.compose_write_advance(compose_write_advance),
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
logic ptx_rst_q = 1'b1;
always @(posedge clk) ptx_rst_q <= reset_busy_d;

// -------------------------------------------------- reader, FIFO, player
wire         present;
wire         fifo_wr_en; wire [63:0] fifo_wr_data;
wire         fifo_almost_full;
logic [1:0]  present_sys = 0;
always @(posedge clk24) present_sys <= {present_sys[0], present};
wire active = present_sys[1];
logic       mode_q = 0; logic [4:0] cnt = 0;
always @(posedge clk24) begin
	mode_q <= active;
	if (mode_q != active) cnt <= 5'd31; else if (cnt != 0) cnt <= cnt - 1'd1;
end
wire entry_busy = (mode_q != active) || (cnt != 0);

wire [47:0] snap_cycles; wire [15:0] snap_frames, snap_underruns; wire [31:0] snap_pots; wire [7:0] snap_ioa;
wire snap_toggle;

pitrex_ddr #(.BUILD(8'd8)) dut (
	.clk(clk), .enable(1'b1), .present(present), .reset(ptx_rst_q), .drop(1'b0), .drop_cause(7'd0), .boot_busy(1'b0),
	.rd_ready(ptx_read_ready), .rd_grant(ptx_read_grant), .rd_addr(ptx_read_addr),
	.rd_burstcnt(ptx_read_burstcnt), .rd_data(ptx_read_data), .rd_data_valid(ptx_read_data_valid),
	.wr_ready(ptx_write_ready), .wr_grant(ptx_write_grant), .wr_done(ptx_write_done),
	.wr_addr(ptx_write_addr), .wr_burstcnt(ptx_write_burstcnt), .wr_data(ptx_write_data),
	.wr_be(ptx_write_be), .wr_advance(ptx_write_advance),
	.fifo_wr_en(fifo_wr_en), .fifo_wr_data(fifo_wr_data), .fifo_almost_full(fifo_almost_full),
	.aud_fifo_wr_en(), .aud_fifo_wr_data(), .aud_fifo_almost_full(1'b0),
	.snap_cycles(snap_cycles), .snap_frames(snap_frames), .snap_underruns(snap_underruns),
	.snap_pots(32'd0), .snap_ioa(8'hFF), .snap_toggle(snap_toggle)
);

wire f_rd_en, f_rd_valid, f_rd_empty; wire [63:0] f_rd_data;
vfb_async_fifo #(.WIDTH(64), .DEPTH(128), .ALMOST_FULL_MARGIN(40)) fifo (
	.wr_clk(clk), .wr_reset(!present), .wr_en(fifo_wr_en), .wr_data(fifo_wr_data),
	.wr_full(), .wr_almost_full(fifo_almost_full),
	.rd_clk(clk24), .rd_reset(!active), .rd_en(f_rd_en), .rd_data(f_rd_data),
	.rd_valid(f_rd_valid), .rd_empty(f_rd_empty));

// machine pause model: the E-cycle phase freezes while stalled (B8 vectrex.vhd)
logic [3:0] phase = 0;
wire stall;
always @(posedge clk24) if (!stall) phase <= phase + 1'd1;
wire e_fall = (phase == 4'd0) && !stall;

wire [15:0] bus_addr; wire [7:0] bus_do; wire bus_rw;
pitrex_player player (
	.clk(clk24), .enable(active), .run(active && !entry_busy), .e_fall(e_fall),
	.fifo_rd_en(f_rd_en), .fifo_rd_data(f_rd_data), .fifo_rd_valid(f_rd_valid), .fifo_rd_empty(f_rd_empty),
	.bus_addr(bus_addr), .bus_do(bus_do), .bus_rw(bus_rw), .stall(stall),
	.pots(32'd0), .psg_ioa(8'hFF),
	.snap_cycles(snap_cycles), .snap_frames(snap_frames), .snap_underruns(snap_underruns),
	.snap_pots(snap_pots), .snap_ioa(snap_ioa), .snap_toggle(snap_toggle)
);

// ------------------------------------------------------------------ ARM mock
logic [31:0] hb = 32'd100;
initial begin
	$readmemh(hexfile, mem, 'h200);
	mem[0] = 64'd0; mem[1] = 64'd16; mem[2] = 64'(ncmd); mem[3] = {~hb ^ MAGIC, hb};
	#50000; mem[0] = {32'd0, MAGIC};           // the ARM attaches after seeing the magic cleared (B7)
	forever begin #10000; hb = hb + 1; mem[3] = {~hb ^ MAGIC, hb}; end
end

// ------------------------------------------------------------------ measure
int under0 = 0, stall_n = 0, stall_clk = 0, cur_stall = 0, max_stall = 0, started = 0;
logic stall_q = 0;
real t_start = 0;
always @(posedge clk24) begin
	stall_q <= stall;
	if (active && !entry_busy && !started) begin started <= 1; t_start = $realtime; end
	if (started && ($realtime - t_start) > 2000000.0) begin   // 2 ms after the machine started
		if (under0 == 0) under0 <= int'(snap_underruns) + 1;
		if (stall) begin
			stall_clk++; cur_stall++;
			if (cur_stall > max_stall) max_stall = cur_stall;
			if (!stall_q) stall_n++;
		end else cur_stall = 0;
	end
end

initial begin
	#(run_us * 1000.0);
	$display("trial win=%0d/%0d lat=%0d : underruns=%0d pauses=%0d paused=%0d us longest=%0.1f us frames=%0d",
		win_us, period_us, lat, int'(snap_underruns) + 1 - under0, stall_n,
		int'(stall_clk * 41.667 / 1000.0), max_stall * 41.667 / 1000.0, snap_frames);
	$display("dbg present=%0d active=%0d entry_busy=%0d rd_idx=%0d magic_ok=%0d state=%0d", present, active, entry_busy, dut.rd_idx, dut.magic_ok, int'(dut.state));
	$finish;
end

endmodule
