/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * VecDrive - a pseudo-3D sunset racer for the PiTrex library.
 * Written for PiTrex on MiSTer (2026). An original game in the spirit of the
 * 80s arcade racers. v2: three themed sections per lap (SEA, DESERT, CITY)
 * with checkpoints, a 3-2-1 start, cars and trucks, palms and round trees,
 * and a soundtrack mode.
 *
 * Controls: joystick 1 steers (analog); button 1 = gas (and start);
 * button 2 = brake; button 3 = engine sound / music.
 *
 * Road maths after the classic "segment" racer: the track is a list of short
 * segments with a curve and a height; each frame the visible segments are
 * projected near-to-far, and a segment is only drawn if it rises above the
 * highest point drawn so far (so a crest hides what is behind it).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pitrex/pitrexio-gpio.h>
#include <vectrex/vectrexInterface.h>

/* ---- screen (library units) ---- */
#define SCR_L      (-17000)
#define SCR_R      ( 17000)
#define SCR_B      (-21000)
#define SCR_T      ( 21000)
#define HORIZON    (  2500)

/* ---- world ---- */
#define SEG_LEN    200.0f
#define ROAD_W     2000.0f        /* half width */
#define CAM_H      3000.0f
#define FOV        2100.0f
#define DRAW_DIST  60
#define NSEG_MAX   1300
#define FPS        50.0f
#define MAX_SPEED  (SEG_LEN * FPS)          /* one segment per frame */
#define ACCEL      (MAX_SPEED / 4.0f)
#define BRAKE      (-MAX_SPEED)
#define DECEL      (-MAX_SPEED / 5.0f)
#define OFF_DECEL  (-MAX_SPEED / 2.0f)
#define OFF_LIMIT  (MAX_SPEED / 4.0f)
#define CENTRIFUGAL 0.3f
#define N_CARS     8
#define PI         3.14159265f

enum { SEA, DESERT, CITY, N_THEMES };
static const char *theme_name[N_THEMES] = { "SEA", "DESERT", "CITY" };
enum { OBJ_NONE, OBJ_PALM, OBJ_ROUND, OBJ_CACTUS, OBJ_ROCK, OBJ_LAMP };

typedef struct { float curve, y; signed char side; unsigned char obj, theme; } seg_t;
typedef struct { float z, x, speed; int truck; } car_t;
typedef struct { float scale, x, sy; int vis; } proj_t;

static seg_t seg[NSEG_MAX];
static int nseg, section_start[N_THEMES];
static float track_len;
static car_t cars[N_CARS];
static proj_t proj[DRAW_DIST + 1];

static float pos_z, speed, player_x, sun_x, sky_x;
static float time_left;
static int lap, state, state_timer, cur_theme, banner_timer, music_mode, btn_prev;
static char banner[24];
enum { ATTRACT, COUNTDOWN, RACE, TIMEUP };

/* ---------------------------------------------------------------- track */
static float last_y(void) { return nseg ? seg[nseg - 1].y : 0.0f; }

static void add_seg(float curve, float y, int theme)
{
	if (nseg >= NSEG_MAX) return;
	seg[nseg].curve = curve; seg[nseg].y = y; seg[nseg].obj = OBJ_NONE;
	seg[nseg].side = 0; seg[nseg].theme = (unsigned char)theme;
	nseg++;
}

static float ease_in(float a, float b, float p) { return a + (b - a) * p * p; }
static float ease_out(float a, float b, float p) { return a + (b - a) * (1 - (1 - p) * (1 - p)); }
static float ease_io(float a, float b, float p) { return a + (b - a) * (0.5f - 0.5f * cosf(p * PI)); }

