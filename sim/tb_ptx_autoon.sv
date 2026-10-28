// ============================================================================
// pitrex_autoon (B10): a completed download for the launcher slot (ioctl index 3) turns
// PiTrex mode on, stickily; other indexes, an unfinished download and the OSD Reset do not
// set or clear it. Wired as in Vectrex.sv: mode = option | auto feeds pitrex_reset, so the
// launcher download itself and the following status[0] pulses must not reset or drop, and a
// pick with the option Off and no previous pick leaves the machine a normal Vectrex.
// Not part of the Quartus project.
// ============================================================================
`timescale 1ns/1ps
module tb_ptx_autoon;
logic clk = 0; always #20.8333 clk = ~clk;

logic [15:0] idx = 0; logic dl = 0, opt = 0, active = 0, s0 = 0, menu = 0, rom_dl = 0;
wire auto_on; wire video_reset, user_reset; wire [6:0] cause;
wire mode_on = opt | auto_on;

pitrex_autoon a (.clk(clk), .ioctl_download(dl), .ioctl_index(idx), .auto_on(auto_on));
pitrex_reset #(.HOLD(2000), .SETTLE(6000)) r (.clk(clk), .active(active), .ptx_mode(mode_on), .ioctl_download(dl), .rom_download(rom_dl),
	.pll_unlocked(1'b0), .fw_reset(1'b0), .status0(s0), .button_reset(1'b0), .menu_reset(menu),
	.save_hold(1'b0), .delayed_reset(1'b0), .bios_reset(1'b0),
	.video_reset(video_reset), .user_reset(user_reset), .cause(cause));

int fails = 0;
task automatic check(input bit ok, input string msg);
	if (!ok) begin fails++; $display("  check FAILED: %s", msg); end else $display("  check ok:     %s", msg);
endtask
task automatic w(input int n); repeat (n) @(posedge clk); endtask
// a download of file slot i (rom_download as Vectrex.sv decodes it)
task automatic download(input [15:0] i, input int len);
	idx = i; rom_dl = (i[4:0] <= 1) && (i[9:8] == 0) && (i != 16'h0080);
	w(3); dl = 1; w(len); dl = 0; w(3); rom_dl = 0;
endtask
int seen_user = 0, seen_video = 0;
always @(posedge user_reset) seen_user++;
always @(posedge video_reset) seen_video++;

initial begin
	w(10);
	check(!auto_on, "power-on: flag clear");
	download(16'h0000, 50); download(16'h0001, 50); download(16'h0002, 50); download(16'h0004, 50);
	download(16'h0005, 50); download(16'h001F, 50);
	check(!auto_on, "downloads for index 0, 1, 2, 4, 5 and 0x1F do not set it");
	// unfinished launcher download: flag only changes when it completes
	idx = 16'd3; w(3); dl = 1; w(40);
	check(!auto_on, "launcher download still running: flag not yet set");
	dl = 0; w(3);
	check(auto_on, "launcher download (index 3) complete: flag set, option still Off");
	download(16'h0043, 50); download(16'h0103, 50);
	check(auto_on, "extension-select index 0x43 and 0x103 downloads: flag stays set");
	// sticky across everything else
	menu = 1; w(5); menu = 0; w(10);
	check(auto_on, "OSD Reset (status[7]) does not clear it");
	download(16'h0000, 100);
	check(auto_on, "a cartridge load does not clear it");
	opt = 1; w(5); opt = 0; w(5);
	check(auto_on, "toggling the option does not clear it");
	// wiring with pitrex_reset: the flag acts as the mode option
	seen_user = 0; seen_video = 0; active = 1; w(100);
	s0 = 1; w(5); s0 = 0; w(50); download(16'd3, 100); s0 = 1; w(20); s0 = 0; w(100);
	check(seen_user == 0 && seen_video == 0, "flag set, option Off: launcher download + status[0] pulses mask like mode On");
	active = 0; w(20000); seen_user = 0; seen_video = 0;
	s0 = 1; w(20); s0 = 0; w(10);
	check(seen_video > 0, "status[0] outside a download is a real reset again");
	$display("RESULT autoon %s", fails == 0 ? "PASS" : "FAIL");
	$finish;
end
initial begin #400000000; $display("RESULT autoon TIMEOUT"); $finish; end
endmodule
