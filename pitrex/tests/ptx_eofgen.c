/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * ptx_eofgen.c - deterministic test stream for the "FRAME as end of frame" proposal
 * (research/frame_eof_proposal_2026-11-19.md, section 4). PROPOSAL ONLY: the flagged FRAME is not
 * agreed with Videodr0me and no core understands it yet.
 *
 * Writes the stream straight to a file (no backend, no reads, no input), so the output is the same
 * on every run:
 *   <out>.ptxs  raw little-endian 64-bit commands (PROTOCOL.md "Command word"), for ptxplay / a sim
 *   <out>.txt   the same as text: one line per command with its stream cycle, for an RTL bench
 *
 * Each frame is exactly FRAME_LEN E cycles (default 30000 = 50 Hz): zero the beam, draw one square
 * (size 120 on even frames, 60 on odd ones, both centred), idle, then the end-of-frame marker:
 *   t2     (today)    WRITE $D008=$30, WRITE $D009=$75 (T2 reload = 30000), plain FRAME
 *   frame  (proposed) no T2 write anywhere; FRAME with reserved bit 0 set, then WAIT 2
 * Both markers take 2 E cycles, so the two variants are cycle-for-cycle the same apart from them.
 * The WAIT 2 keeps the bus clocked for two E periods after the flagged FRAME, so the beam adapter
 * gets its beam samples before the ring can run dry (proposal 2.3).
 *
 * Ramps use fixed waits instead of polling the T1 flag (T1 one-shot of SCALE cycles, blanked
 * SCALE + 2 cycles after the T1H write), so nothing depends on reads.
 *
 * build: cc -O2 -Wall -I../src -o ptx_eofgen ptx_eofgen.c
 * usage: ptx_eofgen t2|frame <out> [frames=100] [frame_len=30000]
 */
#include "ptx.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PTX_FRAME_FLAG_EOF 0x0001u      /* proposed: reserved bit 0 on a FRAME = "this is the end of frame" */

enum { ORB = 0xD000, ORA = 0xD001, DDRB = 0xD002, DDRA = 0xD003,
       T1L = 0xD004, T1H = 0xD005, T2L = 0xD008, T2H = 0xD009,
       SR = 0xD00A, ACR = 0xD00B, PCR = 0xD00C };

#define SCALE 0x7F

static FILE *fb, *ft;
static uint64_t now, emit_end;          /* stream time in E cycles; end of the last command's bus time */
static unsigned long ncmd;

static void put(uint64_t w, const char *what)
{
	unsigned char b[8];
	for (int i = 0; i < 8; i++) b[i] = (unsigned char)(w >> (8 * i));
	fwrite(b, 1, 8, fb);
	fprintf(ft, "%10" PRIu64 "  %016" PRIx64 "  %s\n", now, w, what);
	ncmd++;
}

static void flush_gap(void)             /* gaps longer than one delta field */
{
	while (now - emit_end > PTX_MAX_DELTA) {
		uint64_t at = now; now = emit_end + PTX_MAX_DELTA;
		put(ptx_cmd(PTX_OP_WAIT, PTX_MAX_DELTA, 0, 0), "WAIT 65535");
		emit_end = now; now = at;
	}
}

static void wr(uint16_t a, uint8_t d)   /* one bus write, one E period */
{
	char s[32];
	flush_gap();
	snprintf(s, sizeof s, "WRITE %04X=%02X", a, d);
	put(ptx_cmd(PTX_OP_WRITE, (uint32_t)(now - emit_end), a, d), s);
	now++; emit_end = now;
}

static void adv(uint32_t n) { now += n; }

static void frame_cmd(int eof)          /* takes no bus time */
{
	/* The player ignores a FRAME's delta (pitrex_player.sv: OP_FRAME only counts; the PiTrex library always sends delta 0), so
	 * idle time before the marker goes into its own WAIT. A delta here played as 0 on the core: frames of 831 cycles, not 30000. */
	flush_gap();
	if (now > emit_end) {
		char s[32]; snprintf(s, sizeof s, "WAIT %u", (unsigned)(now - emit_end));
		uint64_t at = now; now = emit_end;
		put(ptx_cmd(PTX_OP_WAIT, (uint32_t)(at - emit_end), 0, 0), s);
		now = at; emit_end = now;
	}
	uint64_t w = ptx_cmd(PTX_OP_FRAME, 0, 0, 0);
	if (eof) w |= PTX_FRAME_FLAG_EOF;
	put(w, eof ? "FRAME +EOF flag" : "FRAME");
	emit_end = now;
}

