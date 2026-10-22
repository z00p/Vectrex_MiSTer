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

/* Sound: 0 = the cartridge's own audio jack, 1 = the console's chip. On a B6 core the "jack"
 * is the core's PCM channel (16-bit, 32 kHz, the jack's own rate), chosen in main() below;
 * older cores use the Vectrex's PSG. UVMC2_AUDIO=psg forces the PSG. */
volatile int32_t uvm2_setting_audio = 1;
void uvm2_config_game(const char *name, unsigned settings) { (void)name; (void)settings; }

/* The cartridge's audio jack: the core's PCM channel (B6). */
int  uvm2_jack_init(void) { return ptx_audio_ready(); }
int  uvm2_jack_space(void) { return (int)ptx_audio_space(); }
void uvm2_jack_write(const int16_t *s, int n) { if (n > 0) ptx_audio_write(s, (uint32_t)n); }
/* stereo games: the PCM channel is mono, so left and right are averaged */
void uvm2_jack_write_lr(const int16_t *l, const int16_t *r, int n)
{
	int16_t m[256];
	while (n > 0) {
		int k = n > 256 ? 256 : n;
		for (int i = 0; i < k; i++) m[i] = (int16_t)(((int32_t)l[i] + r[i]) >> 1);
		ptx_audio_write(m, (uint32_t)k);
		l += k; r += k; n -= k;
	}
}

/* PSRAM: games address it through overridable macros (src/uvmc2_psram.h points them here). */
unsigned char uvmc2_psram[4u << 20];
int  uvm2_psram_ready = 1;
#include <time.h>
uint32_t uvmc2_now_us(void)          /* wall clock, for games that budget time per frame */
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((uint64_t)t.tv_sec * 1000000u + (uint64_t)t.tv_nsec / 1000u);
}
unsigned long uvm2_romzip_desc = 0;
#include "uvm2_sd.h"
struct uvm2_sd_diag uvm2_sd_diag;
/* Streaming reads of "card" files from $UVMC2_DIR (e.g. a game's PCM bundle). The FILE*
 * lives in the struct's spare storage. */
static FILE *uvmc2_fopen(const char *path, char *full, size_t fullsz);
int uvm2_sd_open(const char *path, uvm2_sd_file *f)
{
	char full[512];
	FILE *fp = uvmc2_fopen(path, full, sizeof full);
	f->ok = 0;
	if (!fp) { fprintf(stderr, "uvmc2: %s not found\n", path); return 0; }
	fseek(fp, 0, SEEK_END); f->len = (uint32_t)ftell(fp); fseek(fp, 0, SEEK_SET);
	f->pos = 0;
	f->fil[0] = (uint64_t)(uintptr_t)fp;
	f->ok = 1;
	fprintf(stderr, "uvmc2: streaming %s (%u bytes)\n", full, (unsigned)f->len);
	return 1;
}
uint32_t uvm2_sd_next(uvm2_sd_file *f, unsigned char *d, uint32_t m)
{
	if (!f->ok) return 0;
	uint32_t got = (uint32_t)fread(d, 1, m, (FILE *)(uintptr_t)f->fil[0]);
	f->pos += got;
	if (got == 0) f->ok = 0;
	return got;
}
void uvm2_sd_close(uvm2_sd_file *f)
{
	if (f->fil[0]) fclose((FILE *)(uintptr_t)f->fil[0]);
	f->fil[0] = 0;
	f->ok = 0;
}
/* Data files a game reads from "the card" (e.g. a sample bundle): from the game's folder,
 * $UVMC2_DIR (the launcher sets it), trying the name as given, then upper and lower case. */
#include <ctype.h>
#include <string.h>
static FILE *uvmc2_fopen(const char *path, char *full, size_t fullsz)
{
	const char *dir = getenv("UVMC2_DIR");
	char name[256];
	for (int pass = 0; pass < 3; pass++) {
		size_t i;
		for (i = 0; path[i] && i < sizeof name - 1; i++)
			name[i] = pass == 1 ? (char)toupper((unsigned char)path[i])
			        : pass == 2 ? (char)tolower((unsigned char)path[i]) : path[i];
		name[i] = 0;
		snprintf(full, fullsz, "%s/%s", dir ? dir : ".", name);
		FILE *f = fopen(full, "rb");
		if (f) return f;
	}
	return NULL;
}
uint32_t uvm2_sd_read_from(const char *path, unsigned char *dst, uint32_t max, uint32_t from)
{
	const char *dir = getenv("UVMC2_DIR");
	char full[512];
	{
		FILE *f = uvmc2_fopen(path, full, sizeof full);
		if (!f) goto missing;
		uint32_t got = 0;
		if (fseek(f, (long)from, SEEK_SET) == 0) got = (uint32_t)fread(dst, 1, max, f);
		fclose(f);
		fprintf(stderr, "uvmc2: read %s: %u bytes\n", full, (unsigned)got);
		return got;
	}
missing:
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
	const char *a = getenv("UVMC2_AUDIO");
	uvm2_setting_audio = (ptx_audio_ready() && !(a && !strcmp(a, "psg"))) ? 0 : 1;
	fprintf(stderr, "uvmc2: sound through the %s\n", uvm2_setting_audio ? "PSG" : "core's PCM channel");
	uvm2_runtime_init();
	int r = uvm2_game_main();
	ptx_shutdown();
	return r;
}