/* enter/hold/leave segment counts, curve strength, hill height in segments */
static void road(int theme, int enter, int hold, int leave, float curve, float hill)
{
	float y0 = last_y(), y1 = y0 + hill * SEG_LEN;
	int total = enter + hold + leave;
	for (int i = 0; i < enter; i++) add_seg(ease_in(0, curve, (float)i / enter), ease_io(y0, y1, (float)i / total), theme);
	for (int i = 0; i < hold; i++)  add_seg(curve, ease_io(y0, y1, (float)(enter + i) / total), theme);
	for (int i = 0; i < leave; i++) add_seg(ease_out(curve, 0, (float)i / leave), ease_io(y0, y1, (float)(enter + hold + i) / total), theme);
}

static void build_track(void)
{
	nseg = 0;
	section_start[SEA] = nseg;              /* rolling coast road */
	road(SEA, 10, 40, 10, 0, 0);
	road(SEA, 20, 40, 20, 2.0f, 0);
	road(SEA, 20, 30, 20, 0, 5);
	road(SEA, 20, 40, 20, -3.0f, -3);
	road(SEA, 10, 20, 10, 0, 2);
	road(SEA, 15, 30, 15, 2.5f, -4);
	section_start[DESERT] = nseg;           /* long straights over dunes */
	road(DESERT, 10, 60, 10, 0, 6);
	road(DESERT, 20, 40, 20, -1.5f, -6);
	road(DESERT, 10, 50, 10, 0, 5);
	road(DESERT, 10, 40, 10, 0, -5);
	road(DESERT, 20, 30, 20, 2.0f, 2);
	road(DESERT, 15, 20, 15, 0, -2);
	section_start[CITY] = nseg;             /* tight, flat S-bends */
	road(CITY, 10, 30, 10, 0, 0);
	road(CITY, 10, 20, 10, 4.0f, 0);
	road(CITY, 10, 20, 10, -4.0f, 0);
	road(CITY, 10, 30, 10, 0, 1);
	road(CITY, 10, 20, 10, -4.5f, 0);
	road(CITY, 10, 20, 10, 4.5f, -1);
	road(CITY, 15, 30, 15, 0, 0);

	/* roadside objects by theme, alternating sides */
	for (int i = 8; i < nseg - 4; i++) {
		seg_t *s = &seg[i];
		int k = i / 5;
		switch (s->theme) {
		case SEA:    if (i % 5 == 0) { s->obj = (k % 3 == 0) ? OBJ_ROUND : OBJ_PALM; s->side = (k & 1) ? 1 : -1; } break;
		case DESERT: if (i % 7 == 0) { s->obj = (k % 2) ? OBJ_CACTUS : OBJ_ROCK; s->side = ((i / 7) & 1) ? 1 : -1; } break;
		case CITY:   if (i % 4 == 0) { s->obj = OBJ_LAMP; s->side = ((i / 4) & 1) ? 1 : -1; } break;
		}
	}
	track_len = nseg * SEG_LEN;
}

/* ---------------------------------------------------------------- drawing */
static int outcode(float x, float y)
{
	int c = 0;
	if (x < SCR_L) c |= 1; else if (x > SCR_R) c |= 2;
	if (y < SCR_B) c |= 4; else if (y > SCR_T) c |= 8;
	return c;
}

/* clipped line (Cohen-Sutherland) so the beam never leaves the tube */
static void line(float x0, float y0, float x1, float y1, int bright)
{
	int c0 = outcode(x0, y0), c1 = outcode(x1, y1);
	for (int guard = 0; guard < 8; guard++) {
		if (!(c0 | c1)) {
			v_directDraw32((int)x0, (int)y0, (int)x1, (int)y1, bright);
			return;
		}
		if (c0 & c1) return;
		int c = c0 ? c0 : c1;
		float x, y;
		if (c & 8)      { x = x0 + (x1 - x0) * (SCR_T - y0) / (y1 - y0); y = SCR_T; }
		else if (c & 4) { x = x0 + (x1 - x0) * (SCR_B - y0) / (y1 - y0); y = SCR_B; }
		else if (c & 2) { y = y0 + (y1 - y0) * (SCR_R - x0) / (x1 - x0); x = SCR_R; }
		else            { y = y0 + (y1 - y0) * (SCR_L - x0) / (x1 - x0); x = SCR_L; }
		if (c == c0) { x0 = x; y0 = y; c0 = outcode(x0, y0); }
		else         { x1 = x; y1 = y; c1 = outcode(x1, y1); }
	}
}

