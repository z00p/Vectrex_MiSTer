// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * uvmc2_menu.c - a game menu on the Vectrex for UVMC2 images and PiTrex programs.
 *
 * Started by picking the "UVMC2 Menu.mnu" file in the OSD or from an MGL; the core never starts
 * it by itself. Lists the .um2 files in games/Vectrex-PiTrex/UVMC2 and the .ptx files in
 * games/Vectrex-PiTrex/PiTrex (subfolders too). Stick: up and down move, left goes back, button:
 * choose. A choice is handed to pitrexd the way Main reports an OSD pick, with an absolute
 * FULLPATH, which is how pitrexd knows to come back here when the game ends.
 *
 * The UVMC2 list also offers REFRESH CARD (make_card.sh) and says when the card is out of date.
 *
 * PC:     PTX_STREAM=menu.ptxs PTX_MAX_FRAMES=60 PITREX_FAT=/some/tree ./uvmc2_menu
 * MiSTer: pitrexd runs it; PITREX_FAT is for tests only.
 */
#include "ptx.h"
#include "ptx_font.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { ORB = 0xD000, ORA = 0xD001, DDRB = 0xD002, DDRA = 0xD003,
       T1L = 0xD004, T1H = 0xD005, T2L = 0xD008, T2H = 0xD009,
       SR = 0xD00A, ACR = 0xD00B, PCR = 0xD00C };

enum { FRAME_CYCLES = 30000, MAX_ITEMS = 1024, ROWS = 7, COLS = 16,
       STEP = 16, STEP_N = 39, TAIL = 5,  /* one font grid step: 16 * 40 = 640 beam units, not too fast, see ramp() */
       ADV = 3, ROW_Y = 3300, TOP_Y = 12000, LEFT_X = -15000,
       Z_TEXT = 0x40, Z_HOT = 0x68, Z_TITLE = 0x58,
       STICK = 50, REPEAT_FIRST = 20, REPEAT_NEXT = 6 };

typedef struct { char title[COLS + 1]; char path[768]; } item_t;
typedef enum { HOME, UVMC2, PITREX, STARTING } screen_t;

static item_t games[MAX_ITEMS], progs[MAX_ITEMS];
static int ngames, nprogs;
static char fat[256] = "/media/fat";

/* card state, shared with the worker thread */
static pthread_mutex_t card_m = PTHREAD_MUTEX_INITIALIZER;
static int card_stale, card_busy;
static char card_msg[COLS + 8];

/* ---- beam ---------------------------------------------------------------------------------- */
static void mux(int sel, int v)
{
	ptx_write(ORA, (uint8_t)v);
	ptx_write(ORB, (uint8_t)(sel << 1));
	ptx_advance_cycles(2);
	ptx_write(ORB, 1);
}

static void beam_zero(void)
{
	ptx_advance_cycles(20);
	ptx_write(PCR, 0xCC);
	ptx_advance_cycles(40);
	ptx_write(PCR, 0xCE);
	mux(1, 0);
}

/* the beam moves (dx, dy) * (n + 1) units in n + 2 cycles, lit or not. A lit stroke stays lit for TAIL cycles more:
 * the ramp runs on a little after the write that should end it, and a fast stroke then stops well short of its corner */
static void ramp(int dx, int dy, int n, int lit)
{
	mux(0, dy);
	ptx_write(ORA, (uint8_t)dx);
	ptx_write(SR, lit ? 0xFF : 0);
	ptx_write(T1L, (uint8_t)n);
	ptx_write(T1H, 0);
	ptx_advance_cycles((uint32_t)n + 2 + (lit ? TAIL : 0));
	ptx_write(SR, 0);
}

/* a blank move of (dx, dy) grid steps, in pieces the DAC can hold */
static void move_grid(int dx, int dy)
{
	while (dx || dy) {
		int mx = dx > 6 ? 6 : dx < -6 ? -6 : dx, my = dy > 6 ? 6 : dy < -6 ? -6 : dy;
		ramp(STEP * mx, STEP * my, STEP_N, 0);
		dx -= mx; dy -= my;
	}
}

