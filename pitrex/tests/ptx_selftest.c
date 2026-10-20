/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * ptx_selftest.c - bring-up test for PiTrex mode, with no PiTrex library.
 *
 * Drives the VIA directly through the stream layer, the way the Vectrex BIOS
 * does, so a failure points at the core or the ring rather than the library:
 *   - a square and a centre cross at 50 Hz (T2 frame timer, T1 ramps,
 *     shift-register blanking, mux sample-and-holds),
 *   - a dot that follows joystick 1 (input block: core -> DDR -> ARM),
 *   - four marks lit by buttons 1-4 (PSG port A read through the VIA),
 *   - once a second, a status line: ARM stream time, the core's stream time,
 *     the lead between them, fetched commands, frames and underruns.
 *
 * PC:     PTX_STREAM=selftest.ptxs PTX_MAX_FRAMES=60 ./ptx_selftest
 * MiSTer: PTX_BACKEND=mister ./ptx_selftest      (PiTrex Mode = On first)
 */
#include "ptx.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

enum { ORB = 0xD000, ORA = 0xD001, DDRB = 0xD002, DDRA = 0xD003,
       T1L = 0xD004, T1H = 0xD005, T2L = 0xD008, T2H = 0xD009,
       SR = 0xD00A, ACR = 0xD00B, PCR = 0xD00C, IFR = 0xD00D };

/* port B: PB0 = mux off (high), PB1-2 = mux select, PB3/4 = PSG BC1/BDIR */
static void mux_sample(int sel, int8_t v)
{
	ptx_write(ORA, (uint8_t)v);
	ptx_write(ORB, (uint8_t)(sel << 1));      /* mux on: S/H follows the DAC */
	ptx_advance_cycles(2);
	ptx_write(ORB, 0x01);                     /* mux off: value held */
}

static void beam_zero(void)
{
	ptx_advance_cycles(20);                   /* let SR shift the beam off first */
	ptx_write(PCR, 0xCC);                     /* CA2 low: integrators to zero */
	ptx_advance_cycles(40);
	ptx_write(PCR, 0xCE);
	mux_sample(1, 0);                         /* zero reference */
}

/* one ramp: dx goes straight to the X integrator, dy via the Y sample-and-hold */
static void ramp(int8_t dx, int8_t dy, uint8_t scale, int lit)
{
	mux_sample(0, dy);
	ptx_write(ORA, (uint8_t)dx);
	ptx_write(SR, lit ? 0xFF : 0x00);         /* shift register drives CB2 = beam */
	ptx_write(T1L, scale);
	ptx_write(T1H, 0);                        /* T1 one-shot: PB7 low = RAMP */
	while (!(ptx_read(IFR) & 0x40))
		;
	ptx_write(SR, 0x00);
}

static uint8_t read_buttons(void)
{
	ptx_write(ORA, 14);                       /* PSG register 14 = port A */
	ptx_write(ORB, 0x19);                     /* BDIR=1 BC1=1: latch address */
	ptx_write(ORB, 0x01);
	ptx_write(DDRA, 0x00);
	ptx_write(ORB, 0x09);                     /* BDIR=0 BC1=1: read */
	uint8_t v = ptx_read(ORA);
	ptx_write(ORB, 0x01);
	ptx_write(DDRA, 0xFF);
	return (uint8_t)~v;                       /* active low -> 1 = pressed */
}

static void square(int8_t x, int8_t y, int8_t size)
{
	beam_zero();
	ramp(x, y, 0x7F, 0);
	ramp(size, 0, 0x7F, 1);
	ramp(0, size, 0x7F, 1);
	ramp((int8_t)-size, 0, 0x7F, 1);
	ramp(0, (int8_t)-size, 0x7F, 1);
}

int main(void)
{
	if (ptx_init()) return 1;

	ptx_write(DDRA, 0xFF);
	ptx_write(DDRB, 0x9F);
	ptx_write(ORB, 0x01);
	ptx_write(ACR, 0x98);                     /* T1 one-shot drives PB7; SR shift-out */
	ptx_write(SR, 0x00);
	mux_sample(2, 0x60);                      /* brightness (Z) */

	for (uint32_t frame = 0;; frame++) {
		ptx_write(T2L, 0x30);                 /* T2 = 30000 cycles = 50 Hz */
		ptx_write(T2H, 0x75);

		square(-60, -60, 120);                /* outer square */
		beam_zero(); ramp(-60, 0, 0x7F, 0); ramp(120, 0, 0x7F, 1);   /* cross */
		beam_zero(); ramp(0, -60, 0x7F, 0); ramp(0, 120, 0x7F, 1);

		ptx_input_t in;
		ptx_get_input(&in);
		beam_zero();                          /* joystick dot */
		ramp((int8_t)(in.pot[0] / 3), (int8_t)(in.pot[1] / 3), 0x7F, 0);
		ramp(2, 0, 0x10, 1);

		uint8_t b = read_buttons();
		for (int i = 0; i < 4; i++)           /* button marks under the square */
			if (b & (1 << i)) square((int8_t)(-45 + i * 25), -90, 12);

		while (!(ptx_read(IFR) & 0x20))       /* wait for the frame timer */
			;
		ptx_frame();

		if (frame % 50 == 0) {
			ptx_status_t st;
			ptx_get_status(&st);
			if (st.valid)
				printf("t=%" PRIu64 " core=%" PRIu64 " lead=%" PRId64 " fetched=%" PRIu64
				       " frames=%u underruns=%u pots=%d,%d buttons=%02x seq=%u\n",
				       ptx_now_cycles(), st.core_cycles,
				       (int64_t)(ptx_now_cycles() - st.core_cycles), st.rd_idx,
				       st.frames, st.underruns, in.pot[0], in.pot[1], b, in.seq);
			else
				printf("t=%" PRIu64 " (no core status: file backend)\n", ptx_now_cycles());
			fflush(stdout);
		}
	}
}