/* arc across the top of a circle: n segments from angle a0 to a1 */
static void arc(float cx, float cy, float rx, float ry, float a0, float a1, int n, int bright)
{
	float px = cx + rx * cosf(a0), py = cy + ry * sinf(a0);
	for (int k = 1; k <= n; k++) {
		float a = a0 + (a1 - a0) * k / n;
		float x = cx + rx * cosf(a), y = cy + ry * sinf(a);
		line(px, py, x, y, bright);
		px = x; py = y;
	}
}

static void draw_sky(void)
{
	/* sun: half-disc on the horizon with bands, slides with the bends */
	float cx = sun_x, cy = HORIZON + 600, r = 3200;
	arc(cx, cy, r, r, PI, 0, 10, 70);
	for (int b = 1; b <= 3; b++) {
		float h = cy + b * 700, w = sqrtf(r * r - (h - cy) * (h - cy));
		line(cx - w, h, cx + w, h, 45);
	}
	float span = 40000.0f;
	float off = fmodf(sky_x, span); if (off < 0) off += span;

	if (cur_theme == SEA) {                      /* low islands and wave glints */
		static const short isl[] = { 0, 500, 300, 900, 0, 0, 400, 700, 200, 0, 0 };
		const int n = sizeof isl / sizeof isl[0];
		float step = span / (n - 1);
		for (int rep = -1; rep <= 1; rep++) {
			float x0 = -span / 2 - off + rep * span;
			for (int k = 0; k < n - 1; k++)
				if (isl[k] || isl[k + 1])
					line(x0 + k * step, HORIZON + isl[k], x0 + (k + 1) * step, HORIZON + isl[k + 1], 35);
		}
		for (int w = 0; w < 6; w++) {                /* glints just below the horizon */
			float wx = fmodf(w * 6100.0f - off * 1.5f, 34000.0f); if (wx < 0) wx += 34000.0f;
			wx -= 17000.0f;
			float wy = HORIZON - 300 - (w % 3) * 350;
			line(wx - 900, wy, wx + 900, wy, 30);
		}
	} else if (cur_theme == DESERT) {            /* flat-topped mesas */
		static const short mes[] = { 0, 0, 1600, 1600, 0, 0, 0, 900, 900, 2100, 2100, 0, 0 };
		static const short mxs[] = { 0, 3, 4, 9, 10, 15, 17, 18, 21, 22, 26, 27, 40 };
		const int n = sizeof mes / sizeof mes[0];
		float unit = span / 40.0f;
		for (int rep = -1; rep <= 1; rep++) {
			float x0 = -span / 2 - off + rep * span;
			for (int k = 0; k < n - 1; k++)
				if (mes[k] || mes[k + 1])
					line(x0 + mxs[k] * unit, HORIZON + mes[k], x0 + mxs[k + 1] * unit, HORIZON + mes[k + 1], 40);
		}
	} else {                                     /* city skyline */
		static const short bh[] = { 2200, 3400, 1800, 4200, 2600, 3000, 1500, 3800, 2400, 2000 };
		const int n = sizeof bh / sizeof bh[0];
		float bw = span / n;
		for (int rep = -1; rep <= 1; rep++) {
			float x0 = -span / 2 - off + rep * span;
			for (int k = 0; k < n; k++) {
				float l = x0 + k * bw + 300, rr = x0 + (k + 1) * bw - 300, t = HORIZON + bh[k];
				if (rr < SCR_L || l > SCR_R) continue;
				line(l, HORIZON, l, t, 40);
				line(l, t, rr, t, 40);
				line(rr, t, rr, HORIZON, 40);
			}
		}
	}
	line(SCR_L, HORIZON, SCR_R, HORIZON, 40);
}