static const uint8_t *glyph(char c)
{
	for (unsigned k = 0; k < sizeof ptx_font / sizeof *ptx_font; k++)
		if (ptx_font[k].c == c) return ptx_font[k].s;
	return NULL;
}

/* one line of text, the beam back at the centre first so the rows do not drift apart */
static void text(int x, int y, const char *s, int z)
{
	mux(2, z);
	beam_zero();
	ramp(x / 192, y / 192, 191, 0);
	int px = 0, py = 0;
	for (int i = 0; s[i]; i++) {
		const uint8_t *g = glyph(s[i]);
		if (!g) continue;
		int mv = 1, ox = i * ADV;
		for (; *g != 0xFE; g++) {
			if (*g == 0xFF) { mv = 1; continue; }
			int gx = ox + (*g >> 4), gy = *g & 15;
			if (mv) move_grid(gx - px, gy - py);
			else ramp(STEP * (gx - px), STEP * (gy - py), STEP_N, 1);
			mv = 0; px = gx; py = gy;
		}
	}
}

static void row_text(int row, const char *s, int z)
{
	text(LEFT_X, TOP_Y - (row + 1) * ROW_Y, s, z);
}

/* ---- the lists ----------------------------------------------------------------------------- */
/* file name to something the font can draw: no extension, no "-uvmc2-SD-card" tail, capitals */
static void make_title(char *out, const char *name)
{
	char b[256];
	snprintf(b, sizeof b, "%s", name);
	char *dot = strrchr(b, '.');
	if (dot) *dot = 0;
	for (char *p = b + 1; *p; p++)
		if ((p[-1] == '-' || p[-1] == '_') && (!strncasecmp(p, "uvmc2", 5) || !strncasecmp(p, "umvc2", 5))) { p[-1] = 0; break; }
	int n = 0;
	for (char *p = b; *p && n < COLS; p++) {
		char c = (char)toupper((unsigned char)*p);
		if (!glyph(c)) c = ' ';
		if (c == '-') c = ' ';
		if (c == ' ' && (!n || out[n - 1] == ' ')) continue;
		out[n++] = c;
	}
	while (n && out[n - 1] == ' ') n--;
	out[n] = 0;
}

static void scan(const char *dir, const char *ext, item_t *list, int *n, int depth)
{
	DIR *d = opendir(dir);
	if (!d) return;
	struct dirent *e;
	size_t el = strlen(ext);
	while ((e = readdir(d)) && *n < MAX_ITEMS) {
		if (e->d_name[0] == '.') continue;
		char p[768];
		if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) continue;
		struct stat st;
		if (stat(p, &st)) continue;
		size_t l = strlen(e->d_name);
		if (S_ISDIR(st.st_mode)) { if (depth < 4) scan(p, ext, list, n, depth + 1); }
		else if (l > el && !strcasecmp(e->d_name + l - el, ext)) {
			make_title(list[*n].title, e->d_name);
			snprintf(list[*n].path, sizeof list[*n].path, "%s", p);
			if (list[*n].title[0]) ++*n;
		}
	}
	closedir(d);
}

static int by_title(const void *a, const void *b) { return strcmp(((const item_t *)a)->title, ((const item_t *)b)->title); }

/* ---- the card ------------------------------------------------------------------------------ */
static void card_say(const char *s)
{
	pthread_mutex_lock(&card_m);
	snprintf(card_msg, sizeof card_msg, "%s", s);
	pthread_mutex_unlock(&card_m);
}

static int card_check(void)    /* 1 = the folder has changed since the card was built */
{
	char c[400];
	snprintf(c, sizeof c, "chrt -o 0 bash %s/pitrex/make_card.sh --check > /dev/null 2>&1", fat);
	int r = system(c);
	return r != -1 && WIFEXITED(r) && WEXITSTATUS(r) == 1;
}

static void *check_thread(void *arg)
{
	(void)arg;
	int s = card_check();
	pthread_mutex_lock(&card_m);
	card_stale = s;
	pthread_mutex_unlock(&card_m);
	return NULL;
}

