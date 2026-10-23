/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * platform_mister.c - PureDOOM platform layer for "PiTrex on MiSTer" (Linux).
 *
 * Replaces upstream src/platform.c (bare-metal FatFs + v_millis) with plain
 * libc: stdio file I/O, clock_gettime, getenv-backed HOME.  Input mapping is
 * upstream's (stick = move/turn, buttons 1-3 = fire/use/run, button 4 = strafe)
 * plus menu access, see platform_poll_input().
 *
 * Interface is upstream's platform.h.  GPL-2 like the rest of PureDOOM-pitrex.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "platform.h"      /* upstream src/platform.h (pulls in PureDOOM.h) */
#include <vectrex/vectrexInterface.h>

/* ---- save/config directory: $DOOM_HOME, default /media/fat/games/Vectrex/DOOM ---- */
const char *doom_home_dir(void)
{
	const char *h = getenv("DOOM_HOME");
	return (h && *h) ? h : "/media/fat/games/Vectrex/DOOM";
}

void platform_init(void) {}

/* ---- file I/O ---- */
/* PureDOOM's IdentifyVersion() probes "$DOOMWADDIR/doom1.wad", doom.wad, ... and
   ignores -iwad.  platform_wad_path (set by main) is the IWAD we located; a probe
   for an IWAD name opens it only if the name matches it (case-insensitively), so
   the right game mode is picked wherever the file lives and however it is cased. */
const char *platform_wad_path;

