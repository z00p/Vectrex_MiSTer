// SPDX-License-Identifier: GPL-2.0-or-later
/* A cheap CLOCK_MONOTONIC for the MiSTer (3 Nov 2026).
 *
 * The MiSTer kernel has no vDSO: every clock_gettime is a real syscall, 1.3 us on the A9 (measured). um2run reads the clock a lot (guest
 * TIMER and DWT reads, the audio thread, the ring backend's ownership check), about 4% of core 1 in Pac-Man.  The Cortex-A9 global timer
 * (PERIPHBASE 0xFFFEC000 + 0x200, 64 bits, PERIPHCLK about 200 MHz) is running and reads in 63 ns through /dev/mem.  Its crystal is not
 * the one behind CLOCK_MONOTONIC (about 180 ppm apart here), so the conversion is calibrated at start (50 ms) and slewed back onto
 * CLOCK_MONOTONIC at every um2_clock_sync() (the heartbeat, every 2 s): the result stays continuous, never goes backwards, and is within
 * a few microseconds of CLOCK_MONOTONIC, so deadlines for pthread_cond_timedwait and clock_nanosleep made from it stay right.
 * Anywhere the timer cannot be mapped (the PC, no /dev/mem) everything falls back to clock_gettime.  UM2_SLOWCLOCK=1 forces that. */
#include "um2clock.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define GT_BASE 0xFFFEC000u
static volatile const uint32_t *g_gt;            /* global timer: [0] count low, [1] count high, [2] control */
static volatile uint32_t g_seq;                  /* seqlock over the conversion below (odd while it changes) */
static uint64_t g_c0, g_n0;                      /* ns = n0 + ((count - c0) * mul >> 24) */
static uint64_t g_mul;
static uint64_t g_last_c, g_last_n;              /* the previous sync: CLOCK_MONOTONIC against the count, for the rate */
static uint64_t g_floor;                         /* the largest value handed out (never go backwards across a re-slew) */

static uint64_t sys_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t gt_count(void) {
	uint32_t h, l, h2;
	do { h = g_gt[1]; l = g_gt[0]; h2 = g_gt[1]; } while (h != h2);
	return (uint64_t)h << 32 | l;
}
static uint64_t conv(uint64_t c) { return g_n0 + (((c - g_c0) * g_mul) >> 24); }

void um2_clock_init(void) {
	if (getenv("UM2_SLOWCLOCK")) return;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC | O_CLOEXEC);
	if (fd < 0) return;
	void *p = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, GT_BASE);
	close(fd);
	if (p == MAP_FAILED) return;
	volatile const uint32_t *gt = (volatile const uint32_t *)((volatile const char *)p + 0x200);
	if (!(gt[2] & 1)) { munmap(p, 4096); return; }                                       /* timer not enabled: leave it alone */
	g_gt = gt;
	uint64_t c1 = gt_count(), n1 = sys_ns();
	struct timespec w = { 0, 50000000 }; nanosleep(&w, NULL);
	uint64_t c2 = gt_count(), n2 = sys_ns();
	if (c2 <= c1 || n2 <= n1) { g_gt = NULL; munmap(p, 4096); return; }
	uint64_t mul = ((n2 - n1) << 24) / (c2 - c1);
	if (mul < (2ull << 24) || mul > (20ull << 24)) { g_gt = NULL; munmap(p, 4096); return; }  /* outside 50..500 MHz: not the timer we expect */
	g_mul = mul; g_c0 = c2; g_n0 = n2; g_last_c = c2; g_last_n = n2; g_floor = n2;
}

uint64_t um2_clock_ns(void) {
	if (!g_gt) return sys_ns();
	uint64_t v = 0; uint32_t s;
	do {
		s = g_seq; __sync_synchronize();
		if (s & 1) continue;
		uint64_t c = gt_count();
		if (c - g_c0 > (1ull << 33)) { __sync_synchronize(); if (g_seq == s) return sys_ns(); continue; }   /* no sync for ~40 s: trust the kernel */
		v = conv(c);
		__sync_synchronize();
	} while ((s & 1) || g_seq != s);
	uint64_t f = __atomic_load_n(&g_floor, __ATOMIC_RELAXED);
	while (v > f && !__atomic_compare_exchange_n(&g_floor, &f, v, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) ;
	return v > f ? v : f;
}

void um2_clock_sync(void) {
	if (!g_gt) return;
	uint64_t c = gt_count(), n = sys_ns();
	if (c <= g_last_c || n <= g_last_n) return;
	uint64_t f = conv(c);                                   /* where the fast clock is now: the new segment starts here (continuous) */
	/* the rate since the last sync, corrected so that the fast clock meets CLOCK_MONOTONIC again in about 2 s */
	double ns_per_c = (double)(n - g_last_n) / (double)(c - g_last_c);
	double err = (double)n - (double)f;                     /* > 0: the fast clock is behind */
	if (err > 2e6 || err < -2e6) { f = n > g_floor ? n : g_floor; err = (double)n - (double)f; }   /* far off (a stall): restart from the kernel, never backwards */
	double adj = 1.0 + err / 2e9; if (adj > 1.001) adj = 1.001; if (adj < 0.999) adj = 0.999;
	uint64_t mul = (uint64_t)(ns_per_c * adj * 16777216.0);
	if (mul < (2ull << 24) || mul > (20ull << 24)) return;
	g_seq++; __sync_synchronize();
	g_c0 = c; g_n0 = f; g_mul = mul;
	__sync_synchronize(); g_seq++;
	g_last_c = c; g_last_n = n;
}

int um2_clock_fast(void) { return g_gt != NULL; }

/* the ring backend (../src/ptx_backend_mister.c) reads the clock through this weak hook */
long ptx_clock_ms(void) { return (long)(int64_t)((double)um2_clock_ns() * 1e-6); }   /* no 64-bit divide (a libgcc call on the A9) */
