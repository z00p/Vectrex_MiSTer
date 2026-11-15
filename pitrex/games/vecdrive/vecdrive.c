/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * VecDrive - a pseudo-3D sunset racer for the PiTrex library.
 * Written for PiTrex on MiSTer (2026). An original game in the spirit of the
 * 80s arcade racers. v3: a four-phase race on a branching lattice of
 * nine themes (SEA, DESERT, FOREST, CANYON, CITY, ALPINE, LAKESIDE, VOLCANO,
 * NEON NIGHT). Every phase is time-bound (the next allowance is added on
 * arrival), ends in a fork where the lateral position picks the next theme,
 * and blends into it gradually. Big hills with proper crest occlusion, cars
 * and trucks, a 3-2-1 start and a soundtrack mode.
 *
 * Controls: joystick 1 steers (analog); button 1 = gas (and start);
 * button 2 = brake; button 3 = engine sound / music.
 *
 * Road maths after the classic "segment" racer: the track is a list of short
 * segments with a curve and a height; each frame the visible segments are
 * projected near-to-far, and a segment is only drawn if it rises above the
 * highest point drawn so far (so a crest hides what is behind it - this goes
 * for roadside objects and traffic too).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef VD_UVMC2
/* UVMC2 cartridge build (uvmc2/Makefile): no music folder and no debug output; the two PiTrex
   calls the UVMC2 SDK does not have are in uvmc2/vd_uvmc2.c */
#include <stdint.h>
#define fprintf(...) ((void)0)
void v_setName(char *name);
void v_printString(int8_t x, int8_t y, char *string, uint8_t textSize, uint8_t brightness);
#else
#include <strings.h>
#include <dirent.h>

#include <pitrex/pitrexio-gpio.h>
#ifdef VD_FPS
#include <sys/time.h>
#endif
#endif
#include <vectrex/vectrexInterface.h>

/* ---- screen (library units) ---- */
#define SCR_L      (-17000)
#define SCR_R      ( 17000)
#define SCR_B      (-21000)
#define SCR_T      ( 21000)
#define HORIZON    (  2500)       /* fixed: the printed sunset overlay depends on it */

/* ---- world ---- */
#define SEG_LEN    200.0f
#define ROAD_W     2000.0f        /* half width */
#define CAM_H      3000.0f
#define FOV        2100.0f
#define HILL_K     40.0f          /* vertical exaggeration of the hills (view only) */
#ifndef CLIMB_A                   /* test build "Road stays below the horizon": -DCLIMB_A=200.0f -DCLIMB_B=400.0f */
#define CLIMB_A    1200.0f        /* road above the horizon is squeezed beyond this ... */
#endif
#ifndef CLIMB_B
#define CLIMB_B    2600.0f        /* ... and can never rise more than A + B above it */
#endif
#ifndef VD_SKY_FIXED              /* the skyline lifts onto a crest that rises above it (John's pick, 9 Nov 2026) */
#define VD_SKY_FOLLOWS_CREST
#endif
#define DRAW_DIST  60
#ifndef OBJ_NEAR
#define OBJ_NEAR   4              /* roadside objects in the nearest 4 segments (800 units) are not drawn */
#endif
#define NSEG_MAX   2400
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

/* ---- race layout ---- */
#define PHASE_LEN   1850          /* segments in a phase: ~37.5 s at full speed */
#define APPROACH    110           /* flat straight before the fork */
#define FORK_LEN    48            /* segments over which the road splits */
#define FORK_DMAX   2.0f          /* branch centre offset at the end (road widths) */
#define FORK_WARN   250           /* segments ahead at which the banner appears */
#define FINISH_RUN  30
/* checkpoint race (the default since 10 Nov 2026; until then the -DVD_CHECKPOINT build): after the split the two
   roads bend apart over a quiet stretch with little scenery, the next phase starts and its allowance is added at a
   checkpoint gantry a little way in. -DVD_NOCHECKPOINT builds the earlier race: the allowance comes at the fork. */
#ifndef VD_NOCHECKPOINT
#define VD_CHECKPOINT
#endif
#define DIVERGE_LEN 120           /* segments of the quiet stretch after the split (80 bent too sharply) */
#define DIVERGE_MAX 12.0f         /* branch offset at its end: the other road is off screen */
#define CHECK_SEG   36            /* checkpoint gantry, segments into the new phase */

enum { SEA, DESERT, CITY, FOREST, CANYON, ALPINE, LAKESIDE, VOLCANO, NEON, N_THEMES };
static const char *theme_name[N_THEMES] = {
	"SEA", "DESERT", "CITY", "FOREST", "CANYON", "ALPINE", "LAKESIDE", "VOLCANO", "NEON NIGHT"
};
enum { OBJ_NONE, OBJ_PALM, OBJ_ROUND, OBJ_CACTUS, OBJ_ROCK, OBJ_LAMP, OBJ_PINE, OBJ_SNOWPINE,
       OBJ_PILLAR, OBJ_REED, OBJ_POST, OBJ_LAVA, OBJ_NEON, OBJ_POPLAR, OBJ_BIRCH, OBJ_PARASOL, OBJ_GRASS, OBJ_HUT,
       OBJ_TOWER, OBJ_FORKSIGN, OBJ_FINISH, OBJ_CHECK, OBJ_PORTAL };
#define OBJ_GANTRY(o) ((o) == OBJ_FORKSIGN || (o) == OBJ_FINISH || (o) == OBJ_CHECK || (o) == OBJ_PORTAL)

/* tunnels (10 Nov 2026): one per phase on the stages where the road can go through something: a bore through the
   rock (CANYON, ALPINE, VOLCANO) or an underpass under a road deck (CITY). The road runs level through it with one
   gentle bend; ribs every 4 segments, two wall lines and roof lights rush past inside, the sky shows only through
   the far mouth, and no roadside scenery stands near either mouth */
enum { TUN_NONE, TUN_ROCK, TUN_DECK };
static const unsigned char th_tun[N_THEMES] = { TUN_NONE, TUN_NONE, TUN_DECK, TUN_NONE, TUN_ROCK, TUN_ROCK, TUN_NONE, TUN_ROCK, TUN_NONE };
#define TUN_LEN_ROCK 105          /* segments, a multiple of 4 plus 1 so the far mouth is a rib: ~2 s at full speed */
#define TUN_LEN_DECK 41
#define TUN_W        1.6f         /* wall half width, road half widths */
#define TUN_CLEAR    6            /* segments after the far mouth kept clear of scenery (beyond, it shows through the mouth) */
static int tun_a, tun_b, tun_kind;   /* this phase's tunnel: segments [tun_a, tun_b), 0 0 = none */

/* phase lattice: phase 1 has one position (SEA), phase 2 two, phases 3 and 4
   three; where left / right leads is in next_pos() */
static const unsigned char lat_theme[4][3] = {
	{ SEA }, { DESERT, FOREST }, { CANYON, CITY, ALPINE }, { LAKESIDE, VOLCANO, NEON }
};
static const float ph_time[5] = { 0, 45, 40, 40, 40 };     /* allowance per phase, seconds */

typedef struct { float curve, y, fork; signed char side; unsigned char obj, theme, lat; } seg_t;   /* lat: 0 near the road, 1, 2 set back */
typedef struct { float z, x, speed; int truck; } car_t;
typedef struct { float scale, x, sy; int vis; } proj_t;

static seg_t seg[NSEG_MAX], endseg;
static int nseg, fork_start, zone_len;
static float track_len;
static car_t cars[N_CARS];
static proj_t proj[DRAW_DIST + 1];

static float pos_z, speed, player_x, sun_x, sky_x, hz_off, hz = HORIZON, sky_w = 1;
static float time_left;
#ifdef VD_CHECKPOINT
static int check_due;                                 /* the new phase's checkpoint is still ahead */
static float fork_prev;                               /* branch offset last frame, to ride the bend apart */
#endif
static int ph, cur_pos, ph_theme, prev_theme, race_frames;
static int state, state_timer, banner_timer, banner2_timer, music_mode, btn_prev;
static char banner[40], banner2[24];
enum { ATTRACT, COUNTDOWN, RACE, TIMEUP, GOAL };

#ifdef VD_STATS
static int nlines;
#endif

static unsigned hsh(unsigned k)
{
	k *= 2654435761u; k ^= k >> 15; k *= 2246822519u; k ^= k >> 13;
	return k;
}

/* ---------------------------------------------------------------- track */
static float last_y(void) { return nseg ? seg[nseg - 1].y : 0.0f; }