static void draw_palm(float x, float y, float s)
{
	float h = 3200 * s, w = 900 * s;
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	line(x, y, x + w * 0.2f, y + h, 60);                      /* trunk */
	float tx = x + w * 0.2f, ty = y + h;
	line(tx, ty, tx - w * 1.4f, ty - h * 0.25f, 70);           /* fronds */
	line(tx, ty, tx + w * 1.4f, ty - h * 0.2f, 70);
	line(tx, ty, tx - w * 0.6f, ty + h * 0.15f, 70);
	line(tx, ty, tx + w * 0.7f, ty + h * 0.12f, 70);
}

/* round tree: trunk and stacked arcs ("rotated brackets") for a layered top */
static void draw_round_tree(float x, float y, float s)
{
	float h = 2600 * s, w = 1100 * s;
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	line(x, y, x, y + h * 0.45f, 60);
	int layers = h > 1500 ? 3 : 2, seg_n = h > 1500 ? 4 : 3;
	for (int k = 0; k < layers; k++) {
		float cy = y + h * (0.45f + 0.18f * k);
		float rx = w * (1.0f - 0.25f * k);
		arc(x, cy, rx, h * 0.22f, PI * 0.95f, PI * 0.05f, seg_n, 75);
	}
}

static void draw_cactus(float x, float y, float s)
{
	float h = 2400 * s, w = 500 * s;
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	line(x, y, x, y + h, 70);                                   /* stem */
	line(x, y + h * 0.45f, x - w, y + h * 0.45f, 70);           /* left arm */
	line(x - w, y + h * 0.45f, x - w, y + h * 0.75f, 70);
	line(x, y + h * 0.6f, x + w, y + h * 0.6f, 70);             /* right arm */
	line(x + w, y + h * 0.6f, x + w, y + h * 0.85f, 70);
}

static void draw_rock(float x, float y, float s)
{
	float h = 700 * s, w = 1100 * s;
	if (h < 80) return;
	line(x - w, y, x - w * 0.3f, y + h, 55);
	line(x - w * 0.3f, y + h, x + w * 0.6f, y + h * 0.7f, 55);
	line(x + w * 0.6f, y + h * 0.7f, x + w, y, 55);
}

static void draw_lamp(float x, float y, float s, int side)
{
	float h = 3600 * s, w = 700 * s * -side;          /* arm leans over the road */
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	line(x, y, x, y + h, 60);
	line(x, y + h, x + w, y + h * 0.95f, 60);
	line(x + w, y + h * 0.95f, x + w, y + h * 0.88f, 90);   /* lamp */
}

static void draw_vehicle(float x, float y, float s, int truck, int bright)
{
	if (!truck) {
		float w = 0.35f * ROAD_W * s, h = 0.32f * ROAD_W * s;
		line(x - w, y, x + w, y, bright);
		line(x + w, y, x + w * 0.8f, y + h, bright);
		line(x + w * 0.8f, y + h, x - w * 0.8f, y + h, bright);
		line(x - w * 0.8f, y + h, x - w, y, bright);
	} else {                                     /* box trailer seen from behind */
		float w = 0.45f * ROAD_W * s, h = 0.75f * ROAD_W * s;
		line(x - w, y, x + w, y, bright);
		line(x + w, y, x + w, y + h, bright);
		line(x + w, y + h, x - w, y + h, bright);
		line(x - w, y + h, x - w, y, bright);
		if (h > 400) line(x, y + h * 0.1f, x, y + h * 0.9f, bright - 25);   /* doors */
	}
}

