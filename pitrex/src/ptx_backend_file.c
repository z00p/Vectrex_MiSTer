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
#include <time.h>
#include <sys/resource.h>
#include <pthread.h>

/* PTX_SIMCORE=1 (PC test hook, 16 Nov 2026): model the core's consumption of the stream so the PC run is paced like the
 * MiSTer.  The simulated core plays stream cycles at 1.5 MHz of wall time, never past what has been handed over (an empty
 * FIFO stalls it, exact resume), and not at all inside PTX_SIMPAUSE="start_s:len_s[,start_s:len_s]" (the core's Pause
 * button).  ptx_frame() waits like m_wait: at most PTX_LEAD (default 1) frames ahead, a predicted sleep, then 200 us polls
 * backing off to 2 ms while the core does not move.  status.core_cycles = cycles the simulated core has run. */
static int sim_on = -1, sim_lead = 1, sim_np; static double sim_ps[4], sim_pl[4], sim_t0, sim_last, sim_pos, sim_prod;
static uint64_t sim_fend[8]; static uint32_t sim_n;
static double sim_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int sim_paused(double t) { for (int i = 0; i < sim_np; i++) if (t - sim_t0 >= sim_ps[i] && t - sim_t0 < sim_ps[i] + sim_pl[i]) return 1; return 0; }
static pthread_mutex_t sim_m = PTHREAD_MUTEX_INITIALIZER;
static void sim_adv_locked(void);
static void sim_adv(void) { pthread_mutex_lock(&sim_m); sim_adv_locked(); pthread_mutex_unlock(&sim_m); }
static void sim_adv_locked(void) {
	double t = sim_now(), a = sim_last; sim_last = t;
	/* split [a,t) at pause edges so a long nap across an edge is counted right */
	for (int i = 0; i < sim_np; i++) { double ps = sim_t0 + sim_ps[i], pe = ps + sim_pl[i];
		if (a < ps && t > ps) { sim_pos += (ps - a) * 1.5e6; a = ps; }
		if (a >= ps && a < pe) { a = t < pe ? t : pe; } }
	if (!sim_paused(a)) sim_pos += (t - a) * 1.5e6;
	if (sim_pos > sim_prod) sim_pos = sim_prod;
}
static void sim_init(void) {
	const char *e = getenv("PTX_SIMCORE"); sim_on = e && *e == '1';
	if (!sim_on) return;
	e = getenv("PTX_LEAD"); if (e) sim_lead = atoi(e);
	e = getenv("PTX_SIMPAUSE");
	while (e && *e && sim_np < 4) { char *q; sim_ps[sim_np] = strtod(e, &q); if (*q != ':') break; sim_pl[sim_np] = strtod(q + 1, &q); sim_np++; e = *q == ',' ? q + 1 : q; }
	sim_t0 = sim_last = sim_now();
}

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

static double sim_target; static int sim_pend;   /* the wait file_frame leaves to file_wait */
static void file_frame(uint64_t stream_cycles)
{
	if (sim_on < 0) sim_init();
	if (sim_on) {
		pthread_mutex_lock(&sim_m); sim_adv_locked(); sim_prod = (double)stream_cycles; pthread_mutex_unlock(&sim_m);
		sim_fend[sim_n % 8] = stream_cycles; sim_n++;
		if (sim_n > (uint32_t)sim_lead) { sim_target = (double)sim_fend[(sim_n - 1 - sim_lead) % 8]; sim_pend = 1; }
	}
	if (++frames >= max_frames) {
		file_close();
		fprintf(stderr, "ptx: %u frames written, stopping\n", frames);
		exit(0);
	}
}

static void file_wait(void)      /* the simulated core's pacing wait, apart from file_frame and shaped like m_wait (ptx_backend_mister.c) */
{
	if (!sim_pend) return;
	sim_pend = 0;
	double target = sim_target, w0 = sim_now(), seen = -1, moved = w0; long poll_us = 200; int told = 0;
	for (sim_adv(); sim_pos < target; sim_adv()) {
		double pos; pthread_mutex_lock(&sim_m); pos = sim_pos; pthread_mutex_unlock(&sim_m);
		if (pos != seen) { seen = pos; poll_us = 200; moved = sim_now(); } else if (poll_us < 2000) poll_us *= 2;
		long us = (long)((target - pos) / 1.5) - 500;    /* the predicted sleep, as m_wait's (the simulation knows its position exactly) */
		us = us < 200 ? poll_us : us > 20000 ? 20000 : us;
		struct timespec ts = { 0, us * 1000L }; nanosleep(&ts, NULL);
		if (!told && sim_now() - moved >= 2.0) { fprintf(stderr, "ptx: (sim) the core has fetched nothing for 2 s\n"); told = 1; }
	}
	if (told) fprintf(stderr, "ptx: (sim) the core is fetching again after %.1f s\n", sim_now() - w0);
}

static void file_input(ptx_input_t *o)
{
	fake.seq = frames;
	fake.psg_ioa = frames >= buttons_from ? buttons_down : 0xFF;
	for (int i = 0; i < npress; i++) if (frames >= press[i].from) fake.psg_ioa = press[i].mask;
	for (int i = 0; i < npotat; i++) if (frames >= potat[i].from) for (int k = 0; k < 4; k++) fake.pot[k] = potat[i].p[k];
	*o = fake;
}
static void file_status(ptx_status_t *o) { o->valid = 0; if (sim_on > 0) { sim_adv(); pthread_mutex_lock(&sim_m); o->core_cycles = (uint64_t)sim_pos; pthread_mutex_unlock(&sim_m); o->frames = frames; } }

const ptx_backend_t ptx_backend_file = { file_open, file_push, file_frame, file_input, file_status, file_close, NULL, file_wait };
