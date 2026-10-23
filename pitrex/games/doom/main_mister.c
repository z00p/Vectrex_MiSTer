/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * main_mister.c - PureDOOM-pitrex entry point for "PiTrex on MiSTer" (Linux).
 * Based on upstream src/main.c (Daniel Ferrer's wireframe DOOM for the PiTrex,
 * GPL-2): same draw callback, scaling and frame loop; the bare-metal parts
 * (LED blink, UART logger, bump heap, FatFs, boot vector test) are gone.
 *
 *   WAD:   $DOOM_WAD, else /media/fat/games/Vectrex/DOOM/doom1.wad, else ./doom1.wad
 *   Saves: $DOOM_HOME (default /media/fat/games/Vectrex/DOOM): .doomrc and doomsav*.dsg
 *   Rate:  $DOOM_HZ vector refresh (default 50; upstream used 60)
 *   $DOOM_STATS=1 prints vectors/frame statistics to stderr on exit.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#include "PureDOOM.h"       /* declarations; implementation is in renderer.c */
#include "platform.h"
#include "renderer.h"
#include <pitrex/pitrexio-gpio.h>
#include <vectrex/vectrexInterface.h>

extern const char *doom_home_dir(void);
extern const char *platform_wad_path;

static long s_vectors, s_frames, s_max_vectors;
static long s_frame_vectors;

/* Layout and brightness for the MiSTer core (reversible from the environment):
 *   DOOM_YSCALE  half-height of DOOM's 320x200 frame in PiTrex units. 13500 keeps
 *                DOOM's 4:3 picture over the full 36000-unit width (upstream: 24000,
 *                which stretches it over the whole portrait screen)
 *   DOOM_YOFF    vertical shift of the frame, + is up (upstream: 0)
 *   DOOM_BRIGHT  line intensity 1..127; lower draws thinner lines (upstream: 64)
 *   DOOM_CLASSIC=1  upstream layout and brightness */
static int s_yscale = 13500, s_yoff = 6000, s_bright = 48;

static void pitrex_draw_line(int x0, int y0, int x1, int y1)
{
	s_frame_vectors++;
	v_directDraw32((int32_t)x0 * 18000 / 127, (int32_t)y0 * s_yscale / 127 + s_yoff,
	               (int32_t)x1 * 18000 / 127, (int32_t)y1 * s_yscale / 127 + s_yoff, s_bright);
}

static int s_stats;

static void print_stats(void)
{
	if (s_stats && s_frames)
		fprintf(stderr, "doom: %ld frames, %.1f vectors/frame avg, %ld max\n",
		        s_frames, (double)s_vectors / s_frames, s_max_vectors);
}

/* The upstream bump heap was zero-filled static memory: keep that guarantee. */
static void *heap_malloc(int n) { return calloc(1, (size_t)n); }
static void heap_free(void *p) { free(p); }

static void on_exit_cb(int code) { exit(code); }

static int find_wad(char *out, size_t n)
{
	const char *cand[3];
	const char *e = getenv("DOOM_WAD");
	int nc = 0;
	if (e && *e) cand[nc++] = e;
	cand[nc++] = "/media/fat/games/Vectrex/DOOM/doom1.wad";
	cand[nc++] = "./doom1.wad";
	for (int i = 0; i < nc; i++) {
		char rp[PATH_MAX];
		if (access(cand[i], R_OK) == 0 && realpath(cand[i], rp)) {
			snprintf(out, n, "%s", rp);
			return 1;
		}
		if (i == 0 && e && *e)       /* explicit $DOOM_WAD that is unusable: say so */
			fprintf(stderr, "doom: $DOOM_WAD=%s is not readable, trying defaults\n", e);
	}
	return 0;
}

int main(int argc, char **argv)
{
	char wad[PATH_MAX];
	if (!find_wad(wad, sizeof wad)) {
		fprintf(stderr,
		        "doom: no IWAD found. Put doom1.wad (shareware or retail) in "
		        "/media/fat/games/Vectrex/DOOM/ or set DOOM_WAD=/path/to/doom1.wad "
		        "(also tried ./doom1.wad).\n");
		return 2;
	}

	platform_wad_path = wad;

	/* Save/config dir: DOOM reads/writes .doomrc and doomsav*.dsg relative to
	   HOME (via getenv callback) and the cwd respectively. */
	const char *home = doom_home_dir();
	mkdir(home, 0755);
	if (chdir(home) != 0)
		fprintf(stderr, "doom: cannot use save dir %s (saves go to cwd)\n", home);

	const char *hz = getenv("DOOM_HZ");
	int refresh = (hz && atoi(hz) > 0) ? atoi(hz) : 50;

	s_stats = getenv("DOOM_STATS") != NULL;
	atexit(print_stats);

	vectrexinit(1);
	v_setName("doom");
	v_init();
	v_setRefresh(refresh);

	doom_set_malloc(heap_malloc, heap_free);
	doom_set_file_io(platform_open, platform_close, platform_read, platform_write,
	                 platform_seek, platform_tell, platform_eof);
	doom_set_gettime(platform_gettime);
	doom_set_print(platform_print);
	doom_set_getenv(platform_getenv);
	doom_set_exit(on_exit_cb);

	int flags = DOOM_FLAG_HIDE_MOUSE_OPTIONS | DOOM_FLAG_HIDE_SOUND_OPTIONS |
	            DOOM_FLAG_HIDE_MUSIC_OPTIONS;
	doom_set_resolution(320, 200);
	{
		const char *e;
		if ((e = getenv("DOOM_YSCALE")) && atoi(e) >= 4000 && atoi(e) <= 30000) s_yscale = atoi(e);
		if ((e = getenv("DOOM_YOFF")) && abs(atoi(e)) <= 20000) s_yoff = atoi(e);
		if ((e = getenv("DOOM_BRIGHT")) && atoi(e) >= 1 && atoi(e) <= 127) s_bright = atoi(e);
		if ((e = getenv("DOOM_CLASSIC")) && *e == '1') { s_yscale = 24000; s_yoff = 0; s_bright = 64; }
	}
	renderer_init(pitrex_draw_line);

	char *doom_argv[] = { "doom", "-nomusic", "-nosound" };
	doom_init(3, doom_argv, flags);

	for (;;) {
		v_WaitRecal();
		platform_poll_input();
		s_frame_vectors = 0;
		renderer_scene_begin();
		doom_update();
		renderer_scene_end();
		renderer_draw_sprites();
		renderer_draw_weapons();
		renderer_frame_end();
		s_frames++;
		s_vectors += s_frame_vectors;
		if (s_frame_vectors > s_max_vectors) s_max_vectors = s_frame_vectors;
		if (s_stats && (s_frames % 250) == 0) print_stats();
	}
	return 0;
}
