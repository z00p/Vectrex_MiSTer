// ============================================================================
// PiTrex mode: which resets are real (B8).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The MiSTer framework may pulse status[0] around a file download. With PiTrex mode
// on, MGL menu entries load the core and then, 1-2 s later, download the overlay
// (ioctl index 2) and the .ptx launcher file (index 3) while the program is already
// streaming; those downloads must not reset the machine or drop the takeover.
// A real cartridge/BIOS load (rom_download: index 0/1), the OSD Reset item, the user
// reset button, a framework RESET, PLL loss, save hold, delayed reset and a BIOS change still do.
//
// Only the framework's status[0] is masked, and only in PiTrex mode: while a non-ROM
// download is running, for HOLD clocks after it, and for SETTLE clocks after a takeover
// begins (the framework may pulse status[0] just before a download starts, and the
// program can attach before the MGL's delayed file loads). With PiTrex mode off nothing changes.
//
// cause[] (latched when the drop level rises, reported in status word 11 bits 47..40
// by pitrex_ddr, bit 7 there = arbiter client reset):
//   0 video_reset  1 status[0] seen (masked or not)  2 OSD Reset  3 rom_download
//   4 save_hold    5 delayed_reset                   6 bios change
// ============================================================================

`default_nettype none

module pitrex_reset #(
	parameter integer HOLD   = 12_000_000,       // 0.5 s at 24 MHz
	parameter integer SETTLE = 96_000_000        // 4 s
) (
	input  wire clk,
	input  wire active,                          // a PiTrex takeover is in force (clk domain)
	input  wire ptx_mode,                        // PiTrex Mode option (status[36])
	input  wire ioctl_download,
	input  wire rom_download,                    // index 0/1: a real cartridge or BIOS load
	input  wire pll_unlocked,
	input  wire fw_reset,                        // RESET from the framework
	input  wire status0,                         // status[0]
	input  wire button_reset,                    // buttons[1]
	input  wire menu_reset,                      // status[7]
	input  wire save_hold,
	input  wire delayed_reset,
	input  wire bios_reset,

	output wire video_reset,                     // replaces the old video_reset
	output wire user_reset,                      // level that drops the PiTrex takeover
	output logic [6:0] cause = '0
);

logic [24:0] hold = '0;
wire dl_other = ioctl_download && !rom_download;
always_ff @(posedge clk) begin
	if (dl_other)          hold <= HOLD[24:0];
	else if (hold != 25'd0) hold <= hold - 1'd1;
end
logic [26:0] age = '0;
logic        active_q = 1'b0;
always_ff @(posedge clk) begin
	active_q <= active;
	if (active && !active_q)  age <= SETTLE[26:0];
	else if (age != 27'd0)    age <= age - 1'd1;
end
wire mask0 = ptx_mode && (dl_other || hold != 25'd0 || age != 27'd0);

assign video_reset = pll_unlocked | fw_reset | (status0 && !mask0) | button_reset;
assign user_reset  = video_reset | menu_reset | rom_download | save_hold | delayed_reset | bios_reset;

logic user_q = 1'b0;
always_ff @(posedge clk) begin
	user_q <= user_reset;
	if (user_reset && !user_q)
		cause <= {bios_reset, delayed_reset, save_hold, rom_download, menu_reset, status0, video_reset};
end

endmodule

`default_nettype wire
