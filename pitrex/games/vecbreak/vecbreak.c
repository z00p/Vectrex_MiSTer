/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 John (z00p) - PiTrex on MiSTer
 */
/*
 * VecBreak - a small Breakout for the PiTrex library.
 * Written for the PiTrex-on-MiSTer bring-up (2026): it exercises the analog
 * joystick (comparator read-back), the buttons (PSG port A), sound (PSG
 * writes), vector drawing and text, all at a steady 50 Hz.
 *
 * Controls: joystick 1 left/right moves the bat; button 1 serves / restarts.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pitrex/pitrexio-gpio.h>
#include <vectrex/vectrexInterface.h>

#define FIELD_L   (-15000)
#define FIELD_R   ( 15000)
#define FIELD_T   ( 14500)
#define FIELD_B   (-17500)

#define ROWS        5
#define COLS        8
#define BRICK_W     ((FIELD_R - FIELD_L) / COLS)
#define BRICK_TOP   11500
#define BRICK_STEP  1300
#define BRICK_HALF  350

#define BAT_Y       (-15000)
#define BAT_HALF_W  2400
#define BAT_HALF_H  300
#define BALL_R      350

enum { SERVE, PLAY, OVER, CLEARED };

static unsigned char bricks[ROWS][COLS];
static int bricks_left;
static int bat_x;
static int ball_x, ball_y, ball_vx, ball_vy;
static int score, lives, level, state, state_timer;
static int sound_frames;

/* Intro jingle: an original rising fanfare (tone periods = 93750 / Hz).
   {period, frames}; period 0 = rest. Plays on channel A, one step per frame. */
static const unsigned short jingle[][2] = {
	{239, 4}, {179, 4}, {142, 4}, {120, 4}, { 90, 8},   /* G4 C5 E5 G5 C6 */
	{ 95, 4}, {120, 4}, {107, 4}, {134, 4}, { 80, 8},   /* B5 G5 A5 F5 D6 */
	{  0, 2}, { 90, 18},                                /* rest, C6 */
};
static int jingle_step = -1, jingle_left;

static void jingle_start(void) { jingle_step = 0; jingle_left = 0; }

static void beep(int period, int frames)
{
	if (jingle_step >= 0) return;
	v_writePSG(0, period & 0xFF);
	v_writePSG(1, (period >> 8) & 0x0F);
	v_writePSG(7, 0x3E);                 /* tone A only; IOA stays an input */
	v_writePSG(8, 0x0D);
	sound_frames = frames;
}

static void sound_tick(void)
{
	if (jingle_step >= 0) {
		if (jingle_left == 0) {
			if (jingle_step >= (int)(sizeof jingle / sizeof jingle[0])) {
				jingle_step = -1;
				v_writePSG(8, 0x00);
				return;
			}
			int period = jingle[jingle_step][0];
			jingle_left = jingle[jingle_step][1];
			jingle_step++;
			if (period) {
				v_writePSG(0, period & 0xFF);
				v_writePSG(1, (period >> 8) & 0x0F);
				v_writePSG(7, 0x3E);
				v_writePSG(8, 0x0D);
			} else {
				v_writePSG(8, 0x00);
			}
		}
		if (jingle_left == 1) v_writePSG(8, 0x08);   /* short release between notes */
		jingle_left--;
		return;
	}
	if (sound_frames > 0 && --sound_frames == 0)
		v_writePSG(8, 0x00);
}

static void new_wall(void)
{
	memset(bricks, 1, sizeof bricks);
	bricks_left = ROWS * COLS;
}

static void serve_position(void)
{
	ball_x = bat_x;
	ball_y = BAT_Y + BAT_HALF_H + BALL_R + 50;
	ball_vx = 0;
	ball_vy = 0;
}

static void new_game(void)
{
	score = 0;
	lives = 3;
	level = 1;
	bat_x = 0;
	new_wall();
	serve_position();
	state = SERVE;
	jingle_start();
}

static void box(int x0, int y0, int x1, int y1, int bright)
{
	v_directDraw32(x0, y0, x1, y0, bright);
	v_directDraw32(x1, y0, x1, y1, bright);
	v_directDraw32(x1, y1, x0, y1, bright);
	v_directDraw32(x0, y1, x0, y0, bright);
}

