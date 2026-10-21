// SPDX-License-Identifier: GPL-2.0-or-later
/* UVMC2 on MiSTer: the program entry and the cartridge-only parts of the UVM2 runtime.
 *
 * As on the cartridge (uvm2_pico_main.c), the game is compiled with -Dmain=uvm2_game_main and
 * this file supplies the real main(): open the stream, bring the runtime up, run the game.
 * The hardware bring-up the runtime asks for (clocks, pads, LED, taking the bus) has no
 * counterpart here: the core holds the 6809 while a program streams. Of the calibration the
 * cartridge keeps in flash, only the beam scale is set (uvm2_config_load below). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "ptx.h"
#include "uvm2_bus.h"

uvm2_stats_t uvm2_stats;
volatile int uvm2_have_calibration = 1;   /* no first-run calibration wizard */

void     uvm2_cpu_init(void) {}
void     uvm2_set_sys_clock(void) {}
void     uvm2_bus_pads(void) {}
void     uvm2_bus_halt(void) {}
void     uvm2_led_init(void) {}
void     uvm2_led_status(int s) { (void)s; }
uint32_t uvm2_clock_calibrate(void) { return 1; }
void     uvm2_hud_frame(void) {}
/* THE CONSOLE CALIBRATION. On the cartridge it lives in flash (uvm2_config.c) and is set
 * per console by a wizard; the beam scale is part of it. Videodr0me's core is our console:
 * at the cartridge's stock scale (160) the picture comes out about a third too large, so the
 * MiSTer default is smaller. Override in /media/fat/pitrex/uvmc2.cfg ("scale=120") or with
 * UVMC2_SCALE=<n> in the environment. LARGER = bigger picture on this core. */
#define UVMC2_MISTER_SCALE 120u
#define UVMC2_CFG "/media/fat/pitrex/uvmc2.cfg"
extern volatile uint32_t DRAW_SCALE;   /* vectrex-draw */
int uvm2_config_load(void)
{
	uint32_t scale = UVMC2_MISTER_SCALE;
	FILE *f = fopen(UVMC2_CFG, "r");
	if (f) {
		char line[80]; unsigned v;
		while (fgets(line, sizeof line, f))
			if (sscanf(line, " scale = %u", &v) == 1 && v >= 32 && v <= 255) scale = v;
		fclose(f);
	}
	const char *e = getenv("UVMC2_SCALE");
	if (e && atoi(e) >= 32 && atoi(e) <= 255) scale = (uint32_t)atoi(e);
	DRAW_SCALE = scale;
	return 1;
}
int      uvm2_config_wizard(void) { return 0; }
void     rust_eh_personality(void) {}

/* Sound: 0 = the cartridge's own audio jack, 1 = the console's chip. There is no jack here,
 * so games that offer both use the Vectrex's PSG and DAC. */
volatile int32_t uvm2_setting_audio = 1;
void uvm2_config_game(const char *name, unsigned settings) { (void)name; (void)settings; }

/* The cartridge's audio jack, PSRAM and SD card: not present. */
int  uvm2_jack_init(void) { return 0; }
int  uvm2_jack_space(void) { return 0; }
void uvm2_jack_write(const int16_t *s, int n) { (void)s; (void)n; }
int  uvm2_psram_ready = 0;
unsigned long uvm2_romzip_desc = 0;
#include "uvm2_sd.h"
struct uvm2_sd_diag uvm2_sd_diag;
int      uvm2_sd_open(const char *path, uvm2_sd_file *f) { (void)path; f->ok = 0; return 0; }
uint32_t uvm2_sd_next(uvm2_sd_file *f, unsigned char *d, uint32_t m) { (void)f; (void)d; (void)m; return 0; }
void     uvm2_sd_close(uvm2_sd_file *f) { f->ok = 0; }
/* Data files a game reads from "the card" (e.g. a sample bundle): from the game's folder,
 * $UVMC2_DIR (the launcher sets it), trying the name as given, then upper and lower case. */
#include <ctype.h>
#include <string.h>
uint32_t uvm2_sd_read_from(const char *path, unsigned char *dst, uint32_t max, uint32_t from)
{
	const char *dir = getenv("UVMC2_DIR");
	char name[256], full[512];
	for (int pass = 0; pass < 3; pass++) {
		size_t i;
		for (i = 0; path[i] && i < sizeof name - 1; i++)
			name[i] = pass == 1 ? (char)toupper((unsigned char)path[i])
			        : pass == 2 ? (char)tolower((unsigned char)path[i]) : path[i];
		name[i] = 0;
		snprintf(full, sizeof full, "%s/%s", dir ? dir : ".", name);
		FILE *f = fopen(full, "rb");
		if (!f) continue;
		uint32_t got = 0;
		if (fseek(f, (long)from, SEEK_SET) == 0) got = (uint32_t)fread(dst, 1, max, f);
		fclose(f);
		fprintf(stderr, "uvmc2: read %s: %u bytes\n", full, (unsigned)got);
		return got;
	}
	fprintf(stderr, "uvmc2: %s not found in %s\n", path, dir ? dir : ".");
	return 0;
}
int      uvm2_sd_write(const char *path, const unsigned char *d, uint32_t n) { (void)path; (void)d; (void)n; return 0; }

/* Samples to leave out, by bundle index: UVMC2_MUTE=0,1,2 (set per game by its launcher,
 * e.g. Tac/Scan's continuous ship roar). Called from v_playSample (tools/patch_uvmc2.py). */
int uvmc2_sample_muted(int idx)
{
	static int parsed;
	static unsigned char muted[256];
	if (!parsed) {
		const char *e = getenv("UVMC2_MUTE");
		while (e && *e) {
			char *end;
			long v = strtol(e, &end, 10);
			if (end == e) break;
			if (v >= 0 && v < 256) muted[v] = 1;
			e = (*end == ',') ? end + 1 : end;
		}
		parsed = 1;
	}
	return idx >= 0 && idx < 256 && muted[idx];
}

void uvm2_runtime_init(void);   /* uvm2_svc.c */
int  uvm2_game_main(void);      /* the game's main(), renamed at compile time */

#undef main
int main(void)
{
	if (ptx_init()) { fprintf(stderr, "uvmc2: no stream\n"); return 1; }
	uvm2_runtime_init();
	int r = uvm2_game_main();
	ptx_shutdown();
	return r;
}
