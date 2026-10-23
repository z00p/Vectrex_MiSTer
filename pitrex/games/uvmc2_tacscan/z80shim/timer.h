/* SPDX-License-Identifier: GPL-2.0-or-later
 * Shim for AAE's timer.h: the Z80 core calls timer_update() after every instruction to
 * advance AAE's emulated peripheral timers; Tac/Scan has none. */
#ifndef Z80SHIM_TIMER_H
#define Z80SHIM_TIMER_H
static inline void timer_update(int cycles, int cpu) { (void)cycles; (void)cpu; }
#endif
