/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 z00p - PiTrex on MiSTer
 */
/*
 * ptx_wait_test.c - the MiSTer ring backend's frame wait against a fake core, on a PC (18 Nov 2026).
 *
 * The backend maps a plain file instead of /dev/mem (PTX_DEVMEM, PTX_DDR_BASE=0). A thread plays the part of the core: it
 * plays the ring at 1.5 MHz of wall time, counting stream time as pitrex_player.sv does (a bus op delta + 1, a WAIT delta,
 * nothing while the ring is empty), updates rd_idx, core_cycles and the input counter, and stops playing during a pause.
 * The producer draws frames of varying length with a varying idle gap before each mark (a PiTrex program's T2 wait).
 *
 * Measured: how late ptx_frame() returns after the core reached the frame end it waited for (overshoot), the naps per wait,
 * the frame rate and the cycles the fake core starved.  Build twice to compare (see tests/ptx_wait_test.sh):
 *   default              one predicted sleep, then 200 us polls (D3)
 *   -DPTX_WAIT_POLL_ONLY 200 us polls only (before 18 Nov 2026)
 */
#define _GNU_SOURCE
#include "ptx.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define MAP_SIZE  (0x1000u + 65536u * 8u + 8192u * 8u)   /* = PTX_MAP_SIZE */
#define NF        4096

static volatile uint8_t *mem;
#define HDR_MAGIC (*(volatile uint32_t *)(mem + 0x00))
#define HDR_WR    (*(volatile uint64_t *)(mem + 0x10))
#define ST_RD     (*(volatile uint64_t *)(mem + 0x40))
#define ST_CYC    (*(volatile uint64_t *)(mem + 0x48))
#define ST_FRAMES (*(volatile uint32_t *)(mem + 0x50))
#define ST_MAGIC  (*(volatile uint32_t *)(mem + 0x58))
#define IN_SEQ    (*(volatile uint32_t *)(mem + 0x88))
#define RING      ((volatile uint64_t *)(mem + 0x1000))

static double t0;
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9 - t0; }
static double pause_from = 3.0, pause_len = 3.0, run_s = 9.0;
static volatile int quit;
static double frame_seen[NF];            /* when the core fetched the FRAME command of frame k */
static volatile double starved;          /* cycles the core had nothing to play (outside the pause, after the first frame) */

static void *core(void *arg)
{
	(void)arg;
	ST_MAGIC = 0x43585450u;              /* "PTXC" */
	double last = now(), last_seq = 0, budget = 0, cur = 0, cyc = 0;
	uint64_t rd = 0; unsigned nfr = 0; int have = 0;
	while (!quit) {
		struct timespec ts = { 0, 20000 }; nanosleep(&ts, NULL);
		double t = now();
		if (t - last_seq >= 0.001) { IN_SEQ++; last_seq = t; }
		int paused = t >= pause_from && t < pause_from + pause_len;
		if (HDR_MAGIC != 0x58525450u) { last = t; continue; }
		if (!paused) budget += (t - last) * 1.5e6;
		last = t;
		while (budget > 0) {
			if (!have) {
				if (rd >= HDR_WR) { if (nfr && !paused) starved += budget; budget = 0; break; }
				uint64_t c = RING[rd & 65535]; rd++; ST_RD = rd;
				uint32_t op = PTX_CMD_OP(c), d = PTX_CMD_DELTA(c);
				if (op == PTX_OP_FRAME) { if (nfr < NF) frame_seen[nfr] = t; nfr++; ST_FRAMES = nfr; continue; }
				cur = op == PTX_OP_WAIT ? d : (op == PTX_OP_WRITE || op == PTX_OP_READ) ? d + 1.0 : 0;
				if (cur <= 0) continue;
				have = 1;
			}
			if (budget >= cur) { budget -= cur; cyc += cur; have = 0; }
			else { cur -= budget; cyc += budget; budget = 0; }
		}
		ST_CYC = (uint64_t)cyc;
	}
	return NULL;
}

int main(void)
{
	t0 = 0; t0 = now();
	if (getenv("PW_NOPAUSE")) pause_len = 0;
	FILE *f = fopen("/tmp/CORENAME", "w"); if (!f) { perror("/tmp/CORENAME"); return 1; } fputs("Vectrex-PiTrex\n", f); fclose(f);
	int fd = open(PTX_DEVMEM, O_RDWR | O_CREAT | O_TRUNC, 0600);
	if (fd < 0 || ftruncate(fd, MAP_SIZE)) { perror(PTX_DEVMEM); return 1; }
	mem = mmap(0, MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }
	pthread_t th; pthread_create(&th, NULL, core, NULL);
	setenv("PTX_BACKEND", "mister", 1); setenv("PTX_LEAD", "1", 1); setenv("PTX_WAIT_CORE", "2", 1);
	if (ptx_init()) { fprintf(stderr, "ptx_init failed\n"); return 1; }
	srand(1);
	static double ret[NF], pre[NF];
	unsigned k = 0;
	for (; k < NF && now() < run_s; k++) {
		struct timespec work = { 0, (1 + rand() % 12) * 1000000L }; nanosleep(&work, NULL);   /* drawing time on the host */
		int n = 300 + rand() % 200;
		for (int i = 0; i < n; i++) { ptx_advance_cycles(40 + rand() % 60); ptx_write(0xD001, (uint8_t)i); }
		ptx_advance_cycles((uint32_t)(rand() % 6000));       /* idle before the mark, written only with the next frame's first command */
		pre[k] = now(); ptx_frame();
		ret[k] = now();
	}
	char buf[512]; if (ptx_pace_report(buf, sizeof buf)) fprintf(stderr, "%s\n", buf);
	/* overshoot: ptx_frame() of frame k waits (PTX_LEAD=1) until the core has fetched frame k-1's end */
	double sum = 0, mx = 0; unsigned n = 0, late1 = 0;
	for (unsigned i = 2; i < k; i++) {
		double s = frame_seen[i - 1]; if (!s || s > ret[i]) continue;
		if (s < pre[i]) continue;                            /* the core was already past: no wait for this frame */
		if (s >= pause_from - 0.05 && s < pause_from + pause_len + 0.1) continue;
		double o = ret[i] - s; sum += o; n++; if (o > mx) mx = o; if (o > 0.001) late1++;
	}
	printf("frames %u in %.1f s (%.1f/s outside the pause), waits measured %u: overshoot avg %.0f us max %.0f us, over 1 ms %u; core starved %.0f cycles\n",
	       k, now(), (k) / (run_s - pause_len), n, n ? sum / n * 1e6 : 0, mx * 1e6, late1, starved);
	quit = 1; pthread_join(th, NULL);
	ptx_shutdown();
	return 0;
}
