// SPDX-License-Identifier: GPL-2.0-or-later
/* UVMC2 mode, phase 1: the UVM2 runtime's drawing (uvm2_draw.c + vectrex-draw, by the UVMC2
 * team) on the PiTrex-on-MiSTer stream. Draws a frame box, a spinning square and a star.
 * UVMC2_SCALE=<n> sets the beam model's DRAW_SCALE; UVMC2_BOX=<units> the frame box (default 110).
 * No sound or samples yet (stubs below). Run: PTX_BACKEND=mister ./uvmc2_lines [frames] */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include "ptx.h"
#include "uvm2_bus.h"
#include "uvm2_draw.h"

void uvm2_mister_frame(void);   /* uvm2_bus_mister.c */

/* runtime pieces not used in phase 1 (sound, samples, settings) */
void uvm2_config_load(void) {}
unsigned uvm2_smp_s_active = 0;
int  uvm2_smp_due(uint32_t c, uint8_t *v) { (void)c; (void)v; return 0; }
uint8_t uvm2_smp_mixer(void) { return 0; }
int  uvm2_smp_needs_latch(void) { return 0; }
void uvm2_smp_frame(uint32_t c) { (void)c; }
uint32_t uvm2_smp_injected = 0;
void rust_eh_personality(void) {}
uvm2_stats_t uvm2_stats;
void uvm2_hud_frame(void) {}
volatile int uvm2_have_calibration = 0;

#define Q4(v) ((int)lrintf((v) * 16.0f))

static void poly(const float *px, const float *py, int n)
{
	uvm2_draw_move_abs_q4(Q4(px[0]), Q4(py[0]));
	for (int i = 1; i <= n; i++) {
		int j = i % n, k = i - 1;
		uvm2_draw_delta_q4(Q4(px[j]) - Q4(px[k]), Q4(py[j]) - Q4(py[k]));
	}
}

extern volatile uint32_t DRAW_SCALE;   /* vectrex-draw */

int main(int argc, char **argv)
{
	const char *e = getenv("UVMC2_BOX");
	float box = e ? (float)atof(e) : 110.0f;
	int frames = argc > 1 ? atoi(argv[1]) : 0;   /* 0 = run until stopped */
	if (ptx_init()) { fprintf(stderr, "uvmc2_lines: no stream\n"); return 1; }
	uvm2_draw_init();
	if ((e = getenv("UVMC2_SCALE"))) DRAW_SCALE = (uint32_t)atoi(e);
	for (int f = 0; frames == 0 || f < frames; f++) {
		uvm2_frame_begin();
		uvm2_draw_intensity(0x5F);
		const float bx[4] = { -box, box, box, -box }, by[4] = { -box, -box, box, box };
		poly(bx, by, 4);

		float a = f * 0.03f, sx[4], sy[4];
		for (int i = 0; i < 4; i++) {
			sx[i] = 60.0f * cosf(a + i * 1.5707963f);
			sy[i] = 60.0f * sinf(a + i * 1.5707963f) + 30.0f;
		}
		uvm2_draw_intensity(0x7F);
		poly(sx, sy, 4);

		float tx[5], ty[5];
		for (int i = 0; i < 5; i++) {
			float t = -a * 1.5f + i * 2.5132741f;   /* every second point of a pentagon */
			tx[i] = 35.0f * cosf(t);
			ty[i] = 35.0f * sinf(t) - 70.0f;
		}
		uvm2_draw_intensity(0x6F);
		poly(tx, ty, 5);

		uvm2_frame_end();   /* replays the list, reloads T2, paces */
	}
	ptx_shutdown();
	return 0;
}
