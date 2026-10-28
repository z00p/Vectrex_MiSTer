// ============================================================================
// PiTrex mode: picking a file in "Load UVMC2 / PiTrex" turns PiTrex mode on (B10).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A core loaded straight from _Unstable runs as VECTREX with VECTREX_v1.CFG, where the
// PiTrex Mode option is deliberately Off, so a .ptx/.um2 picked from the OSD would just
// wait for a takeover that can never start. A completed download for the launcher slot
// (ioctl index 3: the OSD's F3 entry and the MGL launchers' <file index="3">) therefore
// sets a sticky flag; Vectrex.sv ORs it with status[36] wherever the PiTrex enable is used,
// so it goes through the same entry path as switching the option on (same machine reset,
// same takeover rules).
//
// Sticky: only a core (re)load / power-on clears it (the register's initial value). The OSD
// Reset, a cartridge load or the end of a program do not. It is not a status bit, so it is
// never saved in the .CFG. With the option On the flag changes nothing; with the option Off
// and no pick the core is a normal Vectrex.
// ============================================================================

`default_nettype none

module pitrex_autoon (
	input  wire        clk,
	input  wire        ioctl_download,
	input  wire [15:0] ioctl_index,
	output logic       auto_on = 1'b0
);

// file slot 3: the low five index bits, as rom_download in Vectrex.sv reads the slot. Bits 7:6 (extension
// select, .ptx vs .um2) and anything above are deliberately ignored: this core has no other slot to confuse.
wire launcher_dl = ioctl_download && (ioctl_index[4:0] == 5'd3);

// the index is only trusted while the download runs; the flag is set when it completes
logic seen = 1'b0, dl_q = 1'b0;
always_ff @(posedge clk) begin
	dl_q <= ioctl_download;
	if (launcher_dl)                   seen <= 1'b1;
	if (dl_q && !ioctl_download) begin
		if (seen) auto_on <= 1'b1;
		seen <= 1'b0;
	end
end

endmodule

`default_nettype wire