static void draw_player(float steer)
{
	float y = -17500, lean = steer * 500;
	line(-4200, y, 4200, y, 110);                               /* body */
	line(4200, y, 4000, y + 1400, 110);
	line(-4200, y, -4000, y + 1400, 110);
	line(-4000, y + 1400, 4000, y + 1400, 110);
	line(-2600 + lean, y + 1400, -2000 + lean, y + 2600, 100);  /* cabin */
	line(-2000 + lean, y + 2600, 2000 + lean, y + 2600, 100);
	line(2000 + lean, y + 2600, 2600 + lean, y + 1400, 100);
	line(-3800, y - 600, -2400, y - 600, 90);                   /* tyres */
	line(2400, y - 600, 3800, y - 600, 90);
}

static void draw_object(const seg_t *sg, float x, float y, float s)
{
	switch (sg->obj) {
	case OBJ_PALM:   draw_palm(x, y, s); break;
	case OBJ_ROUND:  draw_round_tree(x, y, s); break;
	case OBJ_CACTUS: draw_cactus(x, y, s); break;
	case OBJ_ROCK:   draw_rock(x, y, s); break;
	case OBJ_LAMP:   draw_lamp(x, y, s, sg->side); break;
	default: break;
	}
}

static void draw_road(void)
{
	int base = (int)(pos_z / SEG_LEN) % nseg;
	float pct = fmodf(pos_z, SEG_LEN) / SEG_LEN;
	float py = seg[base].y + (seg[(base + 1) % nseg].y - seg[base].y) * pct;
	float x = 0, dx = -seg[base].curve * pct;
	float maxy = -1e9f;                          /* the nearest segment always counts */
	int have_prev = 0;
	float pl = 0, pr = 0, pc = 0, psy = 0;

	for (int n = 0; n < DRAW_DIST; n++) {
		const seg_t *sg = &seg[(base + n) % nseg];
		float z = (n + 1 - pct) * SEG_LEN;
		x += dx; dx += sg->curve;
		float s = FOV / z;
		float cx = (x - player_x * ROAD_W) * s;
		float sy = HORIZON + (sg->y - (py + CAM_H)) * s;
		float hw = ROAD_W * s;
		proj[n].scale = s; proj[n].x = cx; proj[n].sy = sy; proj[n].vis = 0;
		if (sy <= maxy) continue;                    /* hidden behind a crest */
		proj[n].vis = 1;
		int bright = n < 15 ? 110 : n < 35 ? 85 : 60;
		if (have_prev) {
			line(pl, psy, cx - hw, sy, bright);
			line(pr, psy, cx + hw, sy, bright);
			if (((base + n) & 3) == 0)               /* centre dashes */
				line(pc, psy, cx, sy, bright - 20);
		}
		if (sg->obj)
			draw_object(sg, cx + sg->side * hw * (sg->obj == OBJ_LAMP ? 1.25f : 1.6f), sy, s);
		pl = cx - hw; pr = cx + hw; pc = cx; psy = sy;
		have_prev = 1;
		maxy = sy;
	}

	for (int i = 0; i < N_CARS; i++) {              /* traffic */
		float rel = cars[i].z - pos_z;
		if (rel < 0) rel += track_len;
		int n = (int)(rel / SEG_LEN);
		if (n < 1 || n >= DRAW_DIST || !proj[n].vis) continue;
		draw_vehicle(proj[n].x + cars[i].x * ROAD_W * proj[n].scale, proj[n].sy, proj[n].scale,
		             cars[i].truck, 100);
	}
}

/* ---------------------------------------------------------------- sound */
/* Original synthwave loop: Am - F - C - G, arpeggios on A, bass on B.
   Tone periods = 93750 / Hz; one step = one sixteenth (7 frames). */
static const unsigned short arp[4][4] = {
	{ 213, 179, 142, 179 },      /* A4 C5 E5 C5 */
	{ 268, 213, 179, 213 },      /* F4 A4 C5 A4 */
	{ 179, 142, 120, 142 },      /* C5 E5 G5 E5 */
	{ 239, 190, 160, 190 },      /* G4 B4 D5 B4 */
};
static const unsigned short bass[4] = { 852, 1074, 717, 957 };   /* A2 F2 C3 G2 */
static int music_step, music_frame;

