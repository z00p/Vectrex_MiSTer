/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * um2bus -- device model for the RP2350 pieces a UVMC2 .um2 image uses to drive the Vectrex bus,
 * turning that traffic into timed VIA reads/writes (our PiTrex stream).  See DESIGN.md.
 *
 *   DMA (16 channels, run synchronously on trigger) -> PIO0 TXF0 -> a small PIO interpreter that
 *   executes the program the image uploads (one bus cycle per E period, 100 PIO cycles per E)
 *   SIO GPIO + a synthetic clock (bit 31) -> the bit-banged single accesses (controllers, PSG)
 *   SIO FIFO + spinlocks + core-1 launch
 *
 * The model has no dependency on the loader: guest memory, the core-1 launch and the output are
 * hooks, so it also runs on the PC against a recorded capture (um2bus_replay.c).
 */
#ifndef UM2BUS_H
#define UM2BUS_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
	/* output: one bus cycle each (the sink accounts the cycle itself, like ptx_write) */
	void     (*vwrite)(uint16_t addr, uint8_t data);
	uint8_t  (*vread)(uint16_t addr);
	void     (*advance)(uint32_t cycles);        /* idle E cycles */
	uint64_t (*now)(void);                       /* sink stream time in E cycles */
	void     (*frame)(void);                     /* frame boundary (after the T2 reload) */
	/* guest memory: host pointer for [addr, addr+len) if it is plain RAM, else NULL */
	uint8_t *(*host_ptr)(uint32_t addr, uint32_t len);
	/* anything the model does not own goes back to the loader's generic MMIO */
	uint32_t (*ext_load)(uint32_t addr, int size);
	void     (*ext_store)(uint32_t addr, int size, uint32_t val);
	/* bootrom protocol finished: start core 1 */
	void     (*launch_core1)(uint32_t vtor, uint32_t sp, uint32_t entry);
	int      verbose;
	uint32_t frame_cycles;                       /* stream cycles per frame (30000 = 50 Hz); 0 = no padding */
	volatile uint32_t *dma_mirror;               /* RW alias of the page mapped read-only at 0x50000000 (or NULL: every DMA register read traps) */
	const char *capture_path;                    /* record the model's inputs for replay, or NULL */
	double   (*sysclk)(void);                    /* clk_sys in Hz as the image configured it (NULL: 300 MHz); sets the cart-audio sample rate */
	void     (*irq_poke)(void);                  /* an interrupt line owned by the model changed level (NULL: the replay tool) */
} um2bus_cfg_t;

void um2bus_init(const um2bus_cfg_t *cfg);
/* return 1 if the model owns the address (the access was performed), else 0 */
int  um2bus_store(uint32_t addr, int size, uint32_t val, int core);
int  um2bus_load(uint32_t addr, int size, uint32_t *val, int core);
/* level of the interrupt lines the model owns (bit n = NVIC IRQ n): DMA_IRQ_0..3 = IRQ 10..13 */
uint64_t um2bus_irq_lines(void);
void um2bus_stats(FILE *f);
/* E-clock periods (falling edges of the GPIO31 input) the guest has polled so far */
uint64_t um2bus_edges(void);
/* pio disassembly for logs */
const char *um2bus_pio_dis(uint16_t ins, char *buf, size_t n);
/* replay a capture file through the model (host tool); returns 0 on success */
int  um2bus_replay(const char *path);

#endif