static void draw(void)
{
	char text[40];

	/* walls */
	v_directDraw32(FIELD_L, FIELD_B, FIELD_L, FIELD_T, 40);
	v_directDraw32(FIELD_L, FIELD_T, FIELD_R, FIELD_T, 40);
	v_directDraw32(FIELD_R, FIELD_T, FIELD_R, FIELD_B, 40);

	/* bricks: one bright stroke each, dimmer further down */
	for (int r = 0; r < ROWS; r++) {
		int y = BRICK_TOP - r * BRICK_STEP;
		for (int c = 0; c < COLS; c++)
			if (bricks[r][c]) {
				int x = FIELD_L + c * BRICK_W;
				v_directDraw32(x + 300, y, x + BRICK_W - 300, y, 110 - r * 12);
			}
	}

	box(bat_x - BAT_HALF_W, BAT_Y - BAT_HALF_H, bat_x + BAT_HALF_W, BAT_Y + BAT_HALF_H, 90);
	box(ball_x - BALL_R, ball_y - BALL_R, ball_x + BALL_R, ball_y + BALL_R, 127);

	snprintf(text, sizeof text, "SCORE %04d  LIVES %d", score, lives);
	v_printString(-60, 120, text, 5, 60);
	if (state == SERVE)   v_printString(-40, -20, "PRESS BUTTON 1", 6, 70);
	if (state == OVER)    v_printString(-30, 0, "GAME OVER", 7, 90);
	if (state == CLEARED) v_printString(-40, 0, "WALL CLEARED", 7, 90);
}

static void move_bat(void)
{
	bat_x += currentJoy1X * 4;
	if (bat_x < FIELD_L + BAT_HALF_W) bat_x = FIELD_L + BAT_HALF_W;
	if (bat_x > FIELD_R - BAT_HALF_W) bat_x = FIELD_R - BAT_HALF_W;
}

static void hit_bricks(void)
{
	for (int r = 0; r < ROWS; r++) {
		int y = BRICK_TOP - r * BRICK_STEP;
		if (ball_y + BALL_R < y - BRICK_HALF || ball_y - BALL_R > y + BRICK_HALF)
			continue;
		int c = (ball_x - FIELD_L) / BRICK_W;
		if (c < 0 || c >= COLS || !bricks[r][c])
			continue;
		bricks[r][c] = 0;
		bricks_left--;
		score += (ROWS - r) * 10;
		ball_vy = -ball_vy;
		beep(0x080 + r * 0x20, 3);
		return;                              /* one brick per frame */
	}
}

static void move_ball(void)
{
	ball_x += ball_vx;
	ball_y += ball_vy;

	if (ball_x - BALL_R < FIELD_L) { ball_x = FIELD_L + BALL_R; ball_vx = -ball_vx; beep(0x200, 2); }
	if (ball_x + BALL_R > FIELD_R) { ball_x = FIELD_R - BALL_R; ball_vx = -ball_vx; beep(0x200, 2); }
	if (ball_y + BALL_R > FIELD_T) { ball_y = FIELD_T - BALL_R; ball_vy = -ball_vy; beep(0x200, 2); }

	/* bat: bounce upwards, angle from where the ball hit */
	if (ball_vy < 0 && ball_y - BALL_R <= BAT_Y + BAT_HALF_H && ball_y > BAT_Y - BAT_HALF_H &&
	    ball_x >= bat_x - BAT_HALF_W - BALL_R && ball_x <= bat_x + BAT_HALF_W + BALL_R) {
		ball_y = BAT_Y + BAT_HALF_H + BALL_R;
		ball_vy = -ball_vy;
		ball_vx = (ball_x - bat_x) / 7;
		beep(0x140, 3);
	}

	hit_bricks();

	if (ball_y < FIELD_B) {                  /* missed */
		beep(0x600, 20);
		if (--lives <= 0) { state = OVER; state_timer = 100; }
		else              { serve_position(); state = SERVE; }
	}
	if (bricks_left == 0) { state = CLEARED; state_timer = 100; }
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;
	vectrexinit(1);
	v_setName("vecbreak");
	v_init();
	v_setRefresh(50);
	new_game();

	for (;;) {
		v_WaitRecal();
		v_readButtons();
		v_readJoystick1Analog();
		sound_tick();
		int fire = currentButtonState & 0x01;

		switch (state) {
		case SERVE:
			move_bat();
			serve_position();
			if (fire) {
				state = PLAY;
				ball_vx = 200;
				ball_vy = 450 + level * 60;
			}
			break;
		case PLAY:
			move_bat();
			move_ball();
			break;
		case OVER:
			if (state_timer > 0) state_timer--;
			else if (fire) new_game();
			break;
		case CLEARED:
			if (--state_timer <= 0) {
				level++;
				new_wall();
				serve_position();
				state = SERVE;
				jingle_start();
			}
			break;
		}
		draw();
	}
}
