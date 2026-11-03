// SPDX-License-Identifier: GPL-2.0-or-later
/* A cheap CLOCK_MONOTONIC (the A9 global timer, slewed onto the kernel's clock); see um2clock.c. */
#ifndef UM2CLOCK_H
#define UM2CLOCK_H
#include <stdint.h>
#include <time.h>
void um2_clock_init(void);              /* once, early (takes 50 ms to calibrate) */
void um2_clock_sync(void);              /* every couple of seconds, from one thread */
uint64_t um2_clock_ns(void);            /* CLOCK_MONOTONIC in ns */
int um2_clock_fast(void);               /* 1 if the global timer is in use */
static inline void um2_mono(struct timespec *t) { uint64_t v = um2_clock_ns(); t->tv_sec = (time_t)(v / 1000000000ull); t->tv_nsec = (long)(v % 1000000000ull); }
#endif
