/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * um2bus_replay -- host tool: feed a capture recorded by `um2run --bus --capture FILE` through the device model and the
 * ptx file backend, producing a .ptxs stream for tools/ptxplay.  No ARM, no image: the PC half of the "capture once,
 * develop the translator offline" workflow.
 *   um2bus_replay <capture> <out.ptxs> [max_frames]       (env UM2BUS_NOFAST=1 forces the PIO interpreter)
 */
#include "um2bus.h"
#include "ptx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void k_write(uint16_t a, uint8_t d) { ptx_write(a, d); }
static uint8_t k_read(uint16_t a) { return ptx_read(a); }
static void k_advance(uint32_t c) { ptx_advance_cycles(c); }
static uint64_t k_now(void) { return ptx_now_cycles(); }
static void k_frame(void) { ptx_frame(); }

int main(int argc, char **argv) {
	if (argc < 3) { fprintf(stderr, "usage: %s <capture> <out.ptxs> [max_frames]\n", argv[0]); return 2; }
	setenv("PTX_STREAM", argv[2], 1);
	setenv("PTX_MAX_FRAMES", argc > 3 ? argv[3] : "100000000", 1);
	setenv("PTX_BACKEND", "file", 1);
	if (ptx_init()) return 1;
	um2bus_cfg_t c; memset(&c, 0, sizeof c);
	c.vwrite = k_write; c.vread = k_read; c.advance = k_advance; c.now = k_now; c.frame = k_frame;
	c.frame_cycles = 30000; c.verbose = getenv("UM2BUS_VERBOSE") ? 1 : 0;
	um2bus_init(&c);
	int r = um2bus_replay(argv[1]);
	um2bus_stats(stderr);
	ptx_shutdown();
	return r;
}
