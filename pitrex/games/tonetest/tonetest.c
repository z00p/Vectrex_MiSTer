/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/* PCM channel test (B6 cores): 4 s of a 440 Hz tone, 4 s of an 880 Hz tone, repeated for
 * about 2 minutes, at a loud level; prints the core's audio counters as it goes. */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include "ptx.h"

int main(void)
{
	if (ptx_init()) { fprintf(stderr, "tonetest: no stream\n"); return 1; }
	if (!ptx_audio_ready()) { fprintf(stderr, "tonetest: core has no PCM channel (needs B6)\n"); ptx_shutdown(); return 1; }
	uint32_t phase = 0, t = 0;
	for (int frame = 0; frame < 120 * 50; frame++) {
		uint32_t n = ptx_audio_space();
		int16_t buf[256];
		while (n > 0) {
			uint32_t k = n > 256 ? 256 : n;
			for (uint32_t i = 0; i < k; i++, t++) {
				uint32_t hz = (t / (4 * 32000)) & 1 ? 880 : 440;
				phase += hz * 65536u / 32000u;
				buf[i] = (phase & 0x8000) ? 16000 : -16000;
			}
			ptx_audio_write(buf, k);
			n -= k;
		}
		ptx_write(0xD008, 0x30);   /* VIA T2 reload: lets the core show a (blank) frame */
		ptx_write(0xD009, 0x75);
		ptx_frame();
		usleep(20000);              /* about 50 Hz in real time */
		if (frame % 100 == 0) fprintf(stderr, "tonetest: %d s, %u samples\n", frame / 50, t);
	}
	ptx_shutdown();
	return 0;
}
