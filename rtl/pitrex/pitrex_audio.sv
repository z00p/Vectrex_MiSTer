// ============================================================================
// PiTrex mode: PCM audio playback (machine clock domain, clk_sys = 24 MHz).
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The ARM writes 16-bit mono samples (four per 64-bit word, first sample in
// bits 15:0) into an audio ring in DDR; pitrex_ddr fetches them into the audio
// FIFO. This module plays one sample every DIVIDER clocks (24 MHz / 750 =
// 32 kHz exactly) and holds the last sample if the FIFO runs dry, so an
// underrun is a short flat spot rather than a click.
//
// Pause (B7): while the machine is paused by the user (pause_cpu) the playback position
// freezes - no sample is consumed and the sample-rate divider stops - so the queued
// audio is still there on resume; the top level keeps muting the output meanwhile.
// A command-stream underrun (the player stalling the machine) does NOT freeze PCM:
// the PCM channel runs on wall-clock time, and the ARM sizes its queue (~100 ms) for that.
// ============================================================================

`default_nettype none

module pitrex_audio #(
	parameter integer DIVIDER = 750
) (
	input  wire               clk,
	input  wire               reset,       // not active: silence, drop the word in hand
	input  wire               pause,       // machine paused by the user: freeze playback

	// audio FIFO read side (first-word-fall-through not assumed)
	output logic              fifo_rd_en = 1'b0,
	input  wire  [63:0]       fifo_rd_data,
	input  wire               fifo_rd_valid,
	input  wire               fifo_rd_empty,

	output logic signed [15:0] sample = '0
);

logic [9:0]  div = '0;
logic [63:0] word = '0;
logic [1:0]  slot = '0;        // next sample within `word`
logic        have = 1'b0;      // `word` holds samples not yet played
logic        pending = 1'b0;   // a FIFO read is in flight

always_ff @(posedge clk) begin
	fifo_rd_en <= 1'b0;
	if (reset) begin
		div     <= '0;
		have    <= 1'b0;
		pending <= 1'b0;
		slot    <= '0;
		sample  <= '0;
	end else begin
		// keep one word in hand: fetch the next as soon as this one is used up
		if (!have && !pending && !fifo_rd_empty) begin
			fifo_rd_en <= 1'b1;
			pending    <= 1'b1;
		end
		if (fifo_rd_valid) begin
			word    <= fifo_rd_data;
			have    <= 1'b1;
			pending <= 1'b0;
			slot    <= '0;
		end

		if (pause) begin
			// frozen: keep position, divider phase and the last sample
		end else if (div == DIVIDER - 1) begin
			div <= '0;
			if (have && !fifo_rd_valid) begin
				case (slot)
					2'd0: sample <= word[15:0];
					2'd1: sample <= word[31:16];
					2'd2: sample <= word[47:32];
					2'd3: sample <= word[63:48];
				endcase
				slot <= slot + 1'd1;
				if (slot == 2'd3) have <= 1'b0;
			end
			// else: underrun, hold the last sample
		end else begin
			div <= div + 1'd1;
		end
	end
end

endmodule

`default_nettype wire
