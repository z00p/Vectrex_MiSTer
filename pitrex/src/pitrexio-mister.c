/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * pitrexio-mister.c - drop-in replacement for Kevin Koster's pitrexio-gpio.c.
 *
 * Same API (pitrex/pitrexio-gpio.h), but nothing touches GPIO: writes become
 * stream commands and reads are answered by the shadow VIA in ptx_core.c.
 * Begin/end pairs complete immediately, because there is no bus to wait for.
 */
#include <pitrex/pitrexio-gpio.h>
#include "ptx.h"

#include <stdlib.h>

static unsigned char pending_read;

unsigned char vectrexread_short(unsigned int address) { return ptx_read((uint16_t)address); }
void vectrexwrite_short(unsigned int address, unsigned char data) { ptx_write((uint16_t)address, data); }

char vectrexread_begin(unsigned int address)
{
	pending_read = ptx_read((uint16_t)address);
	return 1;
}
unsigned char vectrexread_end(void) { return pending_read; }
unsigned char vectrexread(unsigned int address) { return ptx_read((uint16_t)address); }

/* "On IRQ" variants wait for the VIA interrupt first. The only IRQ source is
   the VIA, so advance stream time until an enabled flag is set (bounded). */
static void wait_irq(void)
{
	for (int i = 0; i < 0x20000; i++)
		if (ptx_read(0xD00D) & 0x80) return;
}

char vectrexread_begin_on_irq(unsigned int address) { wait_irq(); return vectrexread_begin(address); }
unsigned char vectrexread_on_irq(unsigned int address) { wait_irq(); return ptx_read((uint16_t)address); }

char vectrexwrite_begin(unsigned int address, unsigned char data) { ptx_write((uint16_t)address, data); return 1; }
char vectrexwrite_end(char loop) { (void)loop; return 1; }
void vectrexwrite(unsigned int address, unsigned char data) { ptx_write((uint16_t)address, data); }

char vectrexwrite_begin_on_irq(unsigned int address, unsigned char data) { wait_irq(); return vectrexwrite_begin(address, data); }
void vectrexwrite_on_irq(unsigned int address, unsigned char data) { wait_irq(); ptx_write((uint16_t)address, data); }

void vectrexinterrupt_begin(void) {}
char vectrexinterrupt_end(char interrupt, char loop) { (void)interrupt; (void)loop; return 1; }
void vectrexinterrupt(char interrupt) { (void)interrupt; wait_irq(); }

int vectrexinit(char viaconfig)
{
	(void)viaconfig;
	if (ptx_init()) return -1;
	atexit(ptx_shutdown);
	return 0;
}

int vectrexclose(void) { ptx_shutdown(); return 0; }
