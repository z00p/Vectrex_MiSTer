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
 *   PTX_PRESS        "frame:mask,frame:mask,..." (PSG port A byte, active low, e.g. "400:0xF7,410:0xFF" = hold button 4 for ten frames): from each frame on the byte is that mask; the
 *                    last pair whose frame has been reached wins, and before the first one PTX_BUTTONS applies (a press AND a release, which PTX_BUTTONS alone cannot do)
 *   PTX_POTAT        "frame:x1/y1/x2/y2,..." signed pot values from each frame on (the last entry whose frame has been reached wins; before the first one PTX_POT applies), e.g.
 *                    "1500:0/-100/0/0" = push stick 1 forward from frame 1500: games that want the stick centred at start-up (calibration) can be walked after the title
 */
#include "ptx.h"

#include <stdio.h>
#include <stdlib.h>

static FILE *out;
static uint32_t frames, max_frames;
static ptx_input_t fake;
static uint32_t buttons_from;
static uint8_t buttons_down;
static struct { uint32_t from; uint8_t mask; } press[64]; static int npress;     /* PTX_PRESS */
static struct { uint32_t from; int8_t p[4]; } potat[64]; static int npotat;      /* PTX_POTAT */

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
	const char *pr = getenv("PTX_PRESS");
	while (pr && *pr && npress < 64) {
		char *q; press[npress].from = (uint32_t)strtoul(pr, &q, 0); if (*q != ':') break;
		press[npress++].mask = (uint8_t)strtoul(q + 1, &q, 0); pr = *q == ',' ? q + 1 : q;
	}
	const char *pa = getenv("PTX_POTAT");
	while (pa && *pa && npotat < 64) {
		char *q; potat[npotat].from = (uint32_t)strtoul(pa, &q, 0); if (*q != ':') break;
		for (int i = 0; i < 4; i++) { potat[npotat].p[i] = (int8_t)strtol(q + 1, &q, 0); if (i < 3 && *q != '/') break; }
		npotat++; pa = *q == ',' ? q + 1 : q;
	}
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
	for (int i = 0; i < npress; i++) if (frames >= press[i].from) fake.psg_ioa = press[i].mask;
	for (int i = 0; i < npotat; i++) if (frames >= potat[i].from) for (int k = 0; k < 4; k++) fake.pot[k] = potat[i].p[k];
	*o = fake;
}
static void file_status(ptx_status_t *o) { o->valid = 0; }

const ptx_backend_t ptx_backend_file = { file_open, file_push, file_frame, file_input, file_status, file_close, NULL };