/* make_card's own progress lines are the progress message; normal priority, Main comes first */
static void *refresh_thread(void *arg)
{
	(void)arg;
	char c[400], l[256], t[COLS + 1];
	snprintf(c, sizeof c, "chrt -o 0 bash %s/pitrex/make_card.sh 2>&1", fat);
	FILE *f = popen(c, "r");
	int r = -1;
	if (f) {
		while (fgets(l, sizeof l, f)) {
			const char *p = strncmp(l, "make_card: ", 11) ? l : l + 11;
			char m[260]; snprintf(m, sizeof m, "%s.", p);   /* make_title drops what follows the last dot */
			make_title(t, m);
			card_say(t);
		}
		r = pclose(f);
	}
	int ok = r != -1 && WIFEXITED(r) && WEXITSTATUS(r) == 0, s = card_check();
	card_say(ok ? "CARD READY" : "CARD NOT BUILT");
	pthread_mutex_lock(&card_m);
	card_stale = s; card_busy = 0;
	pthread_mutex_unlock(&card_m);
	return NULL;
}

static void start_thread(void *(*fn)(void *))
{
	pthread_t th;
	if (!pthread_create(&th, NULL, fn, NULL)) pthread_detach(th);
}

/* ---- a choice ------------------------------------------------------------------------------ */
static void put(const char *name, const char *s)
{
	char p[300];
	snprintf(p, sizeof p, "/tmp/%s", name);
	FILE *f = fopen(p, "w");
	if (f) { fputs(s, f); fclose(f); }
}

/* as Main reports a pick; pitrexd stops this program and starts the choice */
static void pick(const item_t *it)
{
	const char *b = strrchr(it->path, '/');
	put("FULLPATH", it->path);
	put("CURRENTPATH", b ? b + 1 : it->path);
	usleep(300000);
	put("FILESELECT", "selected");
}

/* ---- screens ------------------------------------------------------------------------------- */
static void draw_list(const char *title, const item_t *list, int n, int first, int cur, int extra)
{
	/* extra: 1 = row 0 is REFRESH CARD */
	text(LEFT_X, TOP_Y, title, Z_TITLE);
	int total = n + extra;
	for (int r = 0; r < ROWS && first + r < total; r++) {
		int i = first + r;
		char b[COLS + 3];
		const char *s = extra && i == 0 ? "REFRESH CARD" : list[i - extra].title;
		snprintf(b, sizeof b, "%c %s", i == cur ? '>' : ' ', s);
		row_text(r, b, i == cur ? Z_HOT : Z_TEXT);
	}
	if (!total) row_text(0, "NOTHING FOUND", Z_TEXT);
	else if (total > ROWS) {
		char b[32];
		snprintf(b, sizeof b, "%d OF %d", cur + 1, total);
		row_text(ROWS, b, Z_TEXT);
	}
}

static void frame_end(uint64_t t0)
{
	uint64_t used = ptx_now_cycles() - t0;
	if (used + 2 < FRAME_CYCLES) ptx_advance_cycles((uint32_t)(FRAME_CYCLES - used - 2));
	ptx_write(T2L, 0x30);
	ptx_write(T2H, 0x75);
	ptx_frame();
}

