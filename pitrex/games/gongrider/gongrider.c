/*
 * GONG RIDER - a PiTrex game, heavily inspired by Master of the Lamps (Activision, 1985)
 * Copyright (c) 2026 Roger Boesch. MIT licence, see LICENSE.
 * The same game as GONG RIDER for the Vectrex cartridge (../src/gongrider.c),
 * written for the PiTrex: a Raspberry Pi Zero in the cartridge slot that
 * draws on the Vectrex screen through Malban's vectrexInterface library.
 *
 *   Ride   the carpet flies down a tunnel of rings. Every second ring carries
 *          a lit gate; steer the carpet (joystick 1, all four directions) so it
 *          is inside the gate when the ring reaches you. Each gate plays the
 *          next note of a tune. A missed gate costs a lamp.
 *   Gongs  after eight gates the carpet reaches the gong chamber: four gongs
 *          play a sequence, the player repeats it with the joystick (or buttons
 *          1-4). Right = bonus and the next, faster stage. Wrong = a lamp, and
 *          the sequence is played again.
 *
 * Three lamps; none left = game over, then back to the title.
 *
 * Drawing: every line is one v_directDraw32 call with absolute screen
 * coordinates, 32 bits each, (0, 0) in the middle of the screen. The library
 * works out the beam's moves and scales by itself. Depth is a real division
 * per ring and frame, in floating point.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vectrex/vectrexInterface.h>

/* ------------------------------------------------------------------ */
/* Tuning                                                              */
/* ------------------------------------------------------------------ */

/* The game keeps the Vectrex version's units: a ring has a half size of
 * 110, the gates sit within +-60. One such unit is 128 PiTrex units, the
 * same factor the library's own text call uses for its 8-bit positions. */
#define UNIT                128

#define INT_BRIGHT          0x7F
#define INT_NORMAL          0x5F
#define INT_DIM             0x38
#define INT_FAINT           0x24

/* Tunnel. Depth z runs from Z_FAR down to 0. Something at depth z is drawn
 * PERSP_D / (z + PERSP_D) times its size: 1.0 at z = 0, 0.93 at the
 * carpet's plane, 0.09 at the far end.                                    */
#define RING_COUNT          6
#define RING_SPACING        40.0f
#define Z_FAR               (RING_COUNT * RING_SPACING)
#define PERSP_D             25.0f
#define Z_CARPET            2.0f

#define RING_HALF           110.0f
#define GATE_RANGE          60      /* gate centre within +-60 ring units  */
#define GATE_WALK           40      /* max jump from one gate to the next  */
#define GATE_HALF           34.0f   /* drawn half size of the gate         */
#define GATE_TOLERANCE      30      /* carpet centre this close = through  */

#define CARPET_RANGE        64
#define CARPET_STEP         3
#define STICK_THRESHOLD     30      /* analog stick: -128..127             */

#define GATES_PER_STAGE     8
#define LAMPS_START         3

#define GONG_COUNT          4
#define GONG_SEQ_MAX        8
#define GONG_HALF           22.5f   /* a gong is a small octagon           */
#define GONG_HALF_LIT       29.5f
#define GONG_SHOW_FRAMES    30
#define GONG_GAP_FRAMES     12
#define GONG_FLASH_FRAMES   14

#define STATE_TITLE         0
#define STATE_RIDE          1
#define STATE_GONG_INTRO    2
#define STATE_GONG_SHOW     3
#define STATE_GONG_INPUT    4
#define STATE_GONG_RESULT   5
#define STATE_GAME_OVER     6

#define TITLE_FRAMES        250     /* starts by itself after 5 s          */
#define INTRO_FRAMES        90
#define RESULT_FRAMES       90
#define GAME_OVER_FRAMES    200

#define TEXT_BIG            10      /* v_printString sizes                 */
#define TEXT_SMALL          7

/* AY-3-8910 registers */
#define AY_TONE_A_LO        0
#define AY_TONE_A_HI        1
#define AY_TONE_B_LO        2
#define AY_TONE_B_HI        3
#define AY_NOISE            6
#define AY_MIXER            7
#define AY_AMP_A            8
#define AY_AMP_B            9
#define AY_AMP_C            10

/* Tone A and B on, noise on C only; port A stays an input (buttons). */
#define AY_MIXER_GAME       0x1C

/* ------------------------------------------------------------------ */
/* Shapes                                                              */
/* ------------------------------------------------------------------ */

