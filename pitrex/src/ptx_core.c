/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * ptx_core.c - stream time, command emission and the shadow VIA/PSG.
 *
 * Shadow timing follows the 6522: a write occupies one E-cycle; after a write
 * of T1C-H/T2C-H at cycle t the counter loads N and the interrupt flag becomes
 * visible to a read at cycle t + N + 2 (the datasheet's N + 1.5, rounded up to
 * the next whole read cycle).
 */
#include "ptx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- stream time ------------------------------------------------------- */
static uint64_t now_cycles;        /* current stream time */
static uint32_t ns_remainder;      /* sub-cycle nanoseconds (x3 fixed point) */
static uint64_t emit_end;          /* stream time after the last emitted bus op */
static const ptx_backend_t *be;

/* ---- shadow VIA -------------------------------------------------------- */
static uint8_t ora, orb, ddra, ddrb, acr, pcr, ier, ifr_static;
static uint8_t t1_latch_lo, t1_latch_hi, t2_latch_lo;
static uint64_t t1_start;  static uint32_t t1_n; static int t1_armed;
static uint64_t t2_start;  static uint32_t t2_n; static int t2_armed;
static int t2_sync_pending;        /* re-phase the sub-cycle remainder when the program sees T2 expire */
static int no_t2_sync;             /* PTX_NOSYNC=1: old behaviour, for A/B comparison */

/* ---- shadow PSG -------------------------------------------------------- */
static uint8_t psg_reg[16];
static uint8_t psg_latch;
static int     psg_mode;           /* (bdir<<1)|bc1 of the current port B output */

static ptx_input_t input;

static uint8_t pb_out(void) { return orb; }   /* core samples raw outputs */
static uint8_t pa_out(void) { return ora; }

static void emit(uint32_t op, uint16_t addr, uint8_t data)
{
	uint64_t delta = now_cycles - emit_end;
	while (delta > PTX_MAX_DELTA) {
		be->push(ptx_cmd(PTX_OP_WAIT, PTX_MAX_DELTA, 0, 0));
		delta -= PTX_MAX_DELTA;
	}
	be->push(ptx_cmd(op, (uint32_t)delta, addr, data));
	now_cycles += 1;            /* the bus cycle itself */
	emit_end = now_cycles;
}

static void update_timers(void)
{
	if (t1_armed && now_cycles >= t1_start + t1_n + 2) {
		ifr_static |= 0x40;
		if (acr & 0x40) {      /* free-run: reload, next flag N + 2 later */
			uint64_t period = (uint64_t)t1_n + 2;
			uint64_t k = (now_cycles - (t1_start + t1_n + 2)) / period + 1;
			t1_start += k * period;
		} else {
			t1_armed = 0;
		}
	}
	if (t2_armed && now_cycles >= t2_start + t2_n + 2) {
		ifr_static |= 0x20;
		t2_armed = 0;
	}
}

static uint16_t timer_count(uint64_t start, uint32_t n)
{
	/* counter loads at start+1, decrements every cycle, wraps through 0xFFFF */
	int64_t elapsed = (int64_t)now_cycles - (int64_t)(start + 1);
	if (elapsed < 0) return (uint16_t)n;
	return (uint16_t)((int64_t)n - elapsed);
}

static void psg_bus_update(void)
{
	int mode = ((pb_out() >> 4) & 1) << 1 | ((pb_out() >> 3) & 1);
	if (mode == 3)                        /* latch address */
		psg_latch = pa_out() & 0x0F;
	else if (mode == 2)                   /* write data */
		psg_reg[psg_latch] = pa_out();
	psg_mode = mode;
}

static uint8_t psg_read_bus(void)
{
	if (psg_mode != 1) return 0xFF;       /* not in read mode: bus floats high */
	if (psg_latch == 14 && !(psg_reg[7] & 0x40)) {
		be->input(&input);
		return input.psg_ioa;
	}
	return psg_reg[psg_latch];
}

static int compare_bit(void)
{
	be->input(&input);
	int8_t pot = input.pot[(pb_out() >> 1) & 3];
	int8_t dac = (int8_t)pa_out();
	return pot > dac;
}

/* ---- public ------------------------------------------------------------ */
int ptx_init(void)
{
	const char *b = getenv("PTX_BACKEND");
	be = (b && !strcmp(b, "mister")) ? &ptx_backend_mister : &ptx_backend_file;
	if (be->open()) return -1;
	now_cycles = emit_end = 0;
	no_t2_sync = getenv("PTX_NOSYNC") != NULL;
	be->push(ptx_cmd(PTX_OP_RESET_SYNC, 0, 0, 0));
	return 0;
}

void ptx_shutdown(void) { if (be) be->close(); }

void ptx_advance_cycles(uint32_t cycles) { now_cycles += cycles; }

void ptx_advance_ns(uint32_t ns)
{
	/* cycles = ns * 3 / 2000, keeping the remainder */
	uint64_t total = (uint64_t)ns * 3 + ns_remainder;
	now_cycles += total / 2000;
	ns_remainder = (uint32_t)(total % 2000);
}

