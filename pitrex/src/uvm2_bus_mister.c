// SPDX-License-Identifier: GPL-2.0-or-later
/* UVMC2 mode: the UVM2 runtime's bus layer on the PiTrex-on-MiSTer stream.
 *
 * The UVM2 runtime (uvm2_draw.c, from the UVMC2 team's SDK, github.com/tullulah/uvmc2-sdk)
 * builds each frame as a list of 3-byte commands {delay:12, reg:4, data:8}. On the cartridge
 * an executor replays it one VIA write per E period, then parks for `delay` periods. Here each
 * command becomes a ring WRITE to $D000+reg followed by `delay` E cycles of stream time, which
 * is the same timing: ptx_write() itself occupies one E cycle.
 *
 * Built with -DUVM2_HOST -DUVM2_BENCH_NO_CORE1 (single core: uvm2_frame_end calls uvm2_exec
 * directly). The core needs nothing new: this writes the same ring as PiTrex programs. */
#include <stdint.h>
#include "ptx.h"

void uvm2_mister_frame(void);

uint32_t uvm2_exec(const uint8_t *c, uint32_t n)
{
	uint32_t cycles = 0;
	for (uint32_t i = 0; i < n; i++, c += 3) {
		uint32_t v = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16);
		uint32_t delay = v >> 12, reg = (v >> 8) & 0x0Fu, data = v & 0xFFu;
		ptx_write((uint16_t)(0xD000u | reg), (uint8_t)data);
		if (delay) ptx_advance_cycles(delay);
		cycles += delay + 1u;
	}
	uvm2_mister_frame();   /* uvm2_exec runs once per frame, from uvm2_frame_end */
	return cycles;
}

void     uvm2_bus_delay(uint32_t c) { ptx_advance_cycles(c); }
void     uvm2_via_write(uint32_t r, uint32_t d) { ptx_write((uint16_t)(0xD000u | (r & 0x0Fu)), (uint8_t)d); }
uint8_t  uvm2_via_read(uint32_t r) { return ptx_read((uint16_t)(0xD000u | (r & 0x0Fu))); }
uint32_t uvm2_now_us(void) { return ptx_now_us(); }

/* End of a UVMC2 frame on MiSTer: reload VIA Timer 2 as the BIOS does each frame (30000 E
 * cycles = 50 Hz). Videodr0me's renderer takes accepted T2 reloads as frame boundaries, and the
 * UVM2 runtime never touches T2, so without this the display keeps showing the previous image.
 * Then the usual ring pacing. */
void uvm2_mister_frame(void)
{
	ptx_write(0xD008, 0x30);
	ptx_write(0xD009, 0x75);
	ptx_frame();
}