static void psg_tone(int ch, int period)
{
	v_writePSG(ch * 2, period & 0xFF);
	v_writePSG(ch * 2 + 1, (period >> 8) & 0x0F);
}

static int beep_frames;
static void beep(int period, int frames)
{
	psg_tone(2, period);                         /* channel C for effects */
	v_writePSG(10, 0x0D);
	beep_frames = frames;
}

static void sound_tick(int offroad)
{
	int noise = offroad && speed > 0 && state == RACE;
	int mixer = 0x3F & ~0x01;                    /* tone A on */
	if (music_mode) mixer &= ~0x02;              /* tone B (bass) */
	if (beep_frames > 0) mixer &= ~0x04;         /* tone C (beeps) */
	if (noise) mixer &= ~0x20;                   /* noise C (off-road rumble) */
	v_writePSG(7, mixer);                        /* bit 6 = 0: IOA stays an input */

	if (music_mode && state != ATTRACT) {
		if (music_frame-- <= 0) {
			music_frame = 6;
			int bar = (music_step / 8) & 3;
			psg_tone(0, arp[bar][music_step & 3]);
			if ((music_step & 3) == 0) psg_tone(1, bass[bar]);
			v_writePSG(8, 0x0B);
			v_writePSG(9, (music_step & 3) == 0 ? 0x0C : 0x09);
			music_step = (music_step + 1) & 31;
		} else if (music_frame == 3) {
			v_writePSG(8, 0x08);
		}
	} else if (!music_mode) {
		float hz = 45 + 230 * (speed / MAX_SPEED);
		psg_tone(0, (int)(93750 / hz));
		v_writePSG(8, state == RACE ? 0x0B : 0x00);
		v_writePSG(9, 0x00);
	} else {
		v_writePSG(8, 0x00);
		v_writePSG(9, 0x00);
	}

	v_writePSG(6, 0x0C);
	if (beep_frames > 0) {
		if (--beep_frames == 0) v_writePSG(10, noise ? 0x0A : 0x00);
	} else {
		v_writePSG(10, noise ? 0x0A : 0x00);
	}
}

/* ---------------------------------------------------------------- game */
static void show_banner(const char *text, int frames)
{
	snprintf(banner, sizeof banner, "%s", text);
	banner_timer = frames;
}

static void reset_race(void)
{
	pos_z = 0; speed = 0; player_x = 0; sun_x = 0; sky_x = 0;
	time_left = 55; lap = 1; cur_theme = SEA;
	for (int i = 0; i < N_CARS; i++) {
		cars[i].z = (i + 1) * track_len / (N_CARS + 1);
		cars[i].x = (i & 1) ? -0.45f : 0.45f;
		cars[i].truck = (i % 3 == 1);
		cars[i].speed = MAX_SPEED * (cars[i].truck ? 0.30f : 0.40f + 0.06f * (i % 4));
	}
}