/* The octagon's eight corners for a half size of 1. The rings and the gongs
 * use it, at their own centre and size.                                   */
static const float octagon[8][2] = {
    { -0.545f,  1.0f   }, {  0.545f,  1.0f   }, {  1.0f,    0.545f },
    {  1.0f,   -0.545f }, {  0.545f, -1.0f   }, { -0.545f, -1.0f   },
    { -1.0f,   -0.545f }, { -1.0f,    0.545f }
};

/* ------------------------------------------------------------------ */
/* State - not static where the test reads it                          */
/* ------------------------------------------------------------------ */

int      game_state;
int      state_timer;

float    ring_z[RING_COUNT];
int      ring_gate[RING_COUNT];     /* 1 = this ring carries a gate        */
int      ring_gx[RING_COUNT];
int      ring_gy[RING_COUNT];
int      ring_judged[RING_COUNT];   /* gate already passed or missed       */
static int next_has_gate;
static int last_gx, last_gy;

int      carpet_x, carpet_y;
static unsigned carpet_wave;

int      target_x, target_y;        /* nearest gate still ahead - test     */
int      target_valid;

int      lamps;
int      stage;
int      score;
int      gates_this_stage;
int      gates_passed;              /* totals since start - test           */
int      gates_missed;
int      gong_rounds_won;
int      gong_rounds_lost;
static float speed;                 /* depth units per frame               */

static int crash_timer;             /* carpet flicker after a miss         */

int      gong_seq[GONG_SEQ_MAX];
int      gong_len;
int      gong_pos;                  /* step being shown or expected        */
int      gong_expect = -1;          /* gong the player must hit, -1 = none */
static int gong_lit = -1;
static int gong_timer;
static int gong_result;             /* 1 right, 0 wrong                    */
static int last_dir = -1;
static uint8_t last_buttons;

/* The measurement: lines drawn in the frame just finished. Text is drawn
 * by the library and not counted here.                                    */
int      lines_last_frame;
unsigned frame_counter;
static int lines_this_frame;

/* Sound: channel A = notes with a software decay, B = wind, C = noise. */
static uint8_t amp_a;
static uint8_t amp_c;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int iabs(int v)
{
    return v < 0 ? -v : v;
}

static int clampi(int v, int limit)
{
    if (v > limit)  return limit;
    if (v < -limit) return -limit;
    return v;
}

/* 32-bit xorshift, seeded from the clock when a game starts. */
static uint32_t rng_state = 2463534242u;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* A line from (x0, y0) to (x1, y1) in game units, 128 PiTrex units each. */
static void line(float x0, float y0, float x1, float y1, uint8_t bright)
{
    v_directDraw32((int32_t)(x0 * UNIT), (int32_t)(y0 * UNIT),
                   (int32_t)(x1 * UNIT), (int32_t)(y1 * UNIT), bright);
    lines_this_frame++;
}

/* An octagon with its centre at (cx, cy) and the given half size. */
static void octagon_draw(float cx, float cy, float half, uint8_t bright)
{
    int i;

    for (i = 0; i < 8; i++)
    {
        const float *a = octagon[i];
        const float *b = octagon[(i + 1) & 7];

        line(cx + a[0] * half, cy + a[1] * half,
             cx + b[0] * half, cy + b[1] * half, bright);
    }
}