static void add_seg(float curve, float y, int theme)
{
	if (nseg >= NSEG_MAX) return;
	seg[nseg].curve = curve; seg[nseg].y = y; seg[nseg].obj = OBJ_NONE; seg[nseg].fork = 0;
	seg[nseg].side = 0; seg[nseg].theme = (unsigned char)theme; seg[nseg].lat = 0;
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

/* road feature lists per theme: enter, hold, leave, curve (x0.1), hill. Hills
   are drawn HILL_K times higher than their table value (the camera is 3000
   units up, so effective hills need 6000-30000 units to be felt): a climb
   hides the road behind its crest, and in a dip the road ahead rises past the
   horizon. Climbs and dips alternate so the track stays near its start height. */
typedef struct { unsigned char enter, hold, leave; signed char curve, hill; } spec_t;
static const spec_t sp_sea[] = {  /* gentle rolling coast */
	{20,50,20,0,3}, {20,50,20,20,-3}, {20,40,20,30,3}, {20,50,20,-30,-3}, {15,50,15,0,3},
	{20,40,20,25,-3}, {10,30,10,0,0}, {20,40,20,-25,3}, {20,50,20,0,-3} };
static const spec_t sp_desert[] = {  /* long straights over big dunes */
	{20,80,20,0,9}, {20,80,20,0,-9}, {20,60,20,-15,8}, {20,60,20,0,-8}, {20,80,20,0,10},
	{20,50,20,20,-10}, {10,40,10,0,0} };
static const spec_t sp_city[] = {  /* almost flat, tight S-bends */
	{10,30,10,0,0}, {10,20,10,40,1}, {10,20,10,-40,-1}, {10,30,10,0,0}, {10,20,10,-45,0},
	{10,20,10,45,1}, {15,30,15,0,-1} };
static const spec_t sp_forest[] = {  /* winding rolling forest road */
	{20,60,20,25,6}, {20,60,20,-25,-6}, {20,60,20,0,6}, {20,60,20,-35,-6}, {20,50,20,35,6},
	{20,50,20,0,-6}, {15,40,15,-30,5}, {15,40,15,30,-5} };
static const spec_t sp_canyon[] = {  /* steep crests */
	{20,70,20,0,14}, {20,70,20,25,-14}, {20,70,20,-30,12}, {20,70,20,0,-12}, {20,70,20,35,14},
	{20,70,20,-35,-14}, {20,50,20,0,10}, {20,50,20,30,-10} };
static const spec_t sp_alpine[] = {  /* steep passes and hairpins */
	{20,70,20,0,15}, {20,70,20,-40,-15}, {20,70,20,40,14}, {20,70,20,0,-14}, {20,70,20,45,15},
	{20,70,20,-45,-15}, {20,50,20,0,12}, {20,50,20,35,-12} };
static const spec_t sp_lake[] = {  /* sweeping bends, gentle swells */
	{20,60,20,20,3}, {20,60,20,-20,-3}, {20,60,20,0,3}, {20,60,20,-30,-3}, {20,60,20,30,3},
	{20,60,20,0,-3} };
static const spec_t sp_volcano[] = {  /* long climbs and descents */
	{50,200,50,15,21}, {20,60,20,-35,-8}, {20,60,20,35,9}, {50,180,50,0,-24}, {15,40,15,-40,4},
	{20,50,20,40,-4} };
static const spec_t sp_neon[] = {  /* flat city night, sharp S-bends */
	{10,30,10,0,0}, {10,20,10,35,1}, {10,20,10,-35,-1}, {10,30,10,0,0}, {10,20,10,-45,0},
	{10,20,10,45,1}, {10,25,10,0,-1} };
static const spec_t sp_calm[] = {  /* transition zones: level-ish ground */
	{15,40,15,0,1}, {15,40,15,15,-1}, {15,40,15,-15,1}, {15,40,15,0,-1} };
#define SPECS(a) a, (int)(sizeof a / sizeof a[0])
static const struct { const spec_t *s; int n; } th_spec[N_THEMES] = {
	{ SPECS(sp_sea) }, { SPECS(sp_desert) }, { SPECS(sp_city) }, { SPECS(sp_forest) },
	{ SPECS(sp_canyon) }, { SPECS(sp_alpine) }, { SPECS(sp_lake) }, { SPECS(sp_volcano) },
	{ SPECS(sp_neon) }
};

/* (a dash mask of 1 strobed at top speed, one segment a frame: 3 at least since 9 Nov 2026) */
/* roadside objects (spacing in segments, up to 3 kinds) and road markings:
   centre-dash mask and kerb-tick mask (-1 = none) */
/* depth 1: objects stand at three distances from the road (close, middle, set back), mixing tall, medium and
   low kinds (SEA first, John 8 Nov; every theme since 9 Nov). An odd count of kinds lets each one turn up on
   both sides. Some kinds keep their place: huts and mesas set back, lamps, posts and signs at the kerb. */
static const struct { unsigned char spacing, n, kind[5]; signed char dash, kerb; unsigned char depth; } th_style[N_THEMES] = {
	/* SEA      */ { 5, 5, { OBJ_PALM, OBJ_PARASOL, OBJ_GRASS, OBJ_PALM, OBJ_HUT }, 3, -1, 1 },
	/* DESERT   */ { 6, 5, { OBJ_CACTUS, OBJ_ROCK, OBJ_GRASS, OBJ_CACTUS, OBJ_ROCK }, 7, -1, 1 },   /* grass = dry scrub */
	/* CITY     */ { 4, 5, { OBJ_LAMP, OBJ_TOWER, OBJ_LAMP, OBJ_ROUND, OBJ_TOWER }, 3,  7, 1 },   /* lamps, office towers behind */
	/* FOREST   */ { 4, 5, { OBJ_PINE, OBJ_POPLAR, OBJ_GRASS, OBJ_ROUND, OBJ_PINE }, -1,  7, 1 },
	/* CANYON   */ { 6, 5, { OBJ_PILLAR, OBJ_ROCK, OBJ_CACTUS, OBJ_GRASS, OBJ_ROCK }, 7,  3, 1 },
	/* ALPINE   */ { 5, 5, { OBJ_SNOWPINE, OBJ_BIRCH, OBJ_ROCK, OBJ_SNOWPINE, OBJ_GRASS }, 3, -1, 1 },
	/* LAKESIDE */ { 5, 5, { OBJ_REED, OBJ_POST, OBJ_POPLAR, OBJ_REED, OBJ_HUT }, 7,  7, 1 },   /* hut = boathouse */
	/* VOLCANO  */ { 6, 5, { OBJ_LAVA, OBJ_ROCK, OBJ_ROCK, OBJ_LAVA, OBJ_ROCK }, 3,  3, 1 },
	/* NEON     */ { 4, 2, { OBJ_NEON, OBJ_LAMP },                3,  7, 0 },   /* signs and lamps line the kerb */
};

/* distance from the road for the k-th object of a phase */
static unsigned char obj_lat(int obj, int k)
{
	if (obj == OBJ_HUT || obj == OBJ_PILLAR || obj == OBJ_TOWER) return 2;
	return (unsigned char)((k * 2) % 3);
}

/* fill the track from a spec list until it reaches `limit` segments; every
   second pass mirrors the bends so a long phase does not repeat itself */
static void fill(int theme, const spec_t *sp, int n, int limit)
{
	for (int pass = 0; nseg < limit && nseg < NSEG_MAX; pass++)
		for (int k = 0; k < n && nseg < limit; k++)
			road(theme, sp[k].enter, sp[k].hold, sp[k].leave,
			     (pass & 1 ? -0.1f : 0.1f) * sp[k].curve, sp[k].hill);
}

/* build one phase: [transition zone][body][flat approach][fork or finish]; the
   zone blends from prev_t (or -1) into theme: objects switch over with rising
   probability, road markings switch at the middle */
static void build_phase(int theme, int prev_t, int phase)
{
	int total = phase == 1 ? PHASE_LEN - 60 : PHASE_LEN;   /* no transition zone in phase 1 */
	nseg = 0;
	zone_len = prev_t >= 0 ? PHASE_LEN / 6 : 0;
	road(theme, 5, 15, 5, 0, 0);                         /* flat start */
	fill(theme, sp_calm, 4, zone_len);
#ifdef VD_CHECKPOINT
	int quiet = phase < 4 ? DIVERGE_LEN : 0;
#else
	int quiet = 0;
#endif
	int tail = APPROACH + (phase < 4 ? FORK_LEN + quiet : FINISH_RUN);
	tun_a = tun_b = 0; tun_kind = th_tun[theme];
	if (tun_kind) {                                      /* the tunnel, about 40% into the body: level, one bend */
		int len = tun_kind == TUN_ROCK ? TUN_LEN_ROCK : TUN_LEN_DECK;
		float bend = tun_kind == TUN_ROCK ? ((phase & 1) ? 2.0f : -2.0f) : 0.0f;
		fill(theme, th_spec[theme].s, th_spec[theme].n, zone_len + (total - tail - zone_len) * 2 / 5);
		road(theme, 5, 10, 5, 0, 0);                     /* a level run-up to the mouth */
		tun_a = nseg;
		road(theme, 12, len - 24, 12, bend, 0);
		tun_b = nseg;
	}
	fill(theme, th_spec[theme].s, th_spec[theme].n, total - tail);
	road(theme, 10, APPROACH - 20, 10, 0, 0);            /* flat approach */
	fork_start = nseg;
	if (phase < 4) {
		for (int i = 0; i < FORK_LEN; i++) {
			add_seg(0, last_y(), theme);
			seg[nseg - 1].fork = FORK_DMAX * (float)(i + 1) / FORK_LEN;
		}
		for (int i = 0; i < quiet; i++) {                /* the roads bend apart, then run on */
			add_seg(0, last_y(), theme);
			seg[nseg - 1].fork = i < quiet * 3 / 4 ? ease_io(FORK_DMAX, DIVERGE_MAX, (float)(i + 1) / (quiet * 3 / 4)) : DIVERGE_MAX;
		}
	} else {
		for (int i = 0; i < FINISH_RUN; i++) add_seg(0, last_y(), theme);
	}

	for (int i = 8; i < nseg - 4; i++) {                 /* roadside objects */
		seg_t *s = &seg[i];
		int ot = theme;
		if (i < zone_len && (int)(hsh(i / 3) % 1000) >= 1000 * i / zone_len) ot = prev_t;
		int sp = th_style[ot].spacing;
		if (i >= fork_start + FORK_LEN) sp = 13;          /* the quiet stretch: a little low scenery */
		if (i % sp == 0) {
			int k = i / sp;
			s->obj = th_style[ot].kind[k % th_style[ot].n];
			s->side = (k & 1) ? 1 : -1;
			if (th_style[ot].depth) s->lat = obj_lat(s->obj, k);
			if (sp == 13) { s->obj = OBJ_GRASS; s->lat = 2; }
		}
		if (i < zone_len / 2) s->theme = (unsigned char)prev_t;   /* markings: old, then new */
	}
	if (tun_kind) {                                      /* nothing stands near the mouths or in the tunnel */
		for (int i = tun_a - 10; i < tun_b + TUN_CLEAR && i < nseg; i++) seg[i].obj = OBJ_NONE;
		seg[tun_a].obj = OBJ_PORTAL; seg[tun_a].side = 0;
	}
	if (phase < 4) seg[fork_start].obj = OBJ_FORKSIGN;
	else seg[nseg - 12].obj = OBJ_FINISH;
#ifdef VD_CHECKPOINT
	if (phase > 1) { seg[CHECK_SEG].obj = OBJ_CHECK; seg[CHECK_SEG].side = 0; }
#endif
	endseg = seg[nseg - 1]; endseg.obj = OBJ_NONE; endseg.curve = 0;
	track_len = nseg * SEG_LEN;
#ifdef VD_STATS
	fprintf(stderr, "BUILD phase %d theme %s nseg %d zone %d tunnel %d-%d fork %d\n", phase, theme_name[theme], nseg, zone_len, tun_a, tun_b, fork_start);
#endif
}

static const seg_t *get_seg(int i) { return i < nseg ? &seg[i] : &endseg; }

/* where left (0) / right (1) from phase p position pos leads */
static int next_pos(int p, int pos, int dir)
{
	if (p == 1) return dir;
	if (p == 2) return pos + dir;
	static const unsigned char lo[3] = { 0, 0, 1 }, hi[3] = { 1, 2, 2 };
	return dir ? hi[pos] : lo[pos];
}

/* ---------------------------------------------------------------- drawing */
/* the clip window: the screen, or for a moment a tunnel mouth (the sky through the far one, the inside through the near one) */
static float clip_l = SCR_L, clip_r = SCR_R, clip_b = SCR_B, clip_t = SCR_T;

static void clip_to(float l, float b, float r, float t)
{
	clip_l = l > SCR_L ? l : SCR_L; clip_r = r < SCR_R ? r : SCR_R;
	clip_b = b > SCR_B ? b : SCR_B; clip_t = t < SCR_T ? t : SCR_T;
}

static void clip_screen(void) { clip_l = SCR_L; clip_r = SCR_R; clip_b = SCR_B; clip_t = SCR_T; }

static int outcode(float x, float y)
{
	int c = 0;
	if (x < clip_l) c |= 1; else if (x > clip_r) c |= 2;
	if (y < clip_b) c |= 4; else if (y > clip_t) c |= 8;
	return c;
}

/* clipped line (Cohen-Sutherland) so the beam never leaves the tube; rev draws it
   from the far end, clipped exactly as the forward line, so chains can run both ways */
static int obj_pass;                         /* 1: objects register sky boxes only, 2: draw lines only */

static void line_dir(float x0, float y0, float x1, float y1, int bright, int rev)
{
	if (obj_pass == 1) return;
	int c0 = outcode(x0, y0), c1 = outcode(x1, y1);
	for (int guard = 0; guard < 8; guard++) {
		if (!(c0 | c1)) {
#ifdef VD_STATS
			nlines++;
#endif
			if (rev) v_directDraw32((int)x1, (int)y1, (int)x0, (int)y0, bright);
			else     v_directDraw32((int)x0, (int)y0, (int)x1, (int)y1, bright);
			return;
		}
		if (c0 & c1) return;
		int c = c0 ? c0 : c1;
		float x, y;
		if (c & 8)      { x = x0 + (x1 - x0) * (clip_t - y0) / (y1 - y0); y = clip_t; }
		else if (c & 4) { x = x0 + (x1 - x0) * (clip_b - y0) / (y1 - y0); y = clip_b; }
		else if (c & 2) { y = y0 + (y1 - y0) * (clip_r - x0) / (x1 - x0); x = clip_r; }
		else            { y = y0 + (y1 - y0) * (clip_l - x0) / (x1 - x0); x = clip_l; }
		if (c == c0) { x0 = x; y0 = y; c0 = outcode(x0, y0); }
		else         { x1 = x; y1 = y; c1 = outcode(x1, y1); }
	}
}

static void line(float x0, float y0, float x1, float y1, int bright)
{
	line_dir(x0, y0, x1, y1, bright, 0);
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

/* ---- the road hides the sky: vectors cannot occlude, so sky lines are cut
   against the road outline (left/right edge at each drawn edge point, filled
   in by draw_road, which therefore runs before draw_sky) ---- */
static float rb_y[DRAW_DIST + 1], rb_l[DRAW_DIST + 1], rb_r[DRAW_DIST + 1];
static int rb_n;
/* the rest of each edge point (medians, fork width, edge brightness) and, per segment,
   the centre dash and kerb ticks: draw_road collects them, then draws them as chains */
static float rb_il[DRAW_DIST + 1], rb_ir[DRAW_DIST + 1], rb_c[DRAW_DIST + 1], rb_d[DRAW_DIST + 1];
static unsigned char rb_seg[DRAW_DIST + 1];       /* which segment (n) each edge point is */
static unsigned char rb_bl[DRAW_DIST + 1], rb_br[DRAW_DIST + 1];
static float dash_x[DRAW_DIST], dash_y[DRAW_DIST];
static unsigned char dash_b[DRAW_DIST], kerb_b[DRAW_DIST], obj_on[DRAW_DIST];

/* keep the part of [lo,hi] (parameter t) where a + b*t >= 0 */
static void keep_ge0(float a, float b, float *lo, float *hi)
{
	if (b > -1e-6f && b < 1e-6f) { if (a < 0) *hi = *lo - 1; return; }
	float t = -a / b;
	if (b > 0) { if (t > *lo) *lo = t; } else { if (t < *hi) *hi = t; }
}

/* roadside objects and traffic hide the sky too: each registers one or two
   boxes (trunk, crown ...) while it is drawn; only the nearest few that
   reach the horizon band are kept */
#define MAX_BOX 24
static float bx[MAX_BOX][4];
static int bx_n;

static void box(float x0, float y0, float x1, float y1)
{
	if (obj_pass == 2 || bx_n >= MAX_BOX || y1 < hz - 1100 || y0 > hz + 9000 || x1 < SCR_L || x0 > SCR_R) return;
	if (x1 - x0 < 120) { float c = (x0 + x1) / 2; x0 = c - 60; x1 = c + 60; }   /* thin things: keep a hair width */
	bx[bx_n][0] = x0; bx[bx_n][1] = y0; bx[bx_n][2] = x1; bx[bx_n][3] = y1; bx_n++;
}

static void sline(float x0, float y0, float x1, float y1, int bright)
{
	float in_a[16 + MAX_BOX], in_b[16 + MAX_BOX];
	int ni = 0;
	float dx = x1 - x0, dy = y1 - y0;
	for (int k = 0; k + 1 < rb_n && ni < 16; k++) {
		float ya = rb_y[k], yb = rb_y[k + 1], lo = 0, hi = 1;
		if (dy > -1e-3f && dy < 1e-3f) {             /* horizontal: one band */
			if (y0 < ya || y0 > yb) continue;
		} else {
			float ta = (ya - y0) / dy, tb = (yb - y0) / dy;
			if (ta > tb) { float t = ta; ta = tb; tb = t; }
			if (ta > lo) lo = ta;
			if (tb < hi) hi = tb;
			if (lo >= hi) continue;
		}
		float f = (y0 - ya) / (yb - ya), fs = dy / (yb - ya);       /* band fraction at t = 0, per t */
		float l0 = rb_l[k] + (rb_l[k + 1] - rb_l[k]) * f, ls = (rb_l[k + 1] - rb_l[k]) * fs;
		float r0 = rb_r[k] + (rb_r[k + 1] - rb_r[k]) * f, rs = (rb_r[k + 1] - rb_r[k]) * fs;
		keep_ge0(x0 - l0, dx - ls, &lo, &hi);        /* right of the left edge */
		keep_ge0(r0 - x0, rs - dx, &lo, &hi);        /* left of the right edge */
		if (lo < hi) { in_a[ni] = lo; in_b[ni] = hi; ni++; }
	}
	for (int k = 0; k < bx_n; k++) {                 /* object boxes */
		float lo = 0, hi = 1;
		keep_ge0(x0 - bx[k][0], dx, &lo, &hi);
		keep_ge0(bx[k][2] - x0, -dx, &lo, &hi);
		keep_ge0(y0 - bx[k][1], dy, &lo, &hi);
		keep_ge0(bx[k][3] - y0, -dy, &lo, &hi);
		if (lo < hi) { in_a[ni] = lo; in_b[ni] = hi; ni++; }
	}
	float cur = 0;                                   /* draw what is left outside the road */
	for (int pass = 0; pass <= ni; pass++) {
		int best = -1;
		for (int i = 0; i < ni; i++)
			if (in_b[i] >= 0 && (best < 0 || in_a[i] < in_a[best])) best = i;
		float end = best < 0 ? 1.0f : in_a[best];
		if (end - cur > 0.004f) line(x0 + dx * cur, y0 + dy * cur, x0 + dx * end, y0 + dy * end, bright);
		if (best < 0) break;
		if (in_b[best] > cur) cur = in_b[best];
		in_b[best] = -1;
	}
}

/* ---- horizon silhouettes; each takes a fade factor so two can cross-fade ---- */
#define SKY_SPAN 40000.0f
static float sky_off;

/* polyline repeated across the sky; xs in 0..units, hs = height above horizon;
   a segment with both ends at zero height is skipped (gap between landmarks) */
static void ridge(const short *xs, const short *hs, int n, int units, int base, float f)
{
	int b = (int)(base * f);
	if (b < 8) return;
	float unit = SKY_SPAN / units;
	for (int rep = -1; rep <= 1; rep++) {
		float x0 = -SKY_SPAN / 2 - sky_off + rep * SKY_SPAN;
		for (int k = 0; k < n - 1; k++) {
			if (!hs[k] && !hs[k + 1]) continue;
			float xa = x0 + xs[k] * unit, xb = x0 + xs[k + 1] * unit;
			if (xb < SCR_L || xa > SCR_R) continue;
			sline(xa, hz + hs[k], xb, hz + hs[k + 1], b);
		}
	}
}

static void towers(const short *bh, int n, int gap, int spires, int base, float f)
{
	int b = (int)(base * f);
	if (b < 8) return;
	float bw = SKY_SPAN / n;
	for (int rep = -1; rep <= 1; rep++) {
		float x0 = -SKY_SPAN / 2 - sky_off + rep * SKY_SPAN;
		for (int k = 0; k < n; k++) {
			float l = x0 + k * bw + gap, rr = x0 + (k + 1) * bw - gap, t = hz + bh[k];
			if (rr < SCR_L || l > SCR_R) continue;
			sline(l, hz, l, t, b);
			sline(l, t, rr, t, b);
			sline(rr, t, rr, hz, b);
			if (spires && !(k & 1)) sline((l + rr) / 2, t, (l + rr) / 2, t + 1100, b);
		}
	}
}

/* shimmer dashes just below the horizon (sea, lake) */
static void glints(int count, float gapx, float len, float base, float f)
{
	int b = (int)(base * f);
	if (b < 8) return;
	float wrap = 34000.0f;
	for (int w = 0; w < count; w++) {
		float wx = fmodf(w * gapx - sky_off * 1.5f, wrap); if (wx < 0) wx += wrap;
		wx -= 17000.0f;
		float wy = hz - 300 - (w % 3) * 350;
		sline(wx - len, wy, wx + len, wy, b);
	}
}

static void sky_theme(int th, float f)
{
	static const short sea_x[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80 };
	static const short sea_h[] = { 0, 500, 300, 900, 0, 0, 400, 700, 200, 0, 0 };
	static const short mes_x[] = { 0, 6, 8, 18, 20, 30, 34, 36, 42, 44, 52, 54, 80 };
	static const short mes_h[] = { 0, 0, 1600, 1600, 0, 0, 0, 900, 900, 2100, 2100, 0, 0 };
	/* CITY (10 Nov 2026): fourteen taller blocks drawn as one stepped outline (each wall only as high as the step to its
	   neighbour: the separate boxes down to the horizon cost twice the beam travel), masts on the three tallest */
	static const short cty_x[] = { 0, 10, 10, 20, 20, 30, 30, 40, 40, 50, 50, 60, 60, 70, 70, 80, 80, 90, 90, 100, 100, 110, 110, 120, 120, 130, 130, 140, 140 };   /* units of 140 */
	static const short cty_h[] = { 2600, 2600, 4600, 4600, 3200, 3200, 6400, 6400, 2200, 2200, 5200, 5200, 3600, 3600, 7200, 7200, 2800, 2800, 4200, 4200, 3000, 3000, 5800, 5800, 2400, 2400, 3800, 3800, 2600 };
	static const short mst_x[3][2] = { { 35, 35 }, { 75, 75 }, { 115, 115 } }, mst_h[3][2] = { { 6400, 7500 }, { 7200, 8300 }, { 5800, 6900 } };
	static const short neon_h[] = { 3400, 5200, 2600, 6000, 3800, 4600, 2200 };
	static const short for_x[] = { 0, 4, 8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80 };
	static const short for_h[] = { 500, 1500, 600, 1800, 700, 1400, 600, 1900, 500, 1600, 700, 1300, 600, 1700, 500, 1500, 700, 1800, 600, 1400, 500 };
	static const short can_x[] = { 0, 4, 4, 9, 9, 12, 12, 18, 18, 26, 26, 30, 30, 35, 35, 46, 46, 50, 50, 56, 56, 60, 60, 70, 70, 76, 76, 80 };
	static const short can_h[] = { 0, 0, 3400, 3400, 2200, 2200, 4600, 4600, 0, 0, 2800, 2800, 3800, 3800, 0, 0, 2400, 2400, 4200, 4200, 3000, 3000, 0, 0, 3200, 3200, 0, 0 };
	static const short alp_x[] = { 0, 6, 10, 15, 20, 26, 31, 36, 42, 48, 53, 58, 64, 70, 75, 80 };
	static const short alp_h[] = { 300, 4600, 2300, 3900, 1200, 5200, 2600, 4000, 1000, 4800, 2400, 3600, 900, 4400, 2100, 300 };
	static const short lak_x[] = { 0, 9, 17, 27, 35, 45, 53, 63, 71, 80 };
	static const short lak_h[] = { 300, 700, 500, 1100, 600, 800, 400, 900, 500, 300 };
	static const short vol_x[] = { 0, 12, 24, 40, 66, 72, 88, 94, 120, 136, 148, 160 };   /* units of 160 */
	static const short vol_h[] = { 0, 900, 0, 0, 3600, 3250, 3250, 3600, 0, 0, 1100, 0 };
	static const short smk_x[] = { 80, 83, 79, 83, 80 };
	static const short smk_h[] = { 3250, 4300, 5300, 6400, 7500 };
	static const short lav_x[] = { 76, 74, 76 };
	static const short lav_h[] = { 3250, 2200, 1200 };

	switch (th) {
	case SEA:
		ridge(sea_x, sea_h, 11, 80, 35, f);
		glints(6, 6100.0f, 900, 30, f);
		break;
	case DESERT:
		ridge(mes_x, mes_h, 13, 80, 40, f);
		break;
	case CITY:
		ridge(cty_x, cty_h, 29, 140, 40, f);
		for (int m = 0; m < 3; m++) ridge(mst_x[m], mst_h[m], 2, 140, 40, f);
		break;
	case FOREST:
		ridge(for_x, for_h, 21, 80, 40, f);
		break;
	case CANYON:
		ridge(can_x, can_h, 28, 80, 45, f);
		break;
	case ALPINE: {
		ridge(alp_x, alp_h, 16, 80, 45, f);
		float u = SKY_SPAN / 80;                         /* snow caps on the big peaks */
		static const short cap[3] = { 1, 5, 9 };
		for (int rep = -1; rep <= 1; rep++) {
			float x0 = -SKY_SPAN / 2 - sky_off + rep * SKY_SPAN;
			for (int c = 0; c < 3; c++) {
				float px = x0 + alp_x[cap[c]] * u, py = hz + alp_h[cap[c]];
				if (px < SCR_L - 2000 || px > SCR_R + 2000) continue;
				int b = (int)(70 * f);
				if (b < 8) continue;
				sline(px - 1.2f * u, py - 860, px - 0.5f * u, py - 1150, b);
				sline(px - 0.5f * u, py - 1150, px + 0.5f * u, py - 700, b);
				sline(px + 0.5f * u, py - 700, px + 1.5f * u, py - 860, b);
			}
		}
		break;
	}
	case LAKESIDE:
		ridge(lak_x, lak_h, 10, 80, 38, f);
		glints(5, 7300.0f, 1500, 45, f);
		break;
	case VOLCANO:
		ridge(vol_x, vol_h, 12, 160, 42, f);
		ridge(smk_x, smk_h, 5, 160, 30, f);              /* smoke plume */
		ridge(lav_x, lav_h, 3, 160, 80, f);              /* lava rivulet */
		break;
	case NEON:
		towers(neon_h, 7, 600, 1, 75, f);
		break;
	}
}

#ifdef VD_SKY_FOLLOWS_CREST
/* test build "Skyline follows the crest": when the road ahead rises above the horizon (out of a dip), the
   drawn skyline (sun, silhouettes, horizon line) is lifted to sit on the crest, eased so it doesn't jump.
   The printed overlay bands stay where they are. */
static float road_top = -1e9f, sky_lift;
#endif

/* in a tunnel the sky shows only through the far mouth (sky_win 1, window sw_*), or not at all (2); set by draw_road */
static int sky_win;
static float sw_l, sw_b, sw_r, sw_t;

static void draw_sky(void)
{
#ifdef VD_SKY_FOLLOWS_CREST
	float lift_to = road_top - hz;
	if (lift_to < 0) lift_to = 0;
	sky_lift += (lift_to - sky_lift) * 0.2f;
#endif
	if (sky_win == 2) return;
	if (sky_win == 1) clip_to(sw_l, sw_b, sw_r, sw_t);
#ifdef VD_SKY_FOLLOWS_CREST
	float hz_saved = hz;
	hz += sky_lift;
#endif
	/* sun: half-disc on the horizon with bands, slides with the bends */
	float cx = sun_x, cy = hz + 600, r = 3200;
	for (int k = 0; k < 10; k++) {
		float a0 = PI - PI * k / 10, a1 = PI - PI * (k + 1) / 10;
		sline(cx + r * cosf(a0), cy + r * sinf(a0), cx + r * cosf(a1), cy + r * sinf(a1), 70);
	}
	for (int b = 1; b <= 3; b++) {
		float h = cy + b * 700, w = sqrtf(r * r - (h - cy) * (h - cy));
		sline(cx - w, h, cx + w, h, 45);
	}
	sky_off = fmodf(sky_x, SKY_SPAN); if (sky_off < 0) sky_off += SKY_SPAN;
	if (sky_w < 1.0f) sky_theme(prev_theme, 1.0f - sky_w);   /* old dims, new brightens */
	sky_theme(ph_theme, sky_w);
	sline(SCR_L, hz, SCR_R, hz, 40);
#ifdef VD_SKY_FOLLOWS_CREST
	hz = hz_saved;
#endif
	clip_screen();
}

/* ---- roadside objects ---- */
/* palm (design P2, John's pick 8 Nov): near, an outlined ringed trunk that leans and tapers, five broad
   leaf loops in ONE path (out along each leaf's top edge, back along its bottom) and a coconut cluster;
   mid-distance, a curved trunk line and five single fronds; far, a trunk and four straight fronds */
static const float palm_leaf[5][3] = {                      /* dx (x crown width), rise, droop (x height) */
	{ -2.6f, 0.10f, 0.36f }, { -1.6f, 0.24f, 0.08f }, { 0.2f, 0.32f, -0.12f }, { 1.6f, 0.24f, 0.06f }, { 2.6f, 0.10f, 0.34f } };

static void palm_c(float x, float y, float h, float w, float t, float *cx, float *cy)
{
	*cx = x + w * 0.4f * sinf(PI * t * 0.85f);
	*cy = y + h * t;
}

static void draw_palm(float x, float y, float s)
{
	float h = 3000 * s, w = 900 * s, wc = 720 * s, tx, ty, px, py, qx, qy;
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	palm_c(x, y, h, w, 1.0f, &tx, &ty);
	box(x - w * 0.35f, y, x + w * 0.6f, ty);                                    /* trunk */
	box(tx - wc * 2.6f, ty - h * 0.36f, tx + wc * 2.6f, ty + h * 0.37f);        /* crown */
	if (h < 600) {                                              /* far: trunk and four fronds */
		line(x, y, tx, ty, 60);
		line(tx - wc * 2.2f, ty - h * 0.25f, tx, ty, 75);
		line(tx, ty, tx + wc * 2.2f, ty - h * 0.22f, 75);
		line(tx - wc * 1.2f, ty + h * 0.15f, tx, ty, 75);
		line(tx, ty, tx + wc * 1.2f, ty + h * 0.13f, 75);
		return;
	}
	if (h < 1800) {                                             /* mid: curved trunk, five fronds */
		px = x; py = y;
		for (int i = 1; i <= 3; i++) { palm_c(x, y, h, w, i / 3.0f, &qx, &qy); line(px, py, qx, qy, 65); px = qx; py = qy; }
		for (int k = 0; k < 5; k++) {                           /* crown out to the tip; next frond starts back at the crown */
			const float *l = palm_leaf[k];
			float mx = tx + wc * l[0] * 0.5f, my = ty + h * l[1];
			line(tx, ty, mx, my, 80);
			line(mx, my, tx + wc * l[0], ty - h * l[2], 80);
		}
		return;
	}
	/* near: the trunk outline, up the left edge and down the right */
	px = x - w * 0.32f; py = y;
	for (int side = -1; side <= 1; side += 2) {
		for (int i = 0; i <= 4; i++) {
			float t = side < 0 ? i / 4.0f : 1.0f - i / 4.0f, cx, cy, half = w * (0.32f - 0.14f * t);
			palm_c(x, y, h, w, t, &cx, &cy);
			qx = cx + side * half; qy = cy;
			if (side < 0 && i == 0) continue;
			line(px, py, qx, qy, 70);
			px = qx; py = qy;
		}
	}
	for (int k = 1; k <= 3; k++) {                              /* rings */
		float t = k / 4.0f, cx, cy, half = w * (0.32f - 0.14f * t);
		palm_c(x, y, h, w, t, &cx, &cy);
		line(cx - half, cy - h * 0.015f, cx + half, cy + h * 0.015f, 45);
	}
	px = tx; py = ty;                                           /* the leaves, one path from the crown */
	for (int k = 0; k < 5; k++) {
		const float *l = palm_leaf[k];
		float pt[4][2] = {
			{ tx + wc * l[0] * 0.45f, ty + h * (l[1] + 0.05f) }, { tx + wc * l[0] * 0.85f, ty + h * l[1] * 0.5f },
			{ tx + wc * l[0], ty - h * l[2] }, { tx + wc * l[0] * 0.55f, ty + h * (l[1] - 0.07f) } };
		for (int j = 0; j < 4; j++) { line(px, py, pt[j][0], pt[j][1], 85); px = pt[j][0]; py = pt[j][1]; }
		line(px, py, tx, ty, 85); px = tx; py = ty;
	}
	line(tx - w * 0.3f, ty - h * 0.02f, tx, ty - h * 0.09f, 70);     /* coconuts */
	line(tx, ty - h * 0.09f, tx + w * 0.3f, ty - h * 0.02f, 70);
	line(tx + w * 0.3f, ty - h * 0.02f, tx - w * 0.3f, ty - h * 0.02f, 70);
}

/* broadleaf (design B, 8 Nov): a trunk and a bumpy canopy drawn as ONE closed path, lobes
   alternating between two radii; fewer lobes further away */
static void draw_round_tree(float x, float y, float s)
{
	float h = 2600 * s, w = 1150 * s;
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	box(x - w * 0.08f, y, x + w * 0.08f, y + h * 0.4f);
	box(x - w, y + h * 0.38f, x + w, y + h * 1.02f);
	line(x, y, x, y + h * 0.38f, 60);
	int n = h > 1500 ? 12 : h > 600 ? 8 : 6;
	float cy = y + h * 0.7f, ry = h * 0.32f, px = x, py = cy - ry;
	for (int k = 1; k <= n; k++) {
		float a = 2 * PI * k / n - PI / 2, r = (k & 1) ? 0.86f : 1.0f;
		float qx = x + w * r * cosf(a), qy = cy + ry * r * sinf(a);
		line(px, py, qx, qy, 80);
		px = qx; py = qy;
	}
}

/* poplar (design D): tall narrow leaf shape in one closed path, with a vein that is the trunk carried on */
static void draw_poplar(float x, float y, float s)
{
	float h = 3800 * s, w = 550 * s;
	if (h < 150) { line(x, y, x, y + h, 55); return; }
	box(x - w * 1.1f, y, x + w * 1.1f, y + h);
	line(x, y, x, y + h * 0.15f, 60);
	if (h > 900) line(x, y + h * 0.15f, x, y + h * 0.75f, 50);       /* vein */
	int n = h > 900 ? 6 : 3;
	float px = x, py = y + h * 0.15f;
	for (int side = 1; side >= -1; side -= 2) {                  /* up the right, down the left */
		for (int k = 1; k <= n; k++) {
			float t = side > 0 ? (float)k / n : 1.0f - (float)k / n;
			float qx = x + side * w * sinf(PI * t) * (1.1f - 0.4f * t), qy = y + h * (0.15f + 0.85f * t);
			line(px, py, qx, qy, 78);
			px = qx; py = qy;
		}
	}
}

/* bare birch (design E, snow): trunk with a twig at the top, branches off it, a bark mark */
static void draw_birch(float x, float y, float s)
{
	float h = 3200 * s, w = 900 * s;
	if (h < 150) { line(x, y, x, y + h, 60); return; }
	box(x - w * 0.9f, y, x + w * 0.9f, y + h * 1.05f);
	line(x, y, x, y + h, 80);
	if (h < 700) return;
	line(x, y + h, x + w * 0.5f, y + h * 1.05f, 70);
	line(x, y + h * 0.72f, x - w * 0.6f, y + h * 0.98f, 70);
	line(x, y + h * 0.55f, x + w * 0.9f, y + h * 0.85f, 70);
	line(x, y + h * 0.4f, x - w * 0.8f, y + h * 0.7f, 70);
	line(x - w * 0.15f, y + h * 0.25f, x + w * 0.15f, y + h * 0.25f, 40);   /* bark mark */
}

static void draw_cactus(float x, float y, float s)
{
	float h = 2400 * s, w = 500 * s;
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	box(x - w * 0.1f, y, x + w * 0.1f, y + h);
	box(x - w, y + h * 0.45f, x + w, y + h * 0.85f);
	line(x, y, x, y + h, 70);                                   /* stem */
	line(x, y + h * 0.45f, x - w, y + h * 0.45f, 70);           /* left arm */
	line(x - w, y + h * 0.45f, x - w, y + h * 0.75f, 70);
	line(x, y + h * 0.6f, x + w, y + h * 0.6f, 70);             /* right arm */
	line(x + w, y + h * 0.6f, x + w, y + h * 0.85f, 70);
}

/* beach parasol (sea, medium height): a pole and a scalloped canopy as one path */
static void draw_parasol(float x, float y, float s)
{
	float h = 1500 * s, w = 950 * s, e = h * 0.72f;            /* e: canopy edge height */
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	box(x - 60, y, x + 60, y + e);
	box(x - w, y + e, x + w, y + h);
	line(x, y, x, y + h, 60);                                   /* pole, up to the top */
	line(x, y + h, x - w, y + e, 80);                           /* canopy: down the left */
	line(x - w, y + e, x - w * 0.5f, y + e + h * 0.07f, 80);   /* scallops back across */
	line(x - w * 0.5f, y + e + h * 0.07f, x, y + e, 80);
	line(x, y + e, x + w * 0.5f, y + e + h * 0.07f, 80);
	line(x + w * 0.5f, y + e + h * 0.07f, x + w, y + e, 80);
	line(x + w, y + e, x, y + h, 80);                           /* and up the right */
}

/* beach hut (sea, set back from the road): walls and an overhanging pitched roof as one path, then a door */
static void draw_hut(float x, float y, float s)
{
	float w = 1300 * s, wh = 1100 * s, h = 1900 * s;            /* half width, wall height, ridge height */
	if (h < 150) { line(x - w, y, x + w, y, 50); return; }
	box(x - w * 1.2f, y, x + w * 1.2f, y + h);
	line(x - w, y, x - w, y + wh, 70);                          /* left wall */
	line(x - w, y + wh, x - w * 1.25f, y + wh * 0.92f, 75);     /* eave */
	line(x - w * 1.25f, y + wh * 0.92f, x, y + h, 75);          /* roof, up to the ridge */
	line(x, y + h, x + w * 1.25f, y + wh * 0.92f, 75);          /* and down */
	line(x + w * 1.25f, y + wh * 0.92f, x + w, y + wh, 75);
	line(x + w, y + wh, x + w, y, 70);                          /* right wall */
	if (h < 500) return;                                        /* far: the outline is enough */
	line(x - w * 0.25f, y, x - w * 0.25f, y + wh * 0.6f, 60);   /* door */
	line(x - w * 0.25f, y + wh * 0.6f, x + w * 0.25f, y + wh * 0.6f, 60);
	line(x + w * 0.25f, y + wh * 0.6f, x + w * 0.25f, y, 60);
}

/* dune grass (sea, low): four blades fanning out of one root, two chains */
static void draw_grass(float x, float y, float s)
{
	float h = 650 * s, w = 420 * s;
	if (h < 80) return;
	box(x - w, y, x + w, y + h);
	line(x - w, y + h * 0.75f, x - w * 0.1f, y, 60);
	line(x - w * 0.1f, y, x - w * 0.35f, y + h, 60);
	line(x + w * 0.3f, y + h * 0.95f, x + w * 0.1f, y, 60);
	line(x + w * 0.1f, y, x + w, y + h * 0.65f, 60);
}

static void draw_rock(float x, float y, float s)
{
	float h = 700 * s, w = 1100 * s;
	if (h < 80) return;
	box(x - w, y, x + w, y + h);
	line(x - w, y, x - w * 0.3f, y + h, 55);
	line(x - w * 0.3f, y + h, x + w * 0.6f, y + h * 0.7f, 55);
	line(x + w * 0.6f, y + h * 0.7f, x + w, y, 55);
}

static void draw_lamp(float x, float y, float s, int side)
{
	float h = 3600 * s, w = 700 * s * -side;          /* arm leans over the road */
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	box(x - 60, y, x + 60, y + h);
	box(x < x + w ? x : x + w, y + h * 0.88f, x < x + w ? x + w : x, y + h);
	line(x, y, x, y + h, 60);
	line(x, y + h, x + w, y + h * 0.95f, 60);
	line(x + w, y + h * 0.95f, x + w, y + h * 0.88f, 90);   /* lamp */
}

/* fir (design C, 8 Nov): trunk, a base line and the outline in ONE path, up the left side in
   steps to the tip and down the right; snow fir: two steps and a bright cap */
static void draw_pine(float x, float y, float s, int snow)
{
	static const float tier[3][3] = { { 0.2f, 1.0f, 0.55f }, { 0.42f, 0.75f, 0.38f }, { 0.62f, 0.5f, 0.22f } };
	float h = 3400 * s, w = 1000 * s, px, py;
	if (h < 150) { line(x, y, x, y + h, 55); return; }
	box(x - w * 0.1f, y, x + w * 0.1f, y + h * 0.22f);
	box(x - w, y + h * 0.2f, x + w, y + h);
	line(x, y, x, y + h * 0.2f, 55);
	int b = snow ? 85 : 75, nt = h > 700 ? (snow ? 2 : 3) : 1;
	line(x + w, y + h * 0.2f, x - w, y + h * 0.2f, b);          /* base, right to left */
	px = x - w; py = y + h * 0.2f;
	for (int side = -1; side <= 1; side += 2) {
		for (int j = 0; j < nt; j++) {                        /* left: up the steps; right: down */
			int k = side < 0 ? j : nt - 1 - j;
			float by = y + h * tier[k][0], bw = w * tier[k][1], in = w * tier[k][2], iy = by + h * 0.08f;
			if (side < 0) {
				if (k > 0) { line(px, py, x - bw, by, b); px = x - bw; py = by; }
				line(px, py, x - in, iy, b); px = x - in; py = iy;
			} else {
				line(px, py, x + in, iy, b); px = x + in; py = iy;
				line(px, py, x + bw, by, b); px = x + bw; py = by;
			}
			if (side < 0 && j == nt - 1) { line(px, py, x, y + h, b); px = x; py = y + h; }
		}
	}
	if (snow && nt > 1) line(x - w * 0.3f, y + h * 0.7f, x + w * 0.3f, y + h * 0.7f, 110);   /* snow cap */
}

/* flared rock pillar (mesa column) */
static void draw_pillar(float x, float y, float s)
{
	float h = 3200 * s, w = 1000 * s;
	if (h < 150) { line(x, y, x, y + h, 55); return; }
	box(x - w, y, x + w, y + h);
	line(x - w * 0.7f, y, x - w, y + h * 0.6f, 65);
	line(x - w, y + h * 0.6f, x - w, y + h, 65);
	line(x - w, y + h, x + w, y + h, 65);
	line(x + w, y + h, x + w, y + h * 0.6f, 65);
	line(x + w, y + h * 0.6f, x + w * 0.7f, y, 65);
}

static void draw_reed(float x, float y, float s)
{
	float h = 1800 * s, w = 260 * s;
	if (h < 120) { line(x, y, x, y + h, 50); return; }
	box(x - w * 1.7f, y, x + w * 1.7f, y + h * 1.35f);
	line(x - w, y, x - w * 1.6f, y + h, 65);
	line(x, y, x + w * 0.3f, y + h * 1.2f, 65);
	line(x + w, y, x + w * 1.7f, y + h * 0.9f, 65);
	line(x + w * 0.3f, y + h * 1.2f, x + w * 0.3f, y + h * 1.35f, 85);   /* seed head */
}

static void draw_post(float x, float y, float s)           /* jetty post */
{
	float h = 1600 * s, w = 500 * s;
	if (h < 120) { line(x, y, x, y + h, 50); return; }
	box(x - w, y, x + w, y + h);
	line(x, y, x, y + h, 65);
	line(x - w, y + h, x + w, y + h, 65);
	line(x - w * 0.6f, y + h * 0.7f, x + w * 0.6f, y + h * 0.7f, 50);
}

static void draw_lava(float x, float y, float s)
{
	float h = 1400 * s, w = 1000 * s;
	if (h < 90) return;
	box(x - w, y, x + w, y + h);
	line(x - w, y, x - w * 0.4f, y + h * 0.8f, 60);
	line(x - w * 0.4f, y + h * 0.8f, x + w * 0.1f, y + h * 0.4f, 60);
	line(x + w * 0.1f, y + h * 0.4f, x + w * 0.5f, y + h, 60);
	line(x + w * 0.5f, y + h, x + w, y, 60);
	line(x - w * 0.1f, y + h * 0.1f, x + w * 0.05f, y + h * 0.5f, 100);   /* glowing crack */
}

static void draw_neon(float x, float y, float s)
{
	float h = 3000 * s, w = 700 * s;
	if (h < 150) { line(x, y, x, y + h, 50); return; }
	box(x - 60, y, x + 60, y + h * 0.55f);
	box(x - w, y + h * 0.55f, x + w, y + h);
	float by = y + h * 0.55f;
	line(x, y, x, by, 55);
	line(x - w, by, x + w, by, 100);                          /* sign board */
	line(x + w, by, x + w, y + h, 100);
	line(x + w, y + h, x - w, y + h, 100);
	line(x - w, y + h, x - w, by, 100);
	line(x - w * 0.5f, by + h * 0.12f, x + w * 0.5f, y + h * 0.88f, 110);   /* neon stroke */
}

/* overhead gantry across the road: fork sign with two arrows (0), finish line (1) or checkpoint (2) */
static void draw_gantry(float cx, float sy, float s, float hw, int finish)
{
	float hh = 4200 * s, l = cx - hw * 1.15f, r = cx + hw * 1.15f;
	if (hh < 60 || s > 2.4f) return;                  /* overhead and gone: huge lines are slow */
	line(l, sy, l, sy + hh, 90);
	line(r, sy, r, sy + hh, 90);
	line(l, sy + hh, r, sy + hh, 100);
	if (hh < 250) return;
	if (finish == 2) {                                   /* checkpoint: a second bar and a diamond */
		float m = sy + hh * 0.82f, dy = hh * 0.09f, dx = hw * 0.3f;
		line(l, m, r, m, 100);
		line(cx - dx, m - dy, cx, m - 2 * dy, 100); line(cx, m - 2 * dy, cx + dx, m - dy, 100);
		line(cx + dx, m - dy, cx, m, 100); line(cx, m, cx - dx, m - dy, 100);
	} else if (finish) {
		line(l, sy + hh * 0.82f, r, sy + hh * 0.82f, 100);
		for (int k = 1; k < 8; k++) {
			float xk = l + (r - l) * k / 8;
			line(xk, sy + hh, xk, sy + hh * 0.82f, 100);
		}
	} else {
		for (int side = -1; side <= 1; side += 2) {          /* left and right arrows */
			float ax = cx + side * hw * 0.55f, yy = sy + hh * 0.6f, a = hw * 0.35f * side;
			line(ax - a, yy, ax + a, yy, 100);
			line(ax + a, yy, ax + a * 0.3f, yy + hh * 0.14f, 100);
			line(ax + a, yy, ax + a * 0.3f, yy - hh * 0.14f, 100);
		}
	}
}

/* office tower (CITY, set back from the road, 10 Nov 2026): a slab with a stepped-in crown as ONE path, window
   bands across it when near (drawn alternately from each side) and a mast on some; heights vary (v) */
static void draw_tower(float x, float y, float s, unsigned v)
{
	float h = 7000 * s * (0.8f + 0.15f * (float)(v % 5)), w = 1600 * s, c = h * 0.84f, wc = w * 0.6f;
	if (h < 200) { line(x, y, x, y + h, 55); return; }
	box(x - w, y, x + w, y + c);
	box(x - wc, y + c, x + wc, y + h);
	line(x - w, y, x - w, y + c, 75);
	line(x - w, y + c, x - wc, y + c, 75);
	line(x - wc, y + c, x - wc, y + h, 75);
	line(x - wc, y + h, x + wc, y + h, 75);
	line(x + wc, y + h, x + wc, y + c, 75);
	line(x + wc, y + c, x + w, y + c, 75);
	line(x + w, y + c, x + w, y, 75);
	if (h < 4000) return;                                       /* further off the outline is enough */
	for (int i = 1; i <= 3; i++) {                              /* from the bottom right, back and forth */
		float by = y + c * i / 4;
		line_dir(x - w * 0.75f, by, x + w * 0.75f, by, 50, i & 1);
	}
	if (v % 5 >= 3) line(x, y + h, x, y + h * 1.12f, 70);      /* mast on the two tallest kinds */
}

/* ---- tunnels: shapes in road half widths (x) and world units above the road (y) ---- */
typedef struct { float x, y; } pt_t;
static const pt_t rib_rock[6] = { {-TUN_W, 0}, {-TUN_W, 3400}, {-0.9f, 4400}, {0.9f, 4400}, {TUN_W, 3400}, {TUN_W, 0} };
static const pt_t rib_deck[4] = { {-TUN_W, 0}, {-TUN_W, 4200}, {TUN_W, 4200}, {TUN_W, 0} };
/* the faces round the near mouth: a hillside with peaks (generic rock, VOLCANO), mountain peaks with a snow line
   (ALPINE) and a stepped mesa wall (CANYON); rock mouths sit in a concrete headwall */
static const pt_t face_rock[12] = { {-4.6f, 0}, {-3.7f, 2600}, {-3.0f, 3500}, {-2.1f, 5100}, {-1.2f, 5700}, {-0.3f, 6500},
	{0.6f, 5900}, {1.5f, 6800}, {2.4f, 5500}, {3.3f, 4100}, {4.0f, 2500}, {4.7f, 0} };
static const pt_t face_alp[9] = { {-4.8f, 0}, {-3.4f, 3000}, {-2.3f, 5400}, {-0.7f, 8400}, {0.4f, 7400}, {1.7f, 9200},
	{3.1f, 5900}, {4.4f, 2800}, {5.1f, 0} };
static const pt_t snow_alp[7] = { {-1.5f, 6900}, {-0.9f, 7500}, {-0.3f, 7000}, {0.4f, 7300}, {1.0f, 6900}, {1.6f, 7800}, {2.4f, 7550} };
static const pt_t face_can[10] = { {-4.4f, 0}, {-4.4f, 4200}, {-3.4f, 4200}, {-3.4f, 5800}, {-0.8f, 5800}, {-0.8f, 6600},
	{2.6f, 6600}, {2.6f, 5200}, {4.4f, 5200}, {4.4f, 0} };
static const pt_t headwall[4] = { {-2.1f, 0}, {-2.1f, 5000}, {2.1f, 5000}, {2.1f, 0} };
/* the underpass (CITY): sloping wing walls under a road deck with a railing on top */
static const pt_t deck_wing[4] = { {-7.4f, 0}, {-6.2f, 4200}, {6.2f, 4200}, {7.4f, 0} };
static const pt_t deck_face[4] = { {-6.2f, 4200}, {-6.2f, 5300}, {6.2f, 5300}, {6.2f, 4200} };
static const pt_t deck_rail[2] = { {-6.2f, 6000}, {6.2f, 6000} };

/* a shape as one path, forwards or backwards (so ribs and chains join up end to start) */
static void path(const pt_t *p, int np, float cx, float sy, float hw, float s, int b, int rev)
{
	for (int k = 1; k < np; k++) {
		const pt_t *a = rev ? &p[np - k] : &p[k - 1], *c = rev ? &p[np - 1 - k] : &p[k];
		line(cx + a->x * hw, sy + a->y * s, cx + c->x * hw, sy + c->y * s, b);
	}
}

/* the near mouth from outside: the face round it (the opening is the first rib, drawn by draw_tunnel). The face hides
   the sky behind it in three boxes (left, middle, right), each about as high as the face there. */
static void draw_portal(float cx, float sy, float s, float hw, int theme)
{
	if (4200 * s < 60 || s > 2.7f) return;                    /* far, a speck; close (the last 3 segments), gone */
	if (th_tun[theme] == TUN_DECK) {
		box(cx - 6.2f * hw, sy, cx + 6.2f * hw, sy + 6000 * s);
		path(deck_wing, 4, cx, sy, hw, s, 80, 0);
		path(deck_face, 4, cx, sy, hw, s, 90, 1);
		if (s < 0.25f) return;
		path(deck_rail, 2, cx, sy, hw, s, 70, 0);
		for (int k = 2; k >= -2; k--)                          /* railing posts, back along it */
			line(cx + k * 3.1f * hw, sy + 6000 * s, cx + k * 3.1f * hw, sy + 5300 * s, 60);
		return;
	}
	const pt_t *f = theme == ALPINE ? face_alp : theme == CANYON ? face_can : face_rock;
	int nf = theme == ALPINE ? 9 : theme == CANYON ? 10 : 12;
	static const float bt[3][3] = { { 4000, 6000, 4700 }, { 5000, 8000, 6500 }, { 5200, 6400, 5800 } };   /* box tops */
	const float *t = bt[theme == ALPINE ? 1 : theme == CANYON ? 2 : 0];
	box(cx - 4.0f * hw, sy, cx - 1.4f * hw, sy + t[0] * s);
	box(cx - 1.4f * hw, sy, cx + 1.4f * hw, sy + t[1] * s);
	box(cx + 1.4f * hw, sy, cx + 4.0f * hw, sy + t[2] * s);
	path(f, nf, cx, sy, hw, s, 75, 0);
	if (s < 0.2f) return;                                       /* far: the hill alone */
	path(headwall, 4, cx, sy, hw, s, 90, 1);
	if (theme == ALPINE) path(snow_alp, 7, cx, sy, hw, s, 110, 0);
	if (theme == CANYON) {                                      /* rock strata either side of the headwall */
		line(cx - 4.4f * hw, sy + 2400 * s, cx - 2.1f * hw, sy + 2400 * s, 55);
		line(cx + 2.1f * hw, sy + 2400 * s, cx + 4.4f * hw, sy + 2400 * s, 55);
	}
}

static void draw_vehicle(float x, float y, float s, int truck, int bright)
{
	if (!truck) {
		float w = 0.35f * ROAD_W * s, h = 0.32f * ROAD_W * s;
		line(x - w, y, x + w, y, bright);
		line(x + w, y, x + w * 0.8f, y + h, bright);
		line(x + w * 0.8f, y + h, x - w * 0.8f, y + h, bright);
		line(x - w * 0.8f, y + h, x - w, y, bright);
	} else {                                     /* slim box trailer seen from behind */
		float w = 0.315f * ROAD_W * s, h = 0.75f * ROAD_W * s;
		line(x - w, y, x + w, y, bright);
		line(x + w, y, x + w, y + h, bright);
		line(x + w, y + h, x - w, y + h, bright);
		line(x - w, y + h, x - w, y, bright);
		if (h > 400) {
			line(x, y + h * 0.1f, x, y + h * 0.9f, bright - 25);                  /* doors */
			line(x - w * 1.15f, y, x - w * 1.15f, y + h * 0.18f, bright - 15);   /* wheels */
			line(x + w * 1.15f, y, x + w * 1.15f, y + h * 0.18f, bright - 15);
		}
	}
}

/* twin-light GT, rear view (John's pick, 8 Nov): wide hips over the wheels, a fastback cabin that leans
   with the steering, a light bar each side (full brightness while braking) and an inset diffuser.
   Drawn as few paths as possible: the body and diffuser are one closed walk. */
static int braking;
static void draw_player(float steer)
{
	float y = -17500, lean = steer * 500;
	int lights = braking ? 127 : 105;
	/* body: from the diffuser's left foot round the left hip, over the cabin, down the right hip,
	   back along the bottom, then up and across the diffuser to close where it began */
	line(-1500, y - 300, -5000, y - 300, 110);
	line(-5000, y - 300, -5200, y + 500, 110);
	line(-5200, y + 500, -4700, y + 1300, 110);
	line(-4700, y + 1300, -2600 + lean, y + 1500, 110);
	line(-2600 + lean, y + 1500, -1800 + lean, y + 2500, 110);
	line(-1800 + lean, y + 2500, 1800 + lean, y + 2500, 110);
	line(1800 + lean, y + 2500, 2600 + lean, y + 1500, 110);
	line(2600 + lean, y + 1500, 4700, y + 1300, 110);
	line(4700, y + 1300, 5200, y + 500, 110);
	line(5200, y + 500, 5000, y - 300, 110);
	line(5000, y - 300, 1500, y - 300, 110);
	line(1500, y - 300, 1200, y + 300, 80);                     /* diffuser */
	line(1200, y + 300, -1200, y + 300, 80);
	line(-1200, y + 300, -1500, y - 300, 80);
	line(4400, y + 800, 1200, y + 800, lights);                 /* light bars */
	line(-1200, y + 800, -4400, y + 800, lights);
	line(-4900, y - 900, -3100, y - 900, 90);                   /* tyres */
	line(3100, y - 900, 4900, y - 900, 90);
}

static void draw_object(const seg_t *sg, float cx, float sy, float s, float hw)
{
	float d = sg->fork, o = sg->side * hw;
	switch (sg->obj) {
	case OBJ_FORKSIGN: draw_gantry(cx, sy, s, hw, 0); return;
	case OBJ_FINISH:   draw_gantry(cx, sy, s, hw, 1); return;
	case OBJ_CHECK:    draw_gantry(cx, sy, s, hw, 2); return;
	case OBJ_PORTAL:   draw_portal(cx, sy, s, hw, sg->theme); return;
	default: break;
	}
	static const float lat_off[3] = { 0.0f, 0.9f, 2.0f };          /* extra half-widths out from the road */
	float x = cx + o * (1.6f + d + lat_off[sg->lat]), lx = cx + o * (1.25f + d);
	switch (sg->obj) {
	case OBJ_PALM:     draw_palm(x, sy, s); break;
	case OBJ_ROUND:    draw_round_tree(x, sy, s); break;
	case OBJ_CACTUS:   draw_cactus(x, sy, s); break;
	case OBJ_ROCK:     draw_rock(x, sy, s); break;
	case OBJ_LAMP:     draw_lamp(lx, sy, s, sg->side); break;
	case OBJ_PINE:     draw_pine(x, sy, s, 0); break;
	case OBJ_SNOWPINE: draw_pine(x, sy, s, 1); break;
	case OBJ_PILLAR:   draw_pillar(x, sy, s); break;
	case OBJ_REED:     draw_reed(x, sy, s); break;
	case OBJ_POST:     draw_post(lx, sy, s); break;
	case OBJ_LAVA:     draw_lava(x, sy, s); break;
	case OBJ_NEON:     draw_neon(lx, sy, s); break;
	case OBJ_POPLAR:   draw_poplar(x, sy, s); break;
	case OBJ_BIRCH:    draw_birch(x, sy, s); break;
	case OBJ_PARASOL:  draw_parasol(x, sy, s); break;
	case OBJ_GRASS:    draw_grass(x, sy, s); break;
	case OBJ_HUT:      draw_hut(x, sy, s); break;
	case OBJ_TOWER:    draw_tower(x + o * 1.2f, sy, s, hsh((unsigned)(sg - seg))); break;   /* further back: near, a tower's long
	                                                                                      walls run off the side, not up the screen */
	default: break;
	}
}

/* road lines beyond a tunnel mouth show only through it (vectors cannot be hidden): from outside through the near
   mouth, from inside through the far one. draw_road sets clip_from (the first segment n beyond the mouth) and cw_* */
static int clip_from = DRAW_DIST + 1;
static float cw_l, cw_b, cw_r, cw_t;

static void seg_clip(int n)
{
	if (n >= clip_from) clip_to(cw_l, cw_b, cw_r, cw_t);
	else clip_screen();
}

/* the scenery on one side (-1 left, 1 right, 0 the gantries across the road),
   outwards (up) or back; draw_road has registered its sky boxes already */
static void draw_objects(int base, int which, int up)
{
	obj_pass = 2;
	for (int i = 0; i < DRAW_DIST; i++) {
		int n = up ? i : DRAW_DIST - 1 - i;
		if (!obj_on[n]) continue;
		const seg_t *sg = get_seg(base + n);
		int w = OBJ_GANTRY(sg->obj) ? 0 : sg->side < 0 ? -1 : 1;
		if (w != which) continue;
		if (sg->obj == OBJ_PORTAL) clip_screen(); else seg_clip(n);   /* beyond a mouth: only through it */
		draw_object(sg, proj[n].x, proj[n].sy, proj[n].scale, ROAD_W * proj[n].scale);
	}
	clip_screen();
	obj_pass = 0;
}

/* the inside of the tunnel: draw_road lists the visible rib segments (every 4th, near to far) and the roof light
   segments between them. Ribs go out as one zig-zag chain (each drawn the other way from the last), then the two wall
   lines and the roof lights. From outside it all shows only through the near mouth (clipped to it). */
#define RIB_MAX 16
static unsigned char rib_n[RIB_MAX], lit_n[RIB_MAX];
static int n_rib, n_lit;

static void draw_tunnel(int base)
{
	if (!n_rib) return;
	int deck = tun_kind == TUN_DECK, np = deck ? 4 : 6, ne = tun_a - base, k0 = 0;
	const pt_t *rib = deck ? rib_deck : rib_rock;
	float top = deck ? 4200 : 3400;                         /* the wall lines run along the top of the walls */
	if (ne > 3) {                                            /* outside: the mouth itself, then the rest through it */
		if (rib_n[0] != ne) return;                          /* (the mouth is hidden: nothing shows) */
		float s = proj[ne].scale, hw = ROAD_W * s;
		path(rib, np, proj[ne].x, proj[ne].sy, hw, s, ne < 15 ? 100 : ne < 35 ? 80 : 60, 0);
		clip_to(cw_l, cw_b, cw_r, cw_t);
		k0 = 1;
	}
	for (int k = k0; k < n_rib; k++) {
		int n = rib_n[k];
		float s = proj[n].scale;
		int b = base + n == tun_b - 1 ? 110 : n < 15 ? 100 : n < 35 ? 80 : 60;   /* the far mouth stands out */
		path(rib, np, proj[n].x, proj[n].sy, ROAD_W * s, s, b, (k - k0) & 1);
	}
	/* wall lines: back along the side the ribs ended on, then out along the other */
	int side = ((n_rib - k0) & 1) ? 1 : -1;
	for (int pass = 0; pass < 2; pass++, side = -side)
		for (int j = 1; j < n_rib; j++) {
			int a = pass ? j - 1 : n_rib - j, c = pass ? j : n_rib - 1 - j;   /* pass 0: far to near */
			int na = rib_n[a], nc = rib_n[c];
			line(proj[na].x + side * TUN_W * ROAD_W * proj[na].scale, proj[na].sy + top * proj[na].scale,
			     proj[nc].x + side * TUN_W * ROAD_W * proj[nc].scale, proj[nc].sy + top * proj[nc].scale, 70);
		}
	for (int k = n_lit - 1; k >= 0; k--) {                   /* roof lights, back towards the car */
		int n = lit_n[k];
		if (n < rib_n[0] || n + 1 >= DRAW_DIST || !proj[n + 1].vis) continue;   /* (none in front of the nearest rib) */
		float s0 = proj[n].scale, s1 = proj[n + 1].scale, y = deck ? 4200 : 4400;
		for (int l = deck ? -1 : 0; l <= (deck ? 1 : 0); l += 2) {
			float o = deck ? 0.8f * l * ROAD_W : 0;
			line(proj[n + 1].x + o * s1, proj[n + 1].sy + y * s1, proj[n].x + o * s0, proj[n].sy + y * s0, 95);
		}
	}
	clip_screen();
}

static void draw_road(void)
{
	int base = (int)(pos_z / SEG_LEN);
	float pct = fmodf(pos_z, SEG_LEN) / SEG_LEN;
	const seg_t *s0 = get_seg(base), *s1 = get_seg(base + 1);
	float py = s0->y + (s1->y - s0->y) * pct;
	float x = 0, dx = -s0->curve * pct;
	float maxy = -1e9f;                          /* the nearest segment always counts */
	int have_prev = 0;
	float pc = 0, pcy = 0;                                              /* previous segment centre */
	int right_now = player_x >= 0;               /* branch the car is on at the moment */
	rb_n = 0; bx_n = 0; n_rib = n_lit = 0;

	for (int n = 0; n < DRAW_DIST; n++) {
		const seg_t *sg = get_seg(base + n);
		dash_b[n] = kerb_b[n] = obj_on[n] = 0;
		float z = (n + 1 - pct) * SEG_LEN;
		x += dx; dx += sg->curve;
		float s = FOV / z;
		float cx = (x - player_x * ROAD_W) * s;
#ifdef VD_CHECKPOINT
		if (sg->fork > FORK_DMAX) {                  /* past the split your road runs straight on and */
			float d0 = fork_prev > FORK_DMAX ? fork_prev : FORK_DMAX;   /* only the other one swings away */
			cx -= (right_now ? 1 : -1) * (sg->fork - d0) * ROAD_W * s;
		}
#endif
		float sy = hz + ((sg->y - py) * HILL_K - CAM_H) * s;
		float u = sy - hz;                           /* squeeze the part above the horizon: monotonic,  */
		if (u > CLIMB_A) {                           /* so crest hiding still works, but the road can't */
			float w = (u - CLIMB_A) / CLIMB_B;       /* climb through the sun and the skyline */
			sy = hz + CLIMB_A + CLIMB_B * w / (1 + w);
		}
		float hw = ROAD_W * s;
		proj[n].scale = s; proj[n].x = cx; proj[n].sy = sy; proj[n].vis = 0;
		if (sy <= maxy) continue;                    /* hidden behind a crest */
		proj[n].vis = 1;
		int bright = n < 15 ? 110 : n < 35 ? 85 : 60;
		float d = sg->fork;
		float lo = cx - (d + 1) * hw, ro = cx + (d + 1) * hw;       /* outer edges */
		float il = d > 1 ? cx - (d - 1) * hw : cx, ir = d > 1 ? cx + (d - 1) * hw : cx;
		/* far away the edges are drawn every 2nd / 4th segment: fewer, longer lines */
		if (n < (d > 0 ? 10 : 24) || (n < 44 && !(n & 1)) || !(n & 3)) {
			int bl = bright, br = bright;            /* of the edge lines ending here */
			if (d >= 1) {                            /* the road not taken fades away */
				int dim = base + n >= nseg || n > 26 ? 0 : bright * 2 / 5;
				if (right_now) bl = dim; else br = dim;
			}
			rb_y[rb_n] = sy; rb_l[rb_n] = lo; rb_r[rb_n] = ro;
			rb_il[rb_n] = il; rb_ir[rb_n] = ir; rb_c[rb_n] = cx; rb_d[rb_n] = d; rb_seg[rb_n] = (unsigned char)n;
			rb_bl[rb_n] = bl; rb_br[rb_n] = br; rb_n++;
		}
		if (have_prev && n >= 2 && d <= 1) {
			const int dm = th_style[sg->theme].dash, km = th_style[sg->theme].kerb;
			if (dm >= 0 && !((base + n) & dm) && (dm != 1 || n < 30)) {   /* centre dashes */
				dash_b[n] = bright - 20; dash_x[n] = pc; dash_y[n] = pcy;
			}
			if (km >= 0 && d == 0 && !((base + n) & km) && n < 40)       /* kerb ticks */
				kerb_b[n] = bright - 20;
		}
		/* sky boxes now, nearest first; lines later. Roadside scenery is gone before it reaches the very
		   front (John, 8 Nov: near palms filled half the screen); the gantries stay so you drive under them. */
		if (sg->obj && n < (d > 0 ? 22 : 40) &&
		    (n >= OBJ_NEAR || OBJ_GANTRY(sg->obj))) {
			obj_on[n] = 1;
			obj_pass = 1; draw_object(sg, cx, sy, s, hw); obj_pass = 0;
		}
		int ti = base + n - tun_a;                   /* tunnel ribs (every 4th segment) and roof lights between */
		if (tun_kind && ti >= 0 && base + n < tun_b && n >= 2) {
			if (!(ti & 3) && n < 40 && n_rib < RIB_MAX) rib_n[n_rib++] = (unsigned char)n;
			else if ((ti & 3) == 2 && n < 32 && n_lit < RIB_MAX) lit_n[n_lit++] = (unsigned char)n;
		}
		pc = cx; pcy = sy;
		have_prev = 1;
		maxy = sy;
	}
#ifdef VD_SKY_FOLLOWS_CREST
	road_top = maxy;                             /* the crest: the highest road point drawn */
#endif
	sky_win = 0;                                 /* in a tunnel (from just before its mouth) the sky shows only */
	if (tun_kind && base >= tun_a - 3 && base < tun_b - 1) {   /* through the far mouth */
		int ne = tun_b - 1 - base;
		sky_win = 2;
		if (ne < DRAW_DIST && proj[ne].vis) {
			float s = proj[ne].scale, hw = ROAD_W * s;
			sky_win = 1;
			sw_l = proj[ne].x - TUN_W * hw; sw_r = proj[ne].x + TUN_W * hw;
			sw_b = proj[ne].sy; sw_t = proj[ne].sy + (tun_kind == TUN_DECK ? 4200 : 3400) * s;   /* (below the arch's corners) */
		}
	}
	clip_from = DRAW_DIST + 1;
	if (sky_win == 1) {
		clip_from = tun_b - 1 - base; cw_l = sw_l; cw_b = sw_b; cw_r = sw_r; cw_t = sw_t;
	} else if (tun_kind && tun_a - base > 3 && tun_a - base < DRAW_DIST && proj[tun_a - base].vis) {
		int ne = tun_a - base;
		float s = proj[ne].scale, hw = ROAD_W * s;
		clip_from = ne; cw_l = proj[ne].x - TUN_W * hw; cw_r = proj[ne].x + TUN_W * hw;
		cw_b = proj[ne].sy; cw_t = proj[ne].sy + (tun_kind == TUN_DECK ? 4200 : 4400) * s;
	}

	/* The road goes out as chains, each pass starting near where the last one ended:
	   a jump across the road costs more than a line, and on the UVMC2 it also re-centres
	   the beam ("Vectrex Mastery": count strokes, not vectors). Same lines as before. */
	for (int k = 1; k < rb_n; k++)                                   /* left edge, outwards */
		if (rb_bl[k]) seg_clip(rb_seg[k - 1]), line(rb_l[k - 1], rb_y[k - 1], rb_l[k], rb_y[k], rb_bl[k]);
	for (int k = rb_n - 1; k >= 1; k--)                              /* left median, back */
		if (rb_d[k] > 1 && rb_bl[k])                                 /* (grows out of the split point) */
			seg_clip(rb_seg[k - 1]), line_dir(rb_d[k - 1] > 1 ? rb_il[k - 1] : rb_c[k - 1], rb_y[k - 1], rb_il[k], rb_y[k], rb_bl[k], 1);
	clip_screen();
	draw_objects(base, -1, 0);                                       /* left scenery, back */
	for (int n = 0; n < DRAW_DIST; n++)                              /* left kerb ticks, outwards */
		if (kerb_b[n]) {
			float hw = ROAD_W * proj[n].scale;
			seg_clip(n);
			line(proj[n].x - hw, proj[n].sy, proj[n].x - hw * 0.8f, proj[n].sy, kerb_b[n]);
		}
	clip_screen();
	draw_objects(base, 0, 0);                                        /* gantries */
	for (int n = DRAW_DIST - 1; n >= 0; n--)                         /* centre dashes, back */
		if (dash_b[n]) seg_clip(n), line_dir(dash_x[n], dash_y[n], proj[n].x, proj[n].sy, dash_b[n], 1);
	for (int n = 0; n < DRAW_DIST; n++)                              /* right kerb ticks, outwards */
		if (kerb_b[n]) {
			float hw = ROAD_W * proj[n].scale;
			seg_clip(n);
			line(proj[n].x + hw, proj[n].sy, proj[n].x + hw * 0.8f, proj[n].sy, kerb_b[n]);
		}
	clip_screen();
	draw_objects(base, 1, 0);                                        /* right scenery, back */
	for (int k = 1; k < rb_n; k++)                                   /* right median, outwards */
		if (rb_d[k] > 1 && rb_br[k])
			seg_clip(rb_seg[k - 1]), line(rb_d[k - 1] > 1 ? rb_ir[k - 1] : rb_c[k - 1], rb_y[k - 1], rb_ir[k], rb_y[k], rb_br[k]);
	for (int k = rb_n - 1; k >= 1; k--)                              /* right edge, back */
		if (rb_br[k]) seg_clip(rb_seg[k - 1]), line_dir(rb_r[k - 1], rb_y[k - 1], rb_r[k], rb_y[k], rb_br[k], 1);
	clip_screen();
	draw_tunnel(base);

	for (int i = 0; i < N_CARS; i++) {              /* traffic (hidden behind crests too) */
		float rel = cars[i].z - pos_z;
		if (rel < 0) continue;
		int n = (int)(rel / SEG_LEN);
		if (n < 1 || n >= DRAW_DIST || !proj[n].vis) continue;
		seg_clip(n);
		draw_vehicle(proj[n].x + cars[i].x * ROAD_W * proj[n].scale, proj[n].sy, proj[n].scale,
		             cars[i].truck, 100);
		{                                            /* the vehicle hides the sky behind it as well */
			float vx = proj[n].x + cars[i].x * ROAD_W * proj[n].scale, vs = proj[n].scale * ROAD_W;
			box(vx - vs * (cars[i].truck ? 0.315f : 0.35f), proj[n].sy, vx + vs * (cars[i].truck ? 0.315f : 0.35f),
			    proj[n].sy + vs * (cars[i].truck ? 0.75f : 0.32f));
		}
	}
	clip_screen();
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

/* ---- user music: .ay14 register dumps (AY R0..R13, one 14-byte frame per
   50 Hz tick, recorded at a 1 MHz AY clock). An optional <name>.loop holds
   "loop=<frame>" (default 0). Files are looked up in MUSIC_DIR (the env var
   VD_MUSIC_DIR overrides it) and a dump is loaded when selected. ---- */
#define MUSIC_DIR   "/media/fat/pitrex/music"
#define MAX_TRACKS  16
static struct { char path[300]; char name[28]; } track[MAX_TRACKS];     /* path first: qsort sorts on it */
static int n_tracks;
static unsigned char *dump;                       /* the selected dump, if any */
static long dump_frames, dump_pos, dump_loop;
static int chip_r13;                              /* what the chip's envelope shape register holds */

static int track_cmp(const void *a, const void *b)
{
	return strcmp((const char *)a, (const char *)b);
}

#ifdef VD_UVMC2
/* The cartridge build reads the same .ay14/.loop files from the UVMC2 card, folder VECDRIVE/ (on the
   MiSTer: games/Vectrex-PiTrex/UVMC2/VECDRIVE, put on sd.img by the card builder). A dump is not
   loaded whole (a 3-minute track is ~100 KB): it is streamed through a 64-frame buffer, one card read
   every ~1.3 s, and the loop point is a seek. */
#include "uvm2_sd.h"
#define CARD_MUSIC "VECDRIVE"
static uvm2_sd_file mus_f;
static unsigned char mus_buf[14 * 64];
static long mus_first, mus_n;                    /* frame number of mus_buf[0], frames held */
static int mus_track = -1;                       /* the open track, to reopen it after the end */

static int ext_is_ay14(const char *s, size_t len)
{
	const char *e = s + len - 5, *w = ".ay14";
	for (int i = 0; i < 5; i++) {
		char c = e[i];
		if (c >= 'A' && c <= 'Z') c += 32;
		if (c != w[i]) return 0;
	}
	return 1;
}
#endif

/* the 14 registers of frame `pos` of the selected dump, or NULL */
static const unsigned char *dump_frame(long pos)
{
#ifdef VD_UVMC2
	if (mus_track < 0) return NULL;
	if (pos < mus_first || pos >= mus_first + mus_n) {
		uint32_t got;
		if (!mus_f.ok || !uvm2_sd_seek(&mus_f, (uint32_t)(pos * 14))) {
			/* reading to the end may leave the file closed: reopen it for the loop */
			if (mus_track < 0) return NULL;
			uvm2_sd_close(&mus_f);
			if (!uvm2_sd_open(track[mus_track].path, &mus_f) || !uvm2_sd_seek(&mus_f, (uint32_t)(pos * 14))) return NULL;
		}
		got = uvm2_sd_next(&mus_f, mus_buf, sizeof mus_buf);
		mus_first = pos; mus_n = (long)(got / 14);
		if (!mus_n) return NULL;
	}
	return mus_buf + (pos - mus_first) * 14;
#else
	return dump ? dump + pos * 14 : NULL;
#endif
}

static void scan_music(void)
{
#ifdef VD_UVMC2
	uvm2_sd_dir d;
	char nm[120];
	uint32_t size;
	int k;
	if (!uvm2_sd_init() || !uvm2_sd_dir_open(CARD_MUSIC, &d)) return;
	while (n_tracks < MAX_TRACKS && (k = uvm2_sd_dir_next(&d, nm, sizeof nm, &size)) != UVM2_SD_DIR_END) {
		size_t len = strlen(nm);
		if (k != UVM2_SD_DIR_FILE || len < 6 || size < 14 || !ext_is_ay14(nm, len)) continue;
		snprintf(track[n_tracks].path, sizeof track[0].path, "%s/%s", CARD_MUSIC, nm);
		n_tracks++;
	}
	uvm2_sd_dir_close(&d);
	qsort(track, n_tracks, sizeof track[0], track_cmp);
	for (int i = 0; i < n_tracks; i++) {                   /* banner name: base name, upper case */
		const char *b = strrchr(track[i].path, '/') + 1;
		int j = 0;
		for (; b[j] && b[j] != '.' && j < (int)sizeof track[0].name - 1; j++)
			track[i].name[j] = (b[j] >= 'a' && b[j] <= 'z') ? b[j] - 32 : b[j];
		track[i].name[j] = 0;
	}
#else
	const char *dir = getenv("VD_MUSIC_DIR");
	DIR *d;
	struct dirent *e;
	if (!dir || !*dir) dir = MUSIC_DIR;
	if (!(d = opendir(dir))) return;
	while ((e = readdir(d)) != NULL && n_tracks < MAX_TRACKS) {
		size_t len = strlen(e->d_name);
		if (len < 6 || len > 120 || strcasecmp(e->d_name + len - 5, ".ay14") != 0) continue;
		snprintf(track[n_tracks].path, sizeof track[0].path, "%s/%s", dir, e->d_name);
		n_tracks++;
	}
	closedir(d);
	qsort(track, n_tracks, sizeof track[0], track_cmp);
	for (int i = 0; i < n_tracks; i++) {                   /* banner name: base name, upper case */
		const char *b = strrchr(track[i].path, '/');
		int k = 0;
		b = b ? b + 1 : track[i].path;
		for (; b[k] && b[k] != '.' && k < (int)sizeof track[0].name - 1; k++)
			track[i].name[k] = (b[k] >= 'a' && b[k] <= 'z') ? b[k] - 32 : b[k];
		track[i].name[k] = 0;
	}
#endif
}

static void load_track(int i)
{
#ifdef VD_UVMC2
	char lp[310], txt[64];
	uint32_t n;
	if (mus_f.ok) uvm2_sd_close(&mus_f);
	mus_first = mus_n = 0; mus_track = -1;
	dump_frames = dump_pos = dump_loop = 0;
	if (i < 0 || i >= n_tracks || !uvm2_sd_open(track[i].path, &mus_f)) return;
	dump_frames = (long)(mus_f.len / 14);
	mus_track = i;
	snprintf(lp, sizeof lp, "%s", track[i].path);
	strcpy(strrchr(lp, '.'), ".loop");
	if ((n = uvm2_sd_read(lp, (unsigned char *)txt, sizeof txt - 1)) > 0) {
		txt[n] = 0;
		for (char *s = txt; *s; s++)
			if (strncmp(s, "loop=", 5) == 0) { dump_loop = atol(s + 5); break; }
	}
	if (dump_loop < 0 || dump_loop >= dump_frames) dump_loop = 0;
#else
	char lp[310], line[64];
	FILE *f;
	free(dump); dump = NULL; dump_frames = dump_pos = dump_loop = 0;
	if (i < 0 || i >= n_tracks || !(f = fopen(track[i].path, "rb"))) return;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (size >= 14 && (dump = malloc((size_t)size)) != NULL) {
		if (fread(dump, 1, (size_t)size, f) == (size_t)size) dump_frames = size / 14;
		else { free(dump); dump = NULL; }
	}
	fclose(f);
	snprintf(lp, sizeof lp, "%s", track[i].path);
	strcpy(strrchr(lp, '.'), ".loop");
	if ((f = fopen(lp, "r")) != NULL) {
		while (fgets(line, sizeof line, f))
			if (strncmp(line, "loop=", 5) == 0) dump_loop = atol(line + 5);
		fclose(f);
	}
	if (dump_loop < 0 || dump_loop >= dump_frames) dump_loop = 0;
#endif
}

/* one 50 Hz tick of the dump, rescaled from the 1 MHz CPC clock to the
   Vectrex's 1.5 MHz: periods x 3/2 (rounded, clamped to their field widths) */
static int x32(int v, int max) { v = (v * 3 + 1) / 2; return v > max ? max : v; }

static void dump_tick(void)
{
	const unsigned char *r = dump_frame(dump_pos);
	int out[14], i;
	if (!r) return;
	for (i = 0; i < 3; i++) {                        /* tone periods, 12 bit */
		int v = x32(r[i * 2] | ((r[i * 2 + 1] & 0x0F) << 8), 0x0FFF);
		out[i * 2] = v & 0xFF; out[i * 2 + 1] = v >> 8;
	}
	out[6] = x32(r[6] & 0x1F, 0x1F);                 /* noise, 5 bit */
	out[7] = r[7] & 0x3F;                            /* IOA stays an input; bit 7 clear too */
	out[8] = r[8] & 0x1F; out[9] = r[9] & 0x1F; out[10] = r[10] & 0x1F;
	{
		int v = x32(r[11] | (r[12] << 8), 0xFFFF);   /* envelope period, 16 bit */
		out[11] = v & 0xFF; out[12] = v >> 8;
	}
	out[13] = r[13] & 0x0F;
	for (i = 0; i < 13; i++) v_writePSG(i, out[i]);
	if (out[13] != chip_r13) { v_writePSG(13, out[13]); chip_r13 = out[13]; }   /* a write restarts the envelope */
#ifdef VD_MUSIC_TRACE
	fprintf(stderr, "M %ld", dump_pos);
	for (i = 0; i < 14; i++) fprintf(stderr, " %d", out[i]);
	fprintf(stderr, "\n");
#endif
	if (++dump_pos >= dump_frames) dump_pos = dump_loop;
}

static int beep_frames;
static void beep(int period, int frames)
{
	if (music_mode >= 2) return;                     /* a playing dump owns all three channels */
	psg_tone(2, period);                         /* channel C for effects */
	v_writePSG(10, 0x0D);
	beep_frames = frames;
}

static void sound_tick(int offroad)
{
	if (music_mode >= 2) {                           /* user dump: no engine, no effects */
		if (dump_frames && state != ATTRACT) {
			dump_tick();
		} else {
			v_writePSG(7, 0x3F);
			v_writePSG(8, 0); v_writePSG(9, 0); v_writePSG(10, 0);
		}
		return;
	}
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

static void show_banner2(const char *text, int frames)
{
	snprintf(banner2, sizeof banner2, "%s", text);
	banner2_timer = frames;
}

/* spread the traffic over the stretch between `from` and the fork sign */
static void place_cars(float from)
{
	float limit = (fork_start - 30) * SEG_LEN, span = limit - from;
	for (int i = 0; i < N_CARS; i++) {
		cars[i].z = span > 0 ? from + span * (i + 1) / (N_CARS + 1) : -1e9f;
		cars[i].x = (i & 1) ? -0.45f : 0.45f;
		cars[i].truck = (i % 3 == 1);
		/* trucks in the two lanes run at different speeds: two side by side fill the road, and at one speed they
		   stayed that way for good (John's photos, 9 Nov 2026) */
		cars[i].speed = MAX_SPEED * (cars[i].truck ? 0.28f + 0.04f * (i & 1) : 0.40f + 0.06f * (i % 4)) * (1 + 0.04f * (ph - 1));
	}
}

static void reset_race(void)
{
	dump_pos = 0;                                   /* a dump restarts with every race */
	pos_z = 0; speed = 0; player_x = 0; sun_x = 0; sky_x = 0; hz_off = 0; hz = HORIZON; sky_w = 1;
	time_left = ph_time[1]; race_frames = 0;
#ifdef VD_CHECKPOINT
	check_due = 0; fork_prev = 0;
#endif
#ifdef VD_START_TIME
	time_left = VD_START_TIME;                       /* debug: short clock to see TIME UP */
#endif
	ph = 1; cur_pos = 0; ph_theme = SEA; prev_theme = -1;
	build_phase(ph_theme, prev_theme, ph);
	place_cars(SEG_LEN * 150);
}

/* the fork is over: pick the branch under the car, build the next phase and
   add its allowance */
static void advance_phase(void)
{
	int dir = player_x >= 0;
	float old_len = track_len, fend = seg[nseg - 1].fork;   /* branch offset where the old track ends */
	char t[24];
	cur_pos = next_pos(ph, cur_pos, dir);
	prev_theme = ph_theme; ph++;
	ph_theme = lat_theme[ph - 1][cur_pos];
	build_phase(ph_theme, prev_theme, ph);
	pos_z -= old_len; if (pos_z < 0) pos_z = 0;
	player_x -= dir ? fend : -fend;                   /* now relative to the chosen road */
	if (player_x < -2.5f) player_x = -2.5f;
	if (player_x > 2.5f) player_x = 2.5f;
#ifdef VD_CHECKPOINT
	check_due = 1;                                    /* the allowance comes at the checkpoint gantry */
	(void)t;
#else
	time_left += ph_time[ph];
	snprintf(t, sizeof t, "PHASE %d  %s", ph, theme_name[ph_theme]);
	show_banner(t, 150);
	snprintf(t, sizeof t, "EXTRA TIME %d", (int)ph_time[ph]);
	show_banner2(t, 150);
	beep(60, 12);
#endif
	place_cars(pos_z + SEG_LEN * 120);
#ifdef VD_STATS
	fprintf(stderr, "PH %d dir %d theme %s race_frames %d left %.1f len %d\n", ph, dir, theme_name[ph_theme], race_frames, time_left, nseg);
#endif
}

static void update(void)
{
	float dt = 1.0f / FPS;
	int racing = state == RACE;
	int gas = racing && (currentButtonState & 0x01);
	int brake = racing && (currentButtonState & 0x02);
	braking = brake;
	float steer = currentJoy1X / 127.0f;
	float pct = speed / MAX_SPEED;
	int base = (int)(pos_z / SEG_LEN);
	const seg_t *cs = get_seg(base);

#ifdef VD_CHECKPOINT
	{   /* where the roads bend apart the car keeps to its own road: move it out with the branch */
		float f = cs->fork + (get_seg(base + 1)->fork - cs->fork) * (pos_z / SEG_LEN - base);
		if (f > FORK_DMAX && f > fork_prev) player_x += (player_x >= 0 ? 1 : -1) * (f - fork_prev);
		fork_prev = f;
	}
#endif
	player_x += steer * dt * 2.2f * pct;
	player_x -= dt * 2 * pct * pct * cs->curve * CENTRIFUGAL;
	float lim = 2.5f + cs->fork;
	if (player_x < -lim) player_x = -lim;
	if (player_x > lim) player_x = lim;

	speed += (gas ? ACCEL : (brake || state == GOAL) ? BRAKE * (state == GOAL ? 0.5f : 1.0f) : DECEL) * dt;
	/* on road = within one road width of a branch centre (0 before the fork) */
	int offroad = fabsf(fabsf(player_x) - cs->fork) > 1.0f;
	if (offroad && speed > OFF_LIMIT) speed += OFF_DECEL * dt;
	if (speed < 0) speed = 0;
	if (speed > MAX_SPEED) speed = MAX_SPEED;

	sun_x -= cs->curve * pct * 120;                  /* sky parallax follows the bends */
	sky_x += cs->curve * pct * 60;

	pos_z += speed * dt;

	/* camera pitch: a climb tilts the view up (scene moves down), at most a
	   little, and eases back to the printed horizon on level road */
	float slope = HILL_K * (get_seg(base + 6)->y - cs->y) / (6 * SEG_LEN);
	float target = -slope * 350.0f;
	if (target > 450) target = 450;
	if (target < -450) target = -450;
	hz_off += (target - hz_off) * 0.08f;
	hz = HORIZON + hz_off;
	sky_w = zone_len > 0 && base < zone_len ? (float)base / zone_len : 1.0f;

	if (racing && pos_z >= track_len) {
		if (ph >= 4) {                               /* finish line */
			state = GOAL; state_timer = 400;
#ifdef VD_STATS
			fprintf(stderr, "GOAL race_frames %d left %.1f\n", race_frames, time_left);
#endif
			show_banner("GOAL!", 400);
			beep(60, 30);
		} else {
			advance_phase();
		}
		base = (int)(pos_z / SEG_LEN);
	}
#ifdef VD_CHECKPOINT
	if (racing && check_due && base >= CHECK_SEG) {
		char t[24];
		check_due = 0;
		time_left += ph_time[ph];
		snprintf(t, sizeof t, "CHECKPOINT  +%d", (int)ph_time[ph]);   /* one line: here the scenery is still changing over */
		show_banner2(t, 150);
		beep(60, 12);
#ifdef VD_STATS
		fprintf(stderr, "CHECKPOINT ph %d race_frames %d left %.1f x %.2f\n", ph, race_frames, time_left, player_x);
#endif
	}
#endif

	for (int i = 0; i < N_CARS; i++) {
#ifdef VD_NOCARS
		cars[i].z = -1e9f;                         /* debug: empty road */
#endif
		cars[i].z += cars[i].speed * dt;
		float rel = cars[i].z - pos_z;
		if (rel < -3 * SEG_LEN) {                    /* left behind: send it ahead again */
			float z = pos_z + SEG_LEN * (280 + hsh(i * 31 + (unsigned)(pos_z / SEG_LEN)) % 260);
			for (int t = 0; t < 4; t++) {            /* never level with one in the other lane: a gap to pass */
				int clash = 0;
				for (int j = 0; j < N_CARS; j++)
					if (j != i && cars[j].x != cars[i].x && fabsf(cars[j].z - z) < 15 * SEG_LEN) clash = 1;
				if (!clash) break;
				z += 20 * SEG_LEN;
			}
			cars[i].z = z < (fork_start - 30) * SEG_LEN ? z : -1e9f;
			continue;
		}
		float halfw = cars[i].truck ? 0.5f : 0.55f;
		if (rel >= 0 && rel < SEG_LEN * 0.8f && fabsf(player_x - cars[i].x) < halfw && speed > cars[i].speed) {
			speed = cars[i].speed * 0.5f;          /* bump */
			beep(900, 6);
		}
	}

#ifdef VD_STATS
	{   /* frames with a wall ahead: two vehicles level (within a segment) in the two lanes, 0-40 segments ahead */
		static int walls;
		for (int i = 0; i < N_CARS; i++)
			for (int j = i + 1; j < N_CARS; j++) {
				float ri = cars[i].z - pos_z;
				if (cars[i].x != cars[j].x && ri > 0 && ri < 40 * SEG_LEN && fabsf(cars[i].z - cars[j].z) < SEG_LEN) {
					if (++walls % 50 == 1) fprintf(stderr, "WALL frames %d (race_frames %d)\n", walls, race_frames);
					i = j = N_CARS;
				}
			}
	}
#endif
	if (racing) {
		race_frames++;
		time_left -= dt;
		if (time_left <= 0) {
			time_left = 0; state = TIMEUP; state_timer = 150;
#ifdef VD_STATS
			fprintf(stderr, "TIMEUP ph %d seg %d of %d\n", ph, (int)(pos_z / SEG_LEN), nseg);
#endif
		}
	}
	sound_tick(offroad);
}

/* print centred: the font advances about 1.1 x its size */
static void ctext(int y, const char *s, int size, int bright)
{
	v_printString(-(int)(strlen(s) * size * 11 / 20), y, (char *)s, size, bright);
}

static void hud(void)
{
	char t[48];
	int fork = state == RACE && ph < 4 && pos_z > (fork_start - FORK_WARN) * SEG_LEN &&
	           pos_z < (fork_start + FORK_LEN) * SEG_LEN;   /* not once the split is behind you */
	snprintf(t, sizeof t, "TIME %02d", (int)(time_left + 0.99f));
	v_printString(-110, 119, t, 5, 80);	/* size 5 glyphs are 7 units tall: 119 keeps the tops on screen (127) */
	snprintf(t, sizeof t, "%3d KM/H", (int)(speed / MAX_SPEED * 290));
	v_printString(25, 119, t, 5, 80);
	/* text is the expensive part of a frame: the phase line gives way to
	   the fork sign and to the banners, which carry the same information */
	if (fork) {
		const char *l = theme_name[lat_theme[ph][next_pos(ph, cur_pos, 0)]];
		const char *r = theme_name[lat_theme[ph][next_pos(ph, cur_pos, 1)]];
		snprintf(t, sizeof t, "< %s", l);
		v_printString(-118, 106, t, 4, 90);
		snprintf(t, sizeof t, "%s >", r);
		v_printString(118 - (int)(strlen(t) * 44 / 10), 106, t, 4, 90);
	} else if (state != TIMEUP && state != GOAL && banner_timer <= 0 && banner2_timer <= 0) {
		snprintf(t, sizeof t, "PHASE %d/4  %s", ph, theme_name[sky_w < 0.5f ? prev_theme : ph_theme]);
		ctext(106, t, 4, 60);
	}
	if (state == ATTRACT) ctext(60, "PRESS BUTTON 1", 6, 90);
	if (state == COUNTDOWN) {
		int n = state_timer / 50 + 1;
		snprintf(t, sizeof t, "%d", n);
		v_printString(-6, 60, t, 12, 110);
	}
	if (state == TIMEUP) ctext(60, "TIME UP", 7, 100);
	if (state == GOAL) {
		int s = race_frames / 50;
		snprintf(t, sizeof t, "TOTAL TIME %dM %02d.%dS", s / 60, s % 60, (race_frames % 50) / 5);
		ctext(62, t, 5, 100);
	}
	if (banner_timer > 0) {
		banner_timer--;
		ctext(state == GOAL ? 84 : 78, banner, state == GOAL ? 9 : 6, 100);
	}
	if (banner2_timer > 0) {
		banner2_timer--;
		ctext(64, banner2, 6, 100);
	}
}

#ifdef VD_START_PHASE
/* debug: jump straight into a phase/position at a given segment, at speed */
static void debug_jump(int p, int pos, int at)
{
	static const unsigned char lat_n[4] = { 1, 2, 3, 3 };
	ph = p; cur_pos = pos; ph_theme = lat_theme[p - 1][pos]; prev_theme = -1;
	for (int q = 0; p > 1 && q < lat_n[p - 2]; q++)
		for (int d = 0; d < 2; d++)
			if (prev_theme < 0 && next_pos(p - 1, q, d) == pos) prev_theme = lat_theme[p - 2][q];
	build_phase(ph_theme, prev_theme, ph);
	pos_z = at * SEG_LEN; speed = MAX_SPEED * 0.95f; time_left = 99; state = RACE;
#ifdef VD_START_TIME
	time_left = VD_START_TIME;                       /* (a believable clock for screenshots) */
#endif
	place_cars(pos_z + SEG_LEN * 100);
}
#endif

#ifdef VD_AUTO
/* debug driver: full gas, hold the centre (or a fork branch), dodge traffic */
#ifndef VD_AUTO_DIR
#define VD_AUTO_DIR 0
#endif
static void bot(void)
{
	static int wait;
	if (state != RACE) {                             /* start the next race (a press every second) */
		if (++wait % 50 == 0) currentButtonState |= 0x01;
		return;
	}
	int base = (int)(pos_z / SEG_LEN);
	const seg_t *cs = get_seg(base);
	float tx = (VD_AUTO_DIR ? 1.0f : -1.0f) * get_seg(base + (cs->fork > FORK_DMAX ? 0 : 4))->fork;   /* (riding the bend apart: here) */
	if (ph < 4 && base > fork_start - 150 && fabsf(tx) < 0.7f) tx = (VD_AUTO_DIR ? 0.7f : -0.7f);   /* pre-position */
	for (int i = 0; i < N_CARS; i++) {
		float rel = cars[i].z - pos_z;
		if (rel > 0 && rel < SEG_LEN * 26 && fabsf(cars[i].x - tx) < 0.9f) {
			tx = cars[i].x > 0 ? cars[i].x - 0.95f : cars[i].x + 0.95f;
			break;
		}
	}
	float st = (tx - player_x) * 2.5f + cs->curve * 0.27f;
	if (st > 1) st = 1;
	if (st < -1) st = -1;
	currentJoy1X = (int)(st * 127);
	currentButtonState |= 0x01;
}
#endif

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;
	vectrexinit(1);
	v_setName("vecdrive");
	v_init();
	v_setRefresh(50);
#ifdef VD_UVMC2
	v_setAnalog(1);                                  /* the cartridge reads the stick as on/off unless asked */
#endif
	scan_music();
	if (n_tracks) { music_mode = 2; load_track(0); }   /* a soundtrack, if there is one, is the default */
	reset_race();
	state = ATTRACT;
#ifdef VD_START_PHASE
#ifndef VD_START_POS
#define VD_START_POS 0
#endif
#ifndef VD_START_SEG
#define VD_START_SEG 0
#endif
	debug_jump(VD_START_PHASE, VD_START_POS, VD_START_SEG);
#endif

	for (;;) {
		v_WaitRecal();
#if defined(VD_FPS) && !defined(VD_UVMC2)
		{   /* debug (frame rate on the MiSTer): frames per second over every 250 frames, to stderr */
			static struct timeval t0;
			static int nf;
			struct timeval t;
			gettimeofday(&t, NULL);
			if (!nf) t0 = t;
			if (++nf == 251) {
				double s = (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_usec - t0.tv_usec) / 1e6;
				fprintf(stderr, "FPS %.2f (250 frames, %s, seg %d)\n", 250 / s, theme_name[ph_theme], (int)(pos_z / SEG_LEN));
				nf = 0;
			}
		}
#endif
		v_readButtons();
		v_readJoystick1Analog();
#ifdef VD_AUTO
		bot();
#endif
		int pressed = currentButtonState & ~btn_prev;
		btn_prev = currentButtonState;

		if (pressed & 0x04) {                        /* button 3: engine / music */
			music_mode = (music_mode + 1) % (2 + n_tracks);   /* ENGINE, SYNTH LOOP, each dump */
			if (music_mode >= 2) load_track(music_mode - 2);
			show_banner(music_mode >= 2 ? track[music_mode - 2].name : music_mode ? "MUSIC" : "ENGINE", 60);
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
		if (state == GOAL && --state_timer <= 0) { reset_race(); state = ATTRACT; banner_timer = 0; }
		update();

		draw_road();                                 /* first: it fills in the outline that hides the sky */
		draw_sky();
		draw_player(currentJoy1X / 127.0f);
		hud();
#ifdef VD_POPS
		for (int i = 0; i < N_CARS; i++) {           /* debug: log car visibility to find crest pop-ups */
			float rel = cars[i].z - pos_z;
			int n = (int)(rel / SEG_LEN);
			if (rel >= 0 && n < DRAW_DIST) fprintf(stderr, "C %d %d %d %d\n", race_frames, i, n, proj[n].vis);
		}
#endif
#ifdef VD_STATS
		fprintf(stderr, "L %d\n", nlines);
		nlines = 0;
#endif
	}
}