static void update(void)
{
	float dt = 1.0f / FPS;
	int racing = state == RACE;
	int gas = racing && (currentButtonState & 0x01);
	int brake = racing && (currentButtonState & 0x02);
	float steer = currentJoy1X / 127.0f;
	float pct = speed / MAX_SPEED;
	int base = (int)(pos_z / SEG_LEN) % nseg;

	player_x += steer * dt * 2.2f * pct;
	player_x -= dt * 2 * pct * pct * seg[base].curve * CENTRIFUGAL;
	if (player_x < -2.5f) player_x = -2.5f;
	if (player_x > 2.5f) player_x = 2.5f;

	speed += (gas ? ACCEL : brake ? BRAKE : DECEL) * dt;
	int offroad = player_x < -1 || player_x > 1;
	if (offroad && speed > OFF_LIMIT) speed += OFF_DECEL * dt;
	if (speed < 0) speed = 0;
	if (speed > MAX_SPEED) speed = MAX_SPEED;

	sun_x -= seg[base].curve * pct * 120;            /* sky parallax follows the bends */
	sky_x += seg[base].curve * pct * 60;

	pos_z += speed * dt;
	if (pos_z >= track_len) {
		pos_z -= track_len;
		if (racing) { lap++; for (int i = 0; i < N_CARS; i++) cars[i].speed *= 1.08f; }
	}

	/* section checkpoints: banner + extra time */
	base = (int)(pos_z / SEG_LEN) % nseg;
	if (seg[base].theme != cur_theme) {
		cur_theme = seg[base].theme;
		if (racing) {
			char t[24];
			time_left += 25;
			snprintf(t, sizeof t, "%s  +25", theme_name[cur_theme]);
			show_banner(t, 120);
			beep(60, 12);
		}
	}

	for (int i = 0; i < N_CARS; i++) {
		cars[i].z += cars[i].speed * dt;
		if (cars[i].z >= track_len) cars[i].z -= track_len;
		float rel = cars[i].z - pos_z;
		if (rel < 0) rel += track_len;
		float halfw = cars[i].truck ? 0.65f : 0.55f;
		if (rel < SEG_LEN * 0.8f && fabsf(player_x - cars[i].x) < halfw && speed > cars[i].speed) {
			speed = cars[i].speed * 0.5f;          /* bump */
			beep(900, 6);
		}
	}

	if (racing) {
		time_left -= dt;
		if (time_left <= 0) { time_left = 0; state = TIMEUP; state_timer = 150; }
	}
	sound_tick(offroad);
}

static void hud(void)
{
	char t[32];
	snprintf(t, sizeof t, "TIME %02d", (int)(time_left + 0.99f));
	v_printString(-110, 122, t, 5, 80);
	snprintf(t, sizeof t, "%3d KM/H", (int)(speed / MAX_SPEED * 290));
	v_printString(25, 122, t, 5, 80);
	snprintf(t, sizeof t, "LAP %d  %s", lap, theme_name[cur_theme]);
	v_printString(-40, 108, t, 4, 60);
	if (state == ATTRACT) v_printString(-45, 60, "PRESS BUTTON 1", 6, 90);
	if (state == COUNTDOWN) {
		int n = state_timer / 50 + 1;
		snprintf(t, sizeof t, "%d", n);
		v_printString(-6, 60, t, 12, 110);
	}
	if (state == TIMEUP) v_printString(-30, 60, "TIME UP", 7, 100);
	if (banner_timer > 0) {
		banner_timer--;
		v_printString(-30, 75, banner, 7, 100);
	}
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;
	vectrexinit(1);
	v_setName("vecdrive");
	v_init();
	v_setRefresh(50);
	build_track();
	reset_race();
	state = ATTRACT;

	for (;;) {
		v_WaitRecal();
		v_readButtons();
		v_readJoystick1Analog();
		int pressed = currentButtonState & ~btn_prev;
		btn_prev = currentButtonState;

		if (pressed & 0x04) {                        /* button 3: engine / music */
			music_mode = !music_mode;
			show_banner(music_mode ? "MUSIC" : "ENGINE", 60);
		}
		if ((state == ATTRACT || (state == TIMEUP && state_timer <= 0)) && (pressed & 0x01)) {
			reset_race();
			state = COUNTDOWN;
			state_timer = 149;
			beep(150, 8);
		}
		if (state == COUNTDOWN) {
			if (state_timer % 50 == 0 && state_timer > 0) beep(150, 8);
			if (--state_timer < 0) { state = RACE; show_banner("GO!", 50); beep(75, 20); }
		}
		if (state == TIMEUP && state_timer > 0) state_timer--;
		update();

		draw_sky();
		draw_road();
		draw_player(currentJoy1X / 127.0f);
		hud();
	}
}