/* A diamond with its centre at (cx, cy). */
static void diamond_draw(float cx, float cy, float half, uint8_t bright)
{
    line(cx,        cy + half, cx + half, cy,        bright);
    line(cx + half, cy,        cx,        cy - half, bright);
    line(cx,        cy - half, cx - half, cy,        bright);
    line(cx - half, cy,        cx,        cy + half, bright);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

/* The library prints plain C strings; x and y are 8-bit positions as on
 * the Vectrex, 128 PiTrex units each, (x, y) is the bottom left corner.  */
static void text(int y, int x, const char *s, uint8_t size, uint8_t bright)
{
    v_printString((int8_t)x, (int8_t)y, (char *)s, size, bright);
}

/* Every letter of the library's font advances 6 font units of 22.5 * size
 * PiTrex units: 135 * size, or about 1.05 * size in 8-bit positions.    */
static void text_centered(int y, const char *s, uint8_t size, uint8_t bright)
{
    int width = (int)(strlen(s) * size * 135 / UNIT);

    text(y, -width / 2, s, size, bright);
}

/* Score and lamps. The library's font has no star, so a lamp is a small
 * diamond drawn as lines, one per lamp left.                              */
static void hud(void)
{
    char buf[12];
    int  i;

    snprintf(buf, sizeof buf, "%d", score);
    text(116, -60, buf, TEXT_SMALL, INT_NORMAL);

    for (i = 0; i < lamps && i < 5; i++)
        diamond_draw(30.0f + i * 14.0f, 120.0f, 4.0f, INT_NORMAL);
}

/* ------------------------------------------------------------------ */
/* Sound                                                               */
/* ------------------------------------------------------------------ */

/* The AY-3-8910 on the Vectrex board, clocked at 1.5 MHz as on the
 * cartridge: the periods are the same. 1.5 MHz / (16 * f):
 * C5 D5 E5 F5 G5 A5 B5 C6.                                                */
static const uint8_t note_period[8] = { 179, 159, 142, 134, 120, 107, 95, 90 };

/* The gongs are C, E, G and the high C. */
static const uint8_t gong_period[GONG_COUNT] = { 179, 142, 120, 90 };

/* The tune the gates play, one note per gate. */
static const int gate_tune[GATES_PER_STAGE] = { 0, 2, 4, 2, 5, 4, 2, 7 };

/* Writes go into the library's buffer; v_doSound() sends the registers
 * that changed to the chip, once per frame.                               */
static void psg(uint8_t reg, uint8_t value)
{
    v_writePSG_double_buffered(reg, value);
}

static void sound_init(void)
{
    v_noSound();
    psg(AY_MIXER, AY_MIXER_GAME);
    psg(AY_NOISE, 18);
    amp_a = 0;
    amp_c = 0;
}

static void note(uint8_t period, uint8_t amplitude)
{
    psg(AY_TONE_A_LO, period);
    psg(AY_TONE_A_HI, 0);
    amp_a = amplitude;
}

static void crash_noise(void)
{
    amp_c = 15;
}

static void wind(int on)
{
    /* Low hum whose pitch rises with the stage's speed. */
    int period = 900 - (int)(speed * 4.0f) * 60;

    psg(AY_TONE_B_LO, (uint8_t)(period & 0xFF));
    psg(AY_TONE_B_HI, (uint8_t)(period >> 8));
    psg(AY_AMP_B, on ? 5 : 0);
}

/* Once per frame: the decays, then everything to the chip. */
static void sound_update(void)
{
    psg(AY_AMP_A, amp_a);
    psg(AY_AMP_C, amp_c);

    if (amp_a > 0 && (frame_counter & 1))
        amp_a--;
    if (amp_c > 0)
        amp_c--;

    v_doSound();
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

static uint8_t buttons;             /* bit 0..3 = buttons 1..4             */

static void input_read(void)
{
    buttons = v_readButtons() & 0x0F;
    v_readJoystick1Analog();
}

static int stick_left(void)  { return currentJoy1X < -STICK_THRESHOLD; }
static int stick_right(void) { return currentJoy1X >  STICK_THRESHOLD; }
static int stick_down(void)  { return currentJoy1Y < -STICK_THRESHOLD; }
static int stick_up(void)    { return currentJoy1Y >  STICK_THRESHOLD; }

/* ------------------------------------------------------------------ */
/* Tunnel                                                              */
/* ------------------------------------------------------------------ */

/* How large something at depth z is drawn: 1.0 right in front. */
static float depth_factor(float z)
{
    if (z < 0.0f)
        z = 0.0f;
    return PERSP_D / (z + PERSP_D);
}

static void ring_arm(int i)
{
    ring_judged[i] = 0;
    ring_gate[i]   = next_has_gate;

    if (next_has_gate)
    {
        /* A walk from the last gate, pulled back toward the centre so the
         * gates do not pile up against the clamp at the edge.            */
        int step_x = (int)(rng() % (2 * GATE_WALK - 1)) - (GATE_WALK - 1);
        int step_y = (int)(rng() % (2 * GATE_WALK - 1)) - (GATE_WALK - 1);

        last_gx = clampi(last_gx * 3 / 4 + step_x, GATE_RANGE);
        last_gy = clampi(last_gy * 3 / 4 + step_y, GATE_RANGE);
        ring_gx[i] = last_gx;
        ring_gy[i] = last_gy;
    }

    next_has_gate = !next_has_gate;
}

static void tunnel_init(void)
{
    int i;

    next_has_gate = 0;
    last_gx = 0;
    last_gy = 0;

    /* Nearest ring first, so the gates arrive in the order they were armed.
     * The first two rings carry no gate: a moment to find the carpet.     */
    for (i = 0; i < RING_COUNT; i++)
    {
        ring_z[i] = Z_CARPET + 40.0f + i * RING_SPACING;
        ring_arm(i);
        if (i < 2)
            ring_gate[i] = 0;
    }

    carpet_x = 0;
    carpet_y = -30;
    gates_this_stage = 0;
    crash_timer = 0;
}

static void gate_judge(int i)
{
    int dx = iabs(carpet_x - ring_gx[i]);
    int dy = iabs(carpet_y - ring_gy[i]);

    ring_judged[i] = 1;

    if (!ring_gate[i])
        return;

    if (dx <= GATE_TOLERANCE && dy <= GATE_TOLERANCE)
    {
        gates_passed++;
        score += 10;
        note(note_period[gate_tune[gates_this_stage]], 14);
    }
    else
    {
        gates_missed++;
        lamps--;
        crash_timer = 40;
        crash_noise();
    }

    gates_this_stage++;
}

static void tunnel_update(void)
{
    int   i;
    float best = Z_FAR + 1.0f;

    target_valid = 0;

    for (i = 0; i < RING_COUNT; i++)
    {
        ring_z[i] -= speed;

        if (!ring_judged[i] && ring_z[i] <= Z_CARPET)
            gate_judge(i);

        if (ring_z[i] <= 0.0f)
        {
            ring_z[i] += Z_FAR;
            ring_arm(i);
        }

        if (ring_gate[i] && !ring_judged[i] && ring_z[i] < best)
        {
            best     = ring_z[i];
            target_x = ring_gx[i];
            target_y = ring_gy[i];
            target_valid = 1;
        }
    }
}

static void ring_draw(int i)
{
    float k = depth_factor(ring_z[i]);

    /* Far rings fade out, near ones are solid: the brightness follows the
     * depth, from 0x29 at the far end to 0x5F in front.                   */
    uint8_t bright = (uint8_t)(INT_FAINT + (INT_NORMAL - INT_FAINT) * k);

    octagon_draw(0.0f, 0.0f, RING_HALF * k, bright);

    if (ring_gate[i] && !ring_judged[i])
        diamond_draw(ring_gx[i] * k, ring_gy[i] * k, GATE_HALF * k, INT_BRIGHT);
}

/* Four rays from the corners of the screen toward the vanishing point. */
static void rays_draw(void)
{
    line(-120,  120, -40,  40, INT_FAINT);
    line( 120,  120,  40,  40, INT_FAINT);
    line( 120, -120,  40, -40, INT_FAINT);
    line(-120, -120, -40, -40, INT_FAINT);
}

static void carpet_draw(void)
{
    float k    = depth_factor(Z_CARPET);
    float x    = carpet_x * k;
    float y    = carpet_y * k;
    float wave = (carpet_wave & 8) ? 3.0f : -3.0f;

    /* A miss makes the carpet flicker for a moment. */
    if (crash_timer > 0 && (crash_timer & 2))
        return;

    /* The rug seen from behind: narrow back edge, wide front edge with a
     * wave running through it. Its place follows the gates' plane, its
     * size does not, so it stays large enough to see.                     */
    line(x - 22, y + 10,        x + 22, y + 10,        INT_BRIGHT);
    line(x + 22, y + 10,        x + 34, y - 16,        INT_BRIGHT);
    line(x + 34, y - 16,        x,      y - 16 + wave, INT_BRIGHT);
    line(x,      y - 16 + wave, x - 34, y - 16,        INT_BRIGHT);
    line(x - 34, y - 16,        x - 22, y + 10,        INT_BRIGHT);

    /* The rider: a pointed hat on the middle of the rug. */
    line(x - 8, y + 10, x,     y + 34, INT_BRIGHT);
    line(x,     y + 34, x + 8, y + 10, INT_BRIGHT);
}

static void carpet_update(void)
{
    if (stick_left()  && carpet_x > -CARPET_RANGE) carpet_x -= CARPET_STEP;
    if (stick_right() && carpet_x <  CARPET_RANGE) carpet_x += CARPET_STEP;
    if (stick_down()  && carpet_y > -CARPET_RANGE) carpet_y -= CARPET_STEP;
    if (stick_up()    && carpet_y <  CARPET_RANGE) carpet_y += CARPET_STEP;

    carpet_wave++;
}

/* Draw far to near, so the near ring is drawn last. The rings stay in
 * cyclic order (one recycles to the back as the nearest leaves), so the
 * farthest is always the one after the nearest.                         */
static void tunnel_draw(void)
{
    int nearest = 0;
    int i, k;

    for (i = 1; i < RING_COUNT; i++)
    {
        if (ring_z[i] < ring_z[nearest])
            nearest = i;
    }

    k = nearest;

    for (i = 0; i < RING_COUNT; i++)
    {
        if (++k >= RING_COUNT)
            k = 0;
        ring_draw(k);
    }
}

/* ------------------------------------------------------------------ */
/* Gongs                                                               */
/* ------------------------------------------------------------------ */

/* Gong 0 up, 1 right, 2 down, 3 left - the joystick direction that hits it. */
static const int gong_at[GONG_COUNT][2] = {
    {   0,  70 }, {  70,   0 }, {   0, -70 }, { -70,   0 }
};

static void gong_new_sequence(void)
{
    int i;

    gong_len = 2 + stage;
    if (gong_len > GONG_SEQ_MAX)
        gong_len = GONG_SEQ_MAX;

    for (i = 0; i < gong_len; i++)
        gong_seq[i] = (int)(rng() & 3);
}

static void gong_start_show(void)
{
    game_state  = STATE_GONG_SHOW;
    gong_pos    = 0;
    gong_lit    = -1;
    gong_timer  = GONG_GAP_FRAMES * 2;
    gong_expect = -1;
}

static void gong_ring(int g)
{
    gong_lit = g;
    note(gong_period[g], 15);
}

static void gong_draw(int g)
{
    int   lit = (gong_lit == g);
    float x   = (float)gong_at[g][0];
    float y   = (float)gong_at[g][1];

    octagon_draw(x, y, lit ? GONG_HALF_LIT : GONG_HALF, lit ? INT_BRIGHT : INT_DIM);

    /* A struck gong gets a hammer mark across it. */
    if (lit)
        line(x - 12, y + 12, x + 12, y - 12, INT_BRIGHT);
}

static void gongs_draw(void)
{
    int g;

    for (g = 0; g < GONG_COUNT; g++)
        gong_draw(g);
}

/* Direction the stick is pushed as a gong number, or -1. Buttons 1-4 too. */
static int read_gong_input(void)
{
    if (buttons & 0x01) return 0;
    if (buttons & 0x02) return 1;
    if (buttons & 0x04) return 2;
    if (buttons & 0x08) return 3;

    if (stick_up())    return 0;
    if (stick_right()) return 1;
    if (stick_down())  return 2;
    if (stick_left())  return 3;

    return -1;
}

static void gong_show_update(void)
{
    if (--gong_timer > 0)
        return;

    if (gong_lit >= 0)
    {
        gong_lit   = -1;
        gong_timer = GONG_GAP_FRAMES;
        gong_pos++;
        return;
    }

    if (gong_pos >= gong_len)
    {
        game_state  = STATE_GONG_INPUT;
        gong_pos    = 0;
        gong_expect = gong_seq[0];
        last_dir    = -1;
        return;
    }

    gong_ring(gong_seq[gong_pos]);
    gong_timer = GONG_SHOW_FRAMES;
}

static void gong_input_update(void)
{
    int dir = read_gong_input();

    if (gong_timer > 0 && --gong_timer == 0)
        gong_lit = -1;

    /* Only a fresh push counts, holding the stick does not repeat. */
    if (dir >= 0 && dir != last_dir)
    {
        gong_ring(dir);
        gong_timer = GONG_FLASH_FRAMES;

        if (dir != gong_seq[gong_pos])
        {
            gong_result = 0;
            gong_rounds_lost++;
            lamps--;
            crash_noise();
            gong_expect = -1;
            game_state  = STATE_GONG_RESULT;
            state_timer = RESULT_FRAMES;
        }
        else if (++gong_pos >= gong_len)
        {
            gong_result = 1;
            gong_rounds_won++;
            score += 50 * stage;
            gong_expect = -1;
            game_state  = STATE_GONG_RESULT;
            state_timer = RESULT_FRAMES;
        }
        else
        {
            gong_expect = gong_seq[gong_pos];
        }
    }

    last_dir = dir;
}

/* ------------------------------------------------------------------ */
/* Game flow                                                           */
/* ------------------------------------------------------------------ */

static void start_stage(void)
{
    /* 1.25 depth units per frame in stage 1, a quarter more each stage. */
    speed = (4 + stage) / 4.0f;
    if (speed > 3.0f)
        speed = 3.0f;

    tunnel_init();
    game_state = STATE_RIDE;
    wind(1);
}

static void new_game(void)
{
    /* The microsecond the game starts differs every time: two games do not
     * fly the same gates. Never zero, or xorshift stalls.                 */
    rng_state ^= v_micros();
    if (rng_state == 0)
        rng_state = 2463534242u;

    gong_lit = -1;
    lamps    = LAMPS_START;
    stage    = 1;
    score    = 0;
    start_stage();
}

static void to_title(void)
{
    game_state  = STATE_TITLE;
    state_timer = TITLE_FRAMES;
    gong_expect = -1;
    wind(0);
}

static void frame_title(void)
{
    uint8_t pressed = buttons & ~last_buttons;

    text_centered(20, "GONG RIDER", TEXT_BIG, INT_BRIGHT);
    text_centered(-120, "PRESS A BUTTON", TEXT_SMALL, INT_NORMAL);

    gongs_draw();

    if (pressed || --state_timer <= 0)
        new_game();
}

static void frame_ride(void)
{
    rays_draw();
    tunnel_draw();
    carpet_draw();
    hud();

    carpet_update();
    tunnel_update();

    if (crash_timer > 0)
        crash_timer--;

    if (lamps <= 0)
    {
        game_state  = STATE_GAME_OVER;
        state_timer = GAME_OVER_FRAMES;
        wind(0);
        return;
    }

    /* All gates of the stage are behind: on to the gong chamber. */
    if (gates_this_stage >= GATES_PER_STAGE)
    {
        wind(0);
        gong_new_sequence();
        gong_lit    = -1;
        game_state  = STATE_GONG_INTRO;
        state_timer = INTRO_FRAMES;
    }
}

static void frame_gong(void)
{
    gongs_draw();
    hud();

    switch (game_state)
    {
        case STATE_GONG_INTRO:
            text_centered(-120, "THE GONGS", TEXT_SMALL, INT_BRIGHT);
            if (--state_timer <= 0)
                gong_start_show();
            break;

        case STATE_GONG_SHOW:
            text_centered(-120, "LISTEN", TEXT_SMALL, INT_NORMAL);
            gong_show_update();
            break;

        case STATE_GONG_INPUT:
            text_centered(-120, "YOUR TURN", TEXT_SMALL, INT_NORMAL);
            gong_input_update();
            break;

        case STATE_GONG_RESULT:
            text_centered(-120, gong_result ? "WELL DONE" : "WRONG", TEXT_SMALL, INT_BRIGHT);

            if (gong_timer > 0 && --gong_timer == 0)
                gong_lit = -1;

            if (--state_timer <= 0)
            {
                gong_lit = -1;

                if (lamps <= 0)
                {
                    game_state  = STATE_GAME_OVER;
                    state_timer = GAME_OVER_FRAMES;
                }
                else if (gong_result)
                {
                    if (stage < 9)
                        stage++;
                    start_stage();
                }
                else
                {
                    gong_start_show();      /* same sequence again */
                }
            }
            break;
    }
}

static void frame_game_over(void)
{
    text_centered(20, "GAME OVER", TEXT_BIG, INT_BRIGHT);
    hud();

    if (--state_timer <= 0)
        to_title();
}

int main(void)
{
    vectrexinit(1);
    v_init();
    v_setRefresh(50);

    sound_init();
    lamps = LAMPS_START;
    stage = 1;
    to_title();

    while (1)
    {
        v_WaitRecal();
        input_read();

        lines_this_frame = 0;

        switch (game_state)
        {
            case STATE_TITLE:     frame_title();     break;
            case STATE_RIDE:      frame_ride();      break;
            case STATE_GAME_OVER: frame_game_over(); break;
            default:              frame_gong();      break;
        }

        sound_update();

        last_buttons     = buttons;
        lines_last_frame = lines_this_frame;
        frame_counter++;
    }

    return 0;
}
