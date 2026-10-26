// ============================================================================
// pitrex_reset (B8): downloads of overlay (index 2) / launcher (index 3) files, with the
// framework pulsing status[0] around them, must not reset or drop in PiTrex mode; real
// cartridge loads, OSD reset, button reset, framework RESET, and status[0] outside a
// download still must. Not part of the Quartus project.
// ============================================================================
`timescale 1ns/1ps
module tb_ptx_reset;
logic clk = 0; always #20.8333 clk = ~clk;

logic active = 0, mode = 1, dl = 0, rom_dl = 0, pll_bad = 0, fw = 0, s0 = 0, btn = 0, menu = 0, hold = 0, dly = 0, bios = 0;
wire video_reset, user_reset; wire [6:0] cause;

pitrex_reset #(.HOLD(2000), .SETTLE(6000)) dut (.clk(clk), .active(active), .ptx_mode(mode), .ioctl_download(dl), .rom_download(rom_dl),
	.pll_unlocked(pll_bad), .fw_reset(fw), .status0(s0), .button_reset(btn), .menu_reset(menu),
	.save_hold(hold), .delayed_reset(dly), .bios_reset(bios),
	.video_reset(video_reset), .user_reset(user_reset), .cause(cause));

int fails = 0;
task automatic check(input bit ok, input string msg);
	if (!ok) begin fails++; $display("  check FAILED: %s", msg); end else $display("  check ok:     %s", msg);
endtask
task automatic w(input int n); repeat (n) @(posedge clk); endtask
// run a scenario; returns whether user_reset or video_reset ever rose
int seen_user = 0, seen_video = 0;
always @(posedge user_reset) seen_user++;
always @(posedge video_reset) seen_video++;
task automatic clear(); w(5000); seen_user = 0; seen_video = 0; dl = 0; rom_dl = 0; s0 = 0; w(3000); seen_user = 0; seen_video = 0; endtask

initial begin
	w(10); clear();
	// overlay download (index 2) with status[0] pulsed during and just after
	dl = 1; w(5); s0 = 1; w(100); s0 = 0; w(500); s0 = 1; w(50); s0 = 0; dl = 0; w(200); s0 = 1; w(20); s0 = 0; w(100);
	check(!seen_user && !seen_video, "index 2/3 download with status[0] pulses: no reset, no drop");
	clear();
	// status[0] pulse just before a download starts, program freshly attached: masked
	active = 1; w(100); s0 = 1; w(5); s0 = 0; w(50); dl = 1; w(100); dl = 0; w(100);
	check(!seen_user && !seen_video, "status[0] pulse shortly after takeover/before a download: masked");
	w(8000); seen_user = 0; seen_video = 0;
	// status[0] reset long after the hold: real
	s0 = 1; w(20); s0 = 0; w(10);
	check(seen_user && seen_video, "status[0] outside a download resets and drops");
	check(cause[1], "cause reports status[0]");
	clear();
	// real cartridge load
	dl = 1; rom_dl = 1; w(100); dl = 0; rom_dl = 0; w(10);
	check(seen_user && seen_video == 0, "rom_download (index 0/1) drops the takeover");
	check(cause[3], "cause reports rom_download");
	clear();
	menu = 1; w(5); menu = 0; w(5);
	check(seen_user, "OSD Reset (status[7]) drops"); check(cause[2], "cause reports OSD reset");
	clear();
	btn = 1; w(5); btn = 0; w(5);
	check(seen_user && seen_video, "reset button drops");
	clear();
	fw = 1; w(5); fw = 0; w(5);
	check(seen_user && seen_video, "framework RESET drops");
	clear();
	// PiTrex mode off: nothing masked
	mode = 0; dl = 1; s0 = 1; w(10); s0 = 0; dl = 0; w(10);
	check(seen_user && seen_video, "PiTrex mode off: status[0] always counts");
	mode = 1; clear();
	// download pulse while a real reset (button) is held: still drops
	dl = 1; btn = 1; w(10); btn = 0; dl = 0; w(10);
	check(seen_user && seen_video, "button reset during a download still drops");
	$display("RESULT reset %s", fails == 0 ? "PASS" : "FAIL");
	$finish;
end
initial begin #400000000; $display("RESULT reset TIMEOUT"); $finish; end
endmodule