static const char *base_name(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

void *platform_open(const char *path, const char *mode)
{
	if (platform_wad_path && mode[0] == 'r') {
		static const char *const iwads[] = { "doom.wad", "doomu.wad", "doom1.wad",
			"doom2.wad", "doom2f.wad", "plutonia.wad", "tnt.wad", NULL };
		const char *b = base_name(path);
		for (int i = 0; iwads[i]; i++)
			if (!strcasecmp(b, iwads[i]))
				return strcasecmp(b, base_name(platform_wad_path)) ? NULL
				     : fopen(platform_wad_path, mode);
	}
	return fopen(path, mode);
}
void platform_close(void *h) { if (h) fclose((FILE *)h); }
int platform_read(void *h, void *buf, int n)
{
	return (int)fread(buf, 1, (size_t)n, (FILE *)h);
}
int platform_write(void *h, const void *buf, int n)
{
	return (int)fwrite(buf, 1, (size_t)n, (FILE *)h);
}
int platform_seek(void *h, int off, doom_seek_t origin)
{
	int w = origin == DOOM_SEEK_CUR ? SEEK_CUR : origin == DOOM_SEEK_END ? SEEK_END : SEEK_SET;
	return fseek((FILE *)h, off, w) == 0 ? 0 : -1;
}
int platform_tell(void *h) { return (int)ftell((FILE *)h); }
int platform_eof(void *h) { return feof((FILE *)h); }

/* ---- time ----
 * Wall clock (CLOCK_MONOTONIC).  For deterministic harness runs, a virtual
 * clock can be selected with DOOM_VCLOCK=<hz>: time then advances one frame
 * (1/hz s) per platform_poll_input() call instead of with real time. */
static long vclock_frames;
static int vclock_hz = -1;

void platform_gettime(int *sec, int *usec)
{
	if (vclock_hz < 0) {
		const char *v = getenv("DOOM_VCLOCK");
		vclock_hz = (v && atoi(v) > 0) ? atoi(v) : 0;
	}
	if (vclock_hz > 0) {
		*sec = (int)(vclock_frames / vclock_hz);
		*usec = (int)((vclock_frames % vclock_hz) * 1000000L / vclock_hz);
		return;
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*sec = (int)ts.tv_sec;
	*usec = (int)(ts.tv_nsec / 1000);
}

/* ---- environment / print ---- */
char *platform_getenv(const char *var)
{
	if (var && !strcmp(var, "HOME")) return (char *)doom_home_dir();
	return NULL;      /* DOOMWADDIR unset = "."; platform_open() redirects IWAD probes */
}
void platform_print(const char *msg) { fputs(msg, stderr); }

/* ---- input ----
 * Stick: up/down = forward/back, left/right = turn (upstream's mapping).
 * In game:   B1 fire (Ctrl), B2 use (Space), B3 run (Shift),
 *            B4 strafe (Alt) held; a quick tap of B4 (< 12 frames, stick not
 *            moved sideways) taps Escape = open the menu.
 * In menu:   stick = menu cursor, B1 Enter (select), B2 Backspace (back),
 *            B3 'y' (confirm quit/yes), B4 Escape (close menu).
 * Which key a button sent is remembered so the same key is released. */
extern doom_boolean menuactive;
#define STICK_THRESH 40
#define TAP_FRAMES   12

void platform_poll_input(void)
{
	static unsigned prev_btns;
	static int prev_x, prev_y;
	static int sent[4];             /* key sent by each button (0 = none) */
	static int hold4;               /* frames B4 has been held */
	static int b4_moved;            /* stick sideways during B4 hold */
	static int esc_up_pending;
	vclock_frames++;

	v_readButtons();
	v_readJoystick1Analog();
	unsigned btns = currentButtonState & 0x0F;
	/* Upstream's stick test runs backwards on this setup (tested on the MiSTer: pulling
	 * down walked forward), while other PiTrex games read up as positive; invert here. */
	int jx = (signed char)currentJoy1X, jy = -(int)(signed char)currentJoy1Y;

	if (esc_up_pending) { doom_key_up(DOOM_KEY_ESCAPE); esc_up_pending = 0; }

	static const doom_key_t game_key[4] = { DOOM_KEY_CTRL, DOOM_KEY_SPACE, DOOM_KEY_SHIFT, DOOM_KEY_ALT };
	static const doom_key_t menu_key[4] = { DOOM_KEY_ENTER, DOOM_KEY_BACKSPACE, DOOM_KEY_Y, DOOM_KEY_ESCAPE };
	for (int i = 0; i < 4; i++) {
		unsigned m = 1u << i;
		int now = (btns & m) != 0, was = (prev_btns & m) != 0;
		if (now && !was) {
			sent[i] = menuactive ? menu_key[i] : game_key[i];
			doom_key_down(sent[i]);
			if (i == 3) { hold4 = 0; b4_moved = 0; }
		} else if (!now && was) {
			if (sent[i]) doom_key_up(sent[i]);
			if (i == 3 && sent[i] == DOOM_KEY_ALT && hold4 < TAP_FRAMES && !b4_moved) {
				doom_key_down(DOOM_KEY_ESCAPE);      /* tap = menu */
				esc_up_pending = 1;
			}
			sent[i] = 0;
		}
	}
	if (btns & 8) { hold4++; if (jx > STICK_THRESH || jx < -STICK_THRESH) b4_moved = 1; }

	if (jy >  STICK_THRESH && prev_y <=  STICK_THRESH) doom_key_down(DOOM_KEY_UP_ARROW);
	if (jy <= STICK_THRESH && prev_y >   STICK_THRESH) doom_key_up(DOOM_KEY_UP_ARROW);
	if (jy < -STICK_THRESH && prev_y >= -STICK_THRESH) doom_key_down(DOOM_KEY_DOWN_ARROW);
	if (jy >= -STICK_THRESH && prev_y < -STICK_THRESH) doom_key_up(DOOM_KEY_DOWN_ARROW);
	if (jx < -STICK_THRESH && prev_x >= -STICK_THRESH) doom_key_down(DOOM_KEY_LEFT_ARROW);
	if (jx >= -STICK_THRESH && prev_x < -STICK_THRESH) doom_key_up(DOOM_KEY_LEFT_ARROW);
	if (jx >  STICK_THRESH && prev_x <=  STICK_THRESH) doom_key_down(DOOM_KEY_RIGHT_ARROW);
	if (jx <= STICK_THRESH && prev_x >   STICK_THRESH) doom_key_up(DOOM_KEY_RIGHT_ARROW);

	prev_btns = btns; prev_x = jx; prev_y = jy;
}