void ptx_wait_until_cycle(uint64_t cycle) { if (cycle > now_cycles) now_cycles = cycle; }
uint64_t ptx_now_cycles(void) { return now_cycles; }
uint32_t ptx_now_us(void) { return (uint32_t)(now_cycles * 2 / 3); }

void ptx_get_status(ptx_status_t *o) { be->status(o); }
void ptx_get_input(ptx_input_t *o) { be->input(o); }

void ptx_poll(void) { if (be && be->poll) be->poll(); }

void ptx_frame(void)
{
	be->push(ptx_cmd(PTX_OP_FRAME, 0, 0, 0));
	be->frame(now_cycles);
}

void ptx_write(uint16_t addr, uint8_t data)
{
	if ((addr & 0xF000) != 0xD000) {      /* outside the VIA: pass through as-is */
		emit(PTX_OP_WRITE, addr, data);
		return;
	}
	update_timers();
	switch (addr & 0x0F) {
	case 0x0: orb = data; break;
	case 0x1: case 0xF: ora = data; break;
	case 0x2: ddrb = data; break;
	case 0x3: ddra = data; break;
	case 0x4: case 0x6: t1_latch_lo = data; break;
	case 0x5: t1_latch_hi = data; break;      /* counter loads after emit, below */
	case 0x7: t1_latch_hi = data; ifr_static &= ~0x40; break;
	case 0x8: t2_latch_lo = data; break;
	case 0x9: break;
	case 0xA: ifr_static &= ~0x04; break;
	case 0xB: acr = data; break;
	case 0xC: pcr = data; break;
	case 0xD: ifr_static &= ~(data & 0x7F); break;
	case 0xE: if (data & 0x80) ier |= data & 0x7F; else ier &= ~(data & 0x7F); break;
	}
	uint64_t t = now_cycles;                  /* cycle this write occupies */
	emit(PTX_OP_WRITE, addr, data);
	switch (addr & 0x0F) {
	case 0x5:
		t1_n = (uint32_t)t1_latch_hi << 8 | t1_latch_lo;
		t1_start = t; t1_armed = 1; ifr_static &= ~0x40;
		break;
	case 0x9:
		t2_n = (uint32_t)data << 8 | t2_latch_lo;
		t2_start = t; t2_armed = 1; ifr_static &= ~0x20; t2_sync_pending = 1;
		break;
	case 0x0: case 0x1: case 0xF:
		psg_bus_update();
		break;
	}
}

uint8_t ptx_read(uint16_t addr)
{
	if ((addr & 0xF000) != 0xD000) {      /* RAM/cart reads are not modelled */
		emit(PTX_OP_READ, addr, 0);
		return 0xFF;
	}
	update_timers();
	uint8_t r = 0xFF;
	int side_effect = 0;
	switch (addr & 0x0F) {
	case 0x0: {
		uint8_t in = (uint8_t)(compare_bit() << 5);   /* bit 6 (cart PB6) reads 0 */
		r = (orb & ddrb) | (in & ~ddrb);
		ifr_static &= ~0x18; side_effect = 1;
		break;
	}
	case 0x1: r = (ora & ddra) | (psg_read_bus() & ~ddra); ifr_static &= ~0x03; side_effect = 1; break;
	case 0xF: r = (ora & ddra) | (psg_read_bus() & ~ddra); side_effect = 1; break;
	case 0x2: r = ddrb; break;
	case 0x3: r = ddra; break;
	case 0x4: r = timer_count(t1_start, t1_n) & 0xFF; ifr_static &= ~0x40; side_effect = 1; break;
	case 0x5: r = timer_count(t1_start, t1_n) >> 8; break;
	case 0x6: r = t1_latch_lo; break;
	case 0x7: r = t1_latch_hi; break;
	case 0x8: r = timer_count(t2_start, t2_n) & 0xFF; ifr_static &= ~0x20; side_effect = 1; break;
	case 0x9: r = timer_count(t2_start, t2_n) >> 8; break;
	case 0xA: r = 0; ifr_static &= ~0x04; side_effect = 1; break;
	case 0xB: r = acr; break;
	case 0xC: r = pcr; break;
	case 0xD: {
		uint8_t f = ifr_static & 0x7F;
		/* The program has just seen the frame timer expire (the T2 wait at the top of every
		 * frame). Restart the sub-cycle (quarter/eighth-cycle) rounding from zero here, so every
		 * frame rounds its short delays identically: without this, the leftover from the previous
		 * frame made event spacing differ by one E cycle between frames (jitter on ZERO/REF and
		 * ramp sequences, visible as jumping dots). */
		if ((f & 0x20) && t2_sync_pending) {
			t2_sync_pending = 0;
			if (!no_t2_sync) ns_remainder = 0;
		}
		r = f | ((f & ier) ? 0x80 : 0);
		break;
	}
	case 0xE: r = ier | 0x80; break;
	}
	if (side_effect)
		emit(PTX_OP_READ, addr, 0);           /* keep the core's VIA state identical */
	else
		now_cycles += 1;                      /* a read still takes a bus cycle */
	return r;
}
