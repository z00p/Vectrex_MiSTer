/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * ptx_backend_file.c - PC harness backend: commands go to a raw file of
 * little-endian 64-bit words for tools/ptxplay. Input comes from env vars.
 *
 *   PTX_STREAM       output path (default stream.ptxs)
 *   PTX_MAX_FRAMES   exit cleanly after this many frames (default 120)
 *   PTX_POT          "x1,y1,x2,y2" signed pot values (default 0,0,0,0)
 *   PTX_BUTTONS      PSG port A byte, active low (default 0xFF = nothing pressed)
 *   PTX_BUTTONS_FROM frame from which PTX_BUTTONS applies (default 0)
 */
#include "ptx.h"

#include <stdio.h>
#include <stdlib.h>

static FILE *out;
static uint32_t frames, max_frames;
static ptx_input_t fake;
static uint32_t buttons_from;
static uint8_t buttons_down;

static int file_open(void)
{
	const char *p = getenv("PTX_STREAM");
	out = fopen(p ? p : "stream.ptxs", "wb");
	if (!out) { perror("ptx: stream file"); return -1; }
	const char *m = getenv("PTX_MAX_FRAMES");
	max_frames = m ? (uint32_t)strtoul(m, NULL, 0) : 120;
	int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
	const char *pot = getenv("PTX_POT");
	if (pot) sscanf(pot, "%d,%d,%d,%d", &x1, &y1, &x2, &y2);
	fake.pot[0] = (int8_t)x1; fake.pot[1] = (int8_t)y1;
	fake.pot[2] = (int8_t)x2; fake.pot[3] = (int8_t)y2;
	const char *b = getenv("PTX_BUTTONS");
	fake.psg_ioa = b ? (uint8_t)strtoul(b, NULL, 0) : 0xFF;
	const char *bf = getenv("PTX_BUTTONS_FROM");   /* frame the buttons go down */
	buttons_from = bf ? (uint32_t)strtoul(bf, NULL, 0) : 0;
	buttons_down = fake.psg_ioa;
	return 0;
}

static void file_push(uint64_t cmd)
{
	unsigned char b[8];
	for (int i = 0; i < 8; i++) b[i] = (unsigned char)(cmd >> (8 * i));
	fwrite(b, 1, 8, out);
}

static void file_close(void) { if (out) { fclose(out); out = NULL; } }

static void file_frame(uint64_t stream_cycles)
{
	(void)stream_cycles;
	if (++frames >= max_frames) {
		file_close();
		fprintf(stderr, "ptx: %u frames written, stopping\n", frames);
		exit(0);
	}
}

static void file_input(ptx_input_t *o)
{
	fake.seq = frames;
	fake.psg_ioa = frames >= buttons_from ? buttons_down : 0xFF;
	*o = fake;
}
static void file_status(ptx_status_t *o) { o->valid = 0; }

const ptx_backend_t ptx_backend_file = { file_open, file_push, file_frame, file_input, file_status, file_close, NULL };