static void wait_cmd(uint32_t n)        /* n idle E periods, bus clocked */
{
	flush_gap();
	char s[32];
	snprintf(s, sizeof s, "WAIT %u", (unsigned)(now + n - emit_end));
	put(ptx_cmd(PTX_OP_WAIT, (uint32_t)(now + n - emit_end), 0, 0), s);     /* listed at the cycle it starts */
	now += n; emit_end = now;
}

/* as tests/ptx_selftest.c, with fixed waits */
static void mux_sample(int sel, int8_t v) { wr(ORA, (uint8_t)v); wr(ORB, (uint8_t)(sel << 1)); adv(2); wr(ORB, 0x01); }
static void beam_zero(void) { adv(20); wr(PCR, 0xCC); adv(40); wr(PCR, 0xCE); mux_sample(1, 0); }
static void ramp(int8_t dx, int8_t dy, int lit)
{
	mux_sample(0, dy);
	wr(ORA, (uint8_t)dx);
	wr(SR, lit ? 0xFF : 0x00);
	wr(T1L, SCALE);
	wr(T1H, 0);
	adv(SCALE + 2);
	wr(SR, 0x00);
}
static void square(int8_t x, int8_t y, int8_t size)
{
	beam_zero();
	ramp(x, y, 0);
	ramp(size, 0, 1); ramp(0, size, 1); ramp((int8_t)-size, 0, 1); ramp(0, (int8_t)-size, 1);
}

int main(int argc, char **argv)
{
	if (argc < 3 || (strcmp(argv[1], "t2") && strcmp(argv[1], "frame"))) {
		fprintf(stderr, "usage: %s t2|frame <out> [frames=100] [frame_len=30000]\n", argv[0]);
		return 2;
	}
	int eof = !strcmp(argv[1], "frame");
	unsigned frames = argc > 3 ? (unsigned)atoi(argv[3]) : 100;
	unsigned len = argc > 4 ? (unsigned)atoi(argv[4]) : 30000;
	char p[512];
	snprintf(p, sizeof p, "%s.ptxs", argv[2]); fb = fopen(p, "wb");
	snprintf(p, sizeof p, "%s.txt", argv[2]); ft = fopen(p, "w");
	if (!fb || !ft) { perror("open"); return 1; }
	fprintf(ft, "# ptx_eofgen %s: %u frames of %u E cycles; columns: stream cycle at the command, raw word, meaning\n", argv[1], frames, len);
	fprintf(ft, "# WRITE: the delta (bits 55..40) idle E periods come first, then the write takes 1 E period. FRAME: delta always 0, takes no time. WAIT n: n idle E periods.\n");

	put(ptx_cmd(PTX_OP_RESET_SYNC, 0, 0, 0), "RESET_SYNC");
	wr(DDRA, 0xFF); wr(DDRB, 0x9F); wr(ORB, 0x01);
	wr(ACR, 0x98); wr(SR, 0x00);
	mux_sample(2, 0x60);                    /* brightness */

	uint64_t base = now;
	for (unsigned f = 0; f < frames; f++) {
		uint64_t start = base + (uint64_t)f * len;
		int8_t size = (f & 1) ? 60 : 120;
		square((int8_t)(-size / 2), (int8_t)(-size / 2), size);
		beam_zero();                        /* park at the centre, beam off */
		uint64_t mark = start + len - 2;
		if (now > mark) { fprintf(stderr, "frame %u overruns (%" PRIu64 " > %" PRIu64 ")\n", f, now, mark); return 1; }
		now = mark;
		if (eof) { frame_cmd(1); wait_cmd(2); }
		else { wr(T2L, 0x30); wr(T2H, 0x75); frame_cmd(0); }
		if (now != start + len) { fprintf(stderr, "frame %u ends at %" PRIu64 ", not %" PRIu64 "\n", f, now, start + len); return 1; }
	}
	fprintf(ft, "# end: %lu commands, stream cycle %" PRIu64 "\n", ncmd, now);
	fclose(fb); fclose(ft);
	fprintf(stderr, "%s: %lu commands, %u frames, %" PRIu64 " E cycles\n", argv[1], ncmd, frames, now);
	return 0;
}
