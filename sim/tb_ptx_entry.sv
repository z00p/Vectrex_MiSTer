// ============================================================================
// Entry-ordering testbench: real pitrex_player + real vfb_async_fifo, with a
// small mock of the machine (entry-reset counter equations copied from
// Vectrex.sv, a two-register VIA that ignores writes during reset, a free-running
// E-clock enable). Sweeps the arrival time of the first commands against the
// moment the session goes active. Not part of the Quartus project.
// ============================================================================
`timescale 1ns/1ps

module tb_ptx_entry;

logic clk24 = 0, clk125 = 0;
always #20.8333 clk24 = ~clk24;
always #4 clk125 = ~clk125;

logic present_125 = 0;
logic [1:0] present_sys = 0;
always @(posedge clk24) present_sys <= {present_sys[0], present_125};
wire active = present_sys[1];               // ptx_enable assumed on

// --- production entry-reset equations (Vectrex.sv)
logic       ptx_mode_q = 0;
logic [4:0] cnt = 0;
always @(posedge clk24) begin
	ptx_mode_q <= active;
	if (ptx_mode_q != active) cnt <= 5'd31;
	else if (cnt != 0) cnt <= cnt - 1'd1;
end
wire mode_reset = (cnt != 0);
wire entry_busy = (ptx_mode_q != active) || mode_reset;

// --- E clock enable (paused by the player's stall, like the machine's pause)
logic [3:0] phase = 0;
wire        stall;
always @(posedge clk24) phase <= phase + 1'd1;
wire e_fall = (phase == 4'd0) && !stall;

// --- FIFO
logic        f_wr_en = 0; logic [63:0] f_wr_data = 0;
wire         f_rd_en, f_rd_valid, f_rd_empty; wire [63:0] f_rd_data;
vfb_async_fifo #(.WIDTH(64), .DEPTH(128), .ALMOST_FULL_MARGIN(40)) fifo (
	.wr_clk(clk125), .wr_reset(!present_125), .wr_en(f_wr_en), .wr_data(f_wr_data),
	.wr_full(), .wr_almost_full(),
	.rd_clk(clk24), .rd_reset(!active), .rd_en(f_rd_en), .rd_data(f_rd_data),
	.rd_valid(f_rd_valid), .rd_empty(f_rd_empty));

// --- player
wire [15:0] bus_addr; wire [7:0] bus_do; wire bus_rw;
wire [47:0] snap_cycles; wire [15:0] snap_frames, snap_underruns; wire [31:0] snap_pots; wire [7:0] snap_ioa;
wire snap_toggle;
pitrex_player player (
	.clk(clk24), .enable(active),
`ifdef NEW
	.run(active && !entry_busy),
`endif
	.e_fall(e_fall),
	.fifo_rd_en(f_rd_en), .fifo_rd_data(f_rd_data), .fifo_rd_valid(f_rd_valid), .fifo_rd_empty(f_rd_empty),
	.bus_addr(bus_addr), .bus_do(bus_do), .bus_rw(bus_rw), .stall(stall),
	.pots(32'd0), .psg_ioa(8'hFF),
	.snap_cycles(snap_cycles), .snap_frames(snap_frames), .snap_underruns(snap_underruns),
	.snap_pots(snap_pots), .snap_ioa(snap_ioa), .snap_toggle(snap_toggle));

// --- mock VIA: DDRA ($D003) and DDRB ($D002); ignores bus writes while in reset
logic [7:0] ddra = 0, ddrb = 0;
int writes_in_reset = 0;
always @(posedge clk24) begin
	if (mode_reset) begin ddra <= 0; ddrb <= 0; end
	if (e_fall && !bus_rw && !mode_reset) begin
		if (bus_addr == 16'hD003) ddra <= bus_do;
		if (bus_addr == 16'hD002) ddrb <= bus_do;
	end
	if (e_fall && !bus_rw && mode_reset) writes_in_reset++;
end

int early_pop = 0;                          // pops while the entry reset is still running
always @(posedge clk24) if (f_rd_en && active && entry_busy) early_pop++;

function automatic [63:0] mk(input [7:0] op, input [15:0] delta, input [15:0] a, input [7:0] d);
	mk = {op, delta, a, d, 16'h0};
endfunction

task automatic push(input [63:0] w);
	@(posedge clk125); f_wr_en <= 1; f_wr_data <= w; @(posedge clk125); f_wr_en <= 0;
endtask

int pass = 0, fail = 0, lost = 0, popped_early_trials = 0;

initial begin
	#200;
	for (int k = 0; k < 16; k++) begin
		int e0; e0 = early_pop;
		present_125 = 1;                            // session becomes live (header accepted)
		#(100 + 60 * k);                            // first commands arrive this long after
		push(mk(8'h7F, 0, 0, 0));                   // RESET_SYNC
		push(mk(8'h01, 0, 16'hD003, 8'hA5));        // DDRA = A5
		push(mk(8'h01, 0, 16'hD002, 8'h5A));        // DDRB = 5A
		push(mk(8'h02, 200, 0, 0));                 // WAIT
		#4000;
		if (ddra == 8'hA5 && ddrb == 8'h5A) pass++;
		else begin fail++; if (ddra != 8'hA5 && ddrb == 8'h5A) lost++; end
		if (early_pop != e0) popped_early_trials++;
		$display("  trial %2d (first cmd +%0d ns): DDRA=%02h DDRB=%02h %s", k, 100 + 60 * k, ddra, ddrb,
			(ddra == 8'hA5 && ddrb == 8'h5A) ? "ok" : "LOST");
		present_125 = 0;                            // program ends
		#3000;
	end
	$display("  check %s: %0d/16 trials kept both configuration writes (%0d lost the first; %0d popped during entry reset)",
		fail == 0 ? "ok:    " : "FAILED:", pass, lost, popped_early_trials);
`ifdef NEW
	if (popped_early_trials != 0) fail++;
`endif
	$display("RESULT entry %s", fail == 0 ? "PASS" : "FAIL");
	$finish;
end

initial begin #2000000; $display("RESULT entry TIMEOUT"); $finish; end
endmodule