int main(void)
{
	const char *e = getenv("PITREX_FAT");
	if (e && *e) snprintf(fat, sizeof fat, "%s", e);
	char d[400];
	snprintf(d, sizeof d, "%s/games/Vectrex-PiTrex/UVMC2", fat);
	scan(d, ".um2", games, &ngames, 0);
	snprintf(d, sizeof d, "%s/games/Vectrex-PiTrex/PiTrex", fat);
	scan(d, ".ptx", progs, &nprogs, 0);
	qsort(games, (size_t)ngames, sizeof *games, by_title);
	qsort(progs, (size_t)nprogs, sizeof *progs, by_title);

	if (ptx_init()) return 1;
	start_thread(check_thread);

	ptx_write(DDRA, 0xFF);
	ptx_write(DDRB, 0x9F);
	ptx_write(ORB, 0x01);
	ptx_write(ACR, 0x98);
	ptx_write(SR, 0);

	int both = ngames && nprogs;
	screen_t scr = both ? HOME : nprogs ? PITREX : UVMC2, back = scr;
	int cur = 0, first = 0, hold = 0, held_y = 0, armed = 0, left_armed = 0;
	unsigned msg_frames = 0, start_frames = 0;

	for (;;) {
		uint64_t t0 = ptx_now_cycles();
		ptx_input_t in;
		ptx_get_input(&in);
		int btn = (in.psg_ioa & 0x0F) != 0x0F;
		int y = in.pot[1] > STICK ? -1 : in.pot[1] < -STICK ? 1 : 0;      /* stick up: towards the top of the list */
		int left = in.pot[0] < -STICK;

		pthread_mutex_lock(&card_m);
		int stale = card_stale, busy = card_busy;
		char msg[sizeof card_msg];
		memcpy(msg, card_msg, sizeof msg);
		pthread_mutex_unlock(&card_m);

		int extra = scr == UVMC2;
		int n = scr == HOME ? 2 : scr == UVMC2 ? ngames + extra : scr == PITREX ? nprogs : 0;

		if (y && n && !busy) {
			if (y != held_y) hold = 0;
			if (hold == 0 || hold >= REPEAT_FIRST) {
				cur += y;
				if (cur < 0) cur = n - 1;
				if (cur >= n) cur = 0;
				if (hold >= REPEAT_FIRST) hold = REPEAT_FIRST - REPEAT_NEXT;
			}
			hold++;
		} else hold = 0;
		held_y = y;
		if (cur < first) first = cur;
		if (cur >= first + ROWS) first = cur - ROWS + 1;

		if (left && left_armed && both && scr != HOME && scr != STARTING && !busy) { scr = HOME; cur = first = 0; }
		left_armed = !left;

		/* a button only counts after it has been up once: the one that started the menu, or a game, must not choose */
		if (!btn) armed = 1;
		else if (armed && !busy && scr != STARTING) {
			armed = 0;
			if (scr == HOME) { scr = cur == 0 ? UVMC2 : PITREX; cur = first = 0; }
			else if (scr == UVMC2 && cur == 0) {
				pthread_mutex_lock(&card_m);
				card_busy = 1;
				pthread_mutex_unlock(&card_m);
				card_say("UPDATING CARD");
				msg_frames = 0;
				start_thread(refresh_thread);
			} else if (n) {
				const item_t *it = scr == UVMC2 ? &games[cur - 1] : &progs[cur];
				back = scr;
				scr = STARTING; start_frames = 0;
				pick(it);
				snprintf(msg, sizeof msg, "%s", it->title);
				card_say(it->title);
			}
		}

		if (busy) {
			text(LEFT_X, TOP_Y, "CARD", Z_TITLE);
			row_text(2, "UPDATING", Z_HOT);
			row_text(4, msg, Z_TEXT);
		} else if (scr == STARTING) {
			row_text(2, "STARTING", Z_HOT);
			row_text(4, msg, Z_TEXT);
			if (++start_frames > 750) { scr = back; card_say(""); }   /* 15 s and nothing took over: back to the list */
		} else if (scr == HOME) {
			text(LEFT_X, TOP_Y, "MENU", Z_TITLE);
			row_text(0, cur == 0 ? "> UVMC2 GAMES" : "  UVMC2 GAMES", cur == 0 ? Z_HOT : Z_TEXT);
			row_text(1, cur == 1 ? "> PITREX" : "  PITREX", cur == 1 ? Z_HOT : Z_TEXT);
		} else if (scr == UVMC2) {
			if (msg[0] && msg_frames++ < 150) draw_list(msg, games, ngames, first, cur, extra);
			else draw_list(stale ? "CARD OUT OF DATE" : "UVMC2 GAMES", games, ngames, first, cur, extra);
		} else draw_list("PITREX", progs, nprogs, first, cur, 0);

		frame_end(t0);
	}
}
