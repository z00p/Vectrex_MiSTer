// SPDX-License-Identifier: GPL-2.0-or-later
/* The cart's crystal: the A9 global timer at one rate measured at start, never slewed; see um2clock.c. */
#ifndef UM2CLOCK_H
#define UM2CLOCK_H
#include <stdint.h>
#include <time.h>
void um2_clock_init(void);              /* once, early (takes 50 ms to calibrate) */
uint64_t um2_clock_ns(void);            /* ns on the cart crystal (near CLOCK_MONOTONIC at start, then drifting apart: never mix the two) */
int um2_clock_fast(void);               /* 1 if the global timer is in use */
/* A CLOCK_MONOTONIC deadline after_ns from now, for the kernel calls that only take absolute times (pthread_cond_timedwait): the length
 * comes from the crystal, the deadline from the kernel's clock, read here. */
void um2_kernel_deadline(struct timespec *t, uint64_t after_ns);
#endif
