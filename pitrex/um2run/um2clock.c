// SPDX-License-Identifier: GPL-2.0-or-later
/* The cart's crystal: one cheap, free-running clock for um2run (3 Nov 2026; one rate, no slew since 18 Nov 2026).
 *
 * The MiSTer kernel has no vDSO: every clock_gettime is a real syscall, 1.3 us on the A9 (measured). um2run reads the clock a lot (guest
 * TIMER and DWT reads, the audio thread, the ring backend's ownership check), about 4% of core 1 in Pac-Man.  The Cortex-A9 global timer
 * (PERIPHBASE 0xFFFEC000 + 0x200, 64 bits, PERIPHCLK about 200 MHz) is running and reads in 63 ns through /dev/mem.
 *
 * Its rate is measured once at start against CLOCK_MONOTONIC (50 ms) and then never changed, like the crystal on a real cart: the TIMER,
 * alarms, audio pacing and every interval in um2run come from this one clock.  It is NOT CLOCK_MONOTONIC: the two crystals are about 180 ppm
 * apart here, so the two drift apart by about 0.65 s an hour.  So nothing hands the kernel an absolute deadline made from it: a sleep is
 * "for so long" (um2_kernel_deadline() turns a length into the kernel's own deadline just before the call).  Until 18 Nov 2026 it was slewed
 * back onto CLOCK_MONOTONIC every 2 s so that such deadlines stayed right, with a floor, a seqlock and a jump when 2 ms off: a second clock
 * mixed into the first (review C1/C2).
 * Anywhere the timer cannot be mapped (the PC, no /dev/mem) everything falls back to clock_gettime.  UM2_SLOWCLOCK=1 forces that. */
#include "um2clock.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#define GT_BASE 0xFFFEC000u
static volatile const uint32_t *g_gt;            /* global timer: [0] count low, [1] count high, [2] control */
static uint64_t g_c0, g_n0;                      /* ns = n0 + (count - c0) * mul / 2^24, set once by um2_clock_init */
static uint64_t g_mul;

static uint64_t sys_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static uint64_t gt_count(void) {
	uint32_t h, l, h2;
	do { h = g_gt[1]; l = g_gt[0]; h2 = g_gt[1]; } while (h != h2);
	return (uint64_t)h << 32 | l;
}
/* (d * mul) >> 24 without overflow for any d up to centuries: split d at bit 24 (exact, as the high part's product is a whole multiple of 2^24) */
static uint64_t conv(uint64_t c) { uint64_t d = c - g_c0; return g_n0 + (d >> 24) * g_mul + (((d & 0xFFFFFFu) * g_mul) >> 24); }
/* One (count, CLOCK_MONOTONIC) pair: the kernel read between two counter reads, the narrowest of 8 tries, its count taken at the middle.
 * One plain read is not enough (15 Nov 2026): in main-pitrex --um2run (a large dynamically linked process) the first clock_gettime took
 * 52-54 us against 3-4 us later, every time.  That window sat inside the 50 ms calibration, so the fast clock ran 0.1 % slow. */
static void gt_pair(uint64_t *c, uint64_t *n) {
	uint64_t best = ~0ull;
	for (int i = 0; i < 8; i++) {
		uint64_t a = gt_count(), t = sys_ns(), b = gt_count();
		if (b - a < best) { best = b - a; *c = a + ((b - a) >> 1); *n = t; }
	}
}

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
	uint64_t c1, n1, c2, n2;
	gt_pair(&c1, &n1);
	struct timespec w = { 0, 50000000 }; nanosleep(&w, NULL);
	gt_pair(&c2, &n2);
	if (c2 <= c1 || n2 <= n1) { g_gt = NULL; munmap(p, 4096); return; }
	uint64_t mul = ((n2 - n1) << 24) / (c2 - c1);
	if (mul < (2ull << 24) || mul > (20ull << 24)) { g_gt = NULL; munmap(p, 4096); return; }  /* outside 50..500 MHz: not the timer we expect */
	g_mul = mul; g_c0 = c2; g_n0 = n2;
	__sync_synchronize();                    /* before any other thread exists, but cheap */
}

uint64_t um2_clock_ns(void) { return g_gt ? conv(gt_count()) : sys_ns(); }

void um2_kernel_deadline(struct timespec *t, uint64_t after_ns) {
	uint64_t v = sys_ns() + after_ns;
	t->tv_sec = (time_t)(v / 1000000000ull); t->tv_nsec = (long)(v % 1000000000ull);
}

int um2_clock_fast(void) { return g_gt != NULL; }

/* the ring backend (../src/ptx_backend_mister.c) reads the clock through this weak hook */
long ptx_clock_ms(void) { return (long)(int64_t)((double)um2_clock_ns() * 1e-6); }   /* no 64-bit divide (a libgcc call on the A9) */
