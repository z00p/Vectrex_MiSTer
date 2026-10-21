# SPDX-License-Identifier: GPL-2.0-or-later
"""Gyrocks additions for PiTrex on MiSTer (GPL-2, like the original):
ship damage from rocks and enemies, 3 lives, explosions, sound, game over.
Applies to games/gyrocks/gyrocks.c (a copy of upstream). Run once."""
from pathlib import Path

p = Path(__file__).resolve().parent.parent / "games/gyrocks/gyrocks.c"
s = p.read_text(encoding="utf-8")
NL = chr(10)


def rep(a, b):
    global s
    assert s.count(a) == 1, a[:70]
    s = s.replace(a, b)


rep("*/" + NL + NL + "/*" + NL + "   Todo:",
    "*/" + NL + NL + "/* Modified 2026 for PiTrex on MiSTer (z00p): ship damage from rocks and" + NL +
    "   enemies, 3 lives with invulnerability after a hit, vector explosions, PSG sound" + NL +
    "   effects and a game-over/restart. Original by Carsten Wartmann, GPL-2. */" + NL + NL + "/*" + NL + "   Todo:")
rep("  int16_t vr;" + NL + "  int16_t vp;" + NL + "} enemy_t;",
    "  int16_t vr;" + NL + "  int16_t vp;" + NL + "  int16_t x;    // last drawn position, for ship collisions" + NL + "  int16_t y;" + NL + "} enemy_t;")
rep("unsigned int score;" + NL, "unsigned int score;" + NL + r"""
/* ---- PiTrex on MiSTer additions ---- */
int lives = 3, invuln = 0, game_over = 0, go_timer = 0;
#define MAX_EXPLO 8
typedef struct { int16_t x, y, age, big; } explo_t;
explo_t ex[MAX_EXPLO];
int boom_vol = 0, pew_frames = 0, pew_period = 0;

static void add_explosion(int x, int y, int big)
{
  for (int i = 0; i < MAX_EXPLO; i++)
    if (ex[i].age <= 0) { ex[i].x = x; ex[i].y = y; ex[i].age = big ? 40 : 24; ex[i].big = big; return; }
}

static void sfx_boom(int big) { boom_vol = big ? 30 : 18; }
static void sfx_pew(void) { pew_frames = 5; pew_period = 0x50; }

static void sound_tick(void)
{
  int mixer = 0x3F;                      /* bit 6 = 0: IOA stays an input */
  if (boom_vol > 0) mixer &= ~0x08;      /* noise on A */
  if (pew_frames > 0) mixer &= ~0x02;    /* tone B */
  v_writePSG(7, mixer);
  v_writePSG(6, 0x1F);
  v_writePSG(8, boom_vol > 0 ? (boom_vol + 1) / 2 : 0);
  if (boom_vol > 0) boom_vol--;
  if (pew_frames > 0) {
    v_writePSG(2, pew_period & 0xFF); v_writePSG(3, (pew_period >> 8) & 0x0F);
    v_writePSG(9, 0x0C);
    pew_period += 0x18; pew_frames--;
  } else v_writePSG(9, 0);
}
""")
# collisions: size-aware, return index+1 so the caller can remove the object
rep("  d = d * 10;" + NL + "  x0 = x - d;" + NL + "  y0 = y - d ;" + NL + "  x1 = x + d;" + NL + "  y1 = y + d ;" + NL + NL +
    "  draw_rect(x0, y0, x1, y1);" + NL + "  for (uint8_t i = 0 ; i < MAX_ROCK ; i++)" + NL + "  {" + NL + "    if (r[i].t >= 0)" + NL + "    {" + NL +
    "      if (r[i].x > x0 && r[i].x < x1 && r[i].y > y0 && r[i].y < y1)" + NL + "      {" + NL +
    "        //        r[i].t = -1;    //Kill rock also?" + NL + "        return 1; //Collision with Bullet",
    "  d = d * 10;" + NL + "  (void)x0; (void)y0; (void)x1; (void)y1;" + NL + "  for (uint8_t i = 0 ; i < MAX_ROCK ; i++)" + NL + "  {" + NL + "    if (r[i].t >= 0)" + NL + "    {" + NL +
    "      int reach = d + r[i].d * 6;           /* ship box + rock size */" + NL +
    "      if (abs(r[i].x - x) < reach && abs(r[i].y - y) < reach)" + NL + "      {" + NL +
    "        return i + 1;")
rep("int collision_bullet(int x, int y, int d)",
    "int collision_enemy(int x, int y, int d)" + NL + "{" + NL + "  d = d * 10;" + NL +
    "  for (uint8_t i = 0 ; i < MAX_ENEMY ; i++)" + NL +
    "    if (e[i].t >= 0 && e[i].r > 1500) {     /* only once it is near the front */" + NL +
    "      int reach = d + (e[i].r / 512) * 6;" + NL +
    "      if (abs(e[i].x - x) < reach && abs(e[i].y - y) < reach) return i + 1;" + NL + "    }" + NL +
    "  return 0;" + NL + "}" + NL + NL + "int collision_bullet(int x, int y, int d)")
# ship: damage, lives, invulnerability, blink; no firing after game over
rep("  if (collision_rock(ship->x, ship->y, d))" + NL + "  {" + NL + "    score = 0;" + NL + "  }",
    "  if (!game_over && invuln == 0)" + NL + "  {" + NL +
    "    int hr = collision_rock(ship->x, ship->y, d), he = collision_enemy(ship->x, ship->y, d);" + NL +
    "    if (hr || he)" + NL + "    {" + NL +
    "      if (hr) { r[hr - 1].t = -1; r[hr - 1].r = 0; }" + NL +
    "      if (he) { e[he - 1].t = -1; e[he - 1].r = 0; }" + NL +
    "      add_explosion(ship->x, ship->y, 1); sfx_boom(1);" + NL +
    "      invuln = 120;" + NL +
    "      if (--lives <= 0) { game_over = 1; go_timer = 100; }" + NL + "    }" + NL + "  }" + NL +
    "  if (invuln > 0) invuln--;")
rep("  if ((((currentButtonState&0x08) == (0x08))) && v_millis() > (ship->firedelay + FIREDELAY))" + NL + "  {",
    "  if (!game_over && (((currentButtonState&0x08) == (0x08))) && v_millis() > (ship->firedelay + FIREDELAY))" + NL + "  {" + NL + "    sfx_pew();")
rep("  draw_object(3, ship->x, ship->y, d, rot);               // Ship",
    "  if (game_over || (invuln > 0 && ((invuln / 4) & 1))) return;   /* gone, or blinking */" + NL +
    "  draw_object(3, ship->x, ship->y, d, rot);               // Ship")
# explosions for shot rocks and enemies; enemies remember their position
rep("        rr[i].t = -1;" + NL + "        rr[i].r = 0;" + NL + "        score += 10;",
    "        add_explosion(x, y, 0); sfx_boom(0);" + NL + "        rr[i].t = -1;" + NL + "        rr[i].r = 0;" + NL + "        score += 10;")
rep("        rr[i].t = -1;" + NL + "        rr[i].r = 0;" + NL + "        score += 100;",
    "        add_explosion(x, y, 0); sfx_boom(0);" + NL + "        rr[i].t = -1;" + NL + "        rr[i].r = 0;" + NL + "        score += 100;")
i = s.index("static void  update_enemies(")
j = s.index("      if (collision_bullet(x, y, rr[i].r / 512))", i)
s = s[:j] + "      rr[i].x = x; rr[i].y = y;" + NL + s[j:]
# spawning stops after game over; explosions + sound each frame
rep("  if (rand() % 500 == 1) add_rock(r);", "  if (!game_over && rand() % 500 == 1) add_rock(r);")
rep("  if (rand() % 500 == 1) add_enemy(e);", "  if (!game_over && rand() % 500 == 1) add_enemy(e);")
rep("  v_setBrightness(80);" + NL + "  draw_field();" + NL + "}",
    "  v_setBrightness(80);" + NL + "  draw_field();" + NL + NL +
    "  v_setBrightness(110);                                   /* explosions: radial bursts */" + NL +
    "  for (int i = 0; i < MAX_EXPLO; i++)" + NL + "    if (ex[i].age > 0)" + NL + "    {" + NL +
    "      int life = ex[i].big ? 40 : 24, t = life - ex[i].age;" + NL +
    "      int r0 = t * (ex[i].big ? 14 : 9), len = (ex[i].big ? 60 : 40) - t;" + NL +
    "      if (len < 8) len = 8;" + NL +
    "      for (int k = 0; k < 8; k++)" + NL + "      {" + NL +
    "        int a = k * 45 + (ex[i].big ? 0 : 22);" + NL +
    "        int ux = icos(a), uy = isin(a);                     /* about -255..255 */" + NL +
    "        moveto(ex[i].x + ux * r0 / 255, ex[i].y + uy * r0 / 255);" + NL +
    "        linetoRelative(ux * len / 255 / 16 + 1, uy * len / 255 / 16 + 1);" + NL + "      }" + NL +
    "      ex[i].age--;" + NL + "    }" + NL + "  sound_tick();" + NL + "}")
# HUD + restart
rep("    v_printStringRaster (70, 100, FPSBuf, 5*8, -7, '\\0');",
    "    v_printStringRaster (70, 100, FPSBuf, 5*8, -7, '\\0');" + NL +
    "    { char lb[16]; sprintf(lb, \"Ships: %d \", lives > 0 ? lives : 0); v_printStringRaster(-115, 86, lb, 5*8, -7, '\\0'); }" + NL +
    "    if (game_over)" + NL + "    {" + NL +
    "      v_printStringRaster(-40, 10, \"GAME OVER\", 6*8, -9, '\\0');" + NL +
    "      if (go_timer > 0) go_timer--;" + NL +
    "      else" + NL + "      {" + NL +
    "        v_printStringRaster(-60, -10, \"BUTTON 1: RESTART\", 5*8, -7, '\\0');" + NL +
    "        if (currentButtonState & 0x01)" + NL + "        {" + NL +
    "          for (int k = 0; k < MAX_ROCK; k++) r[k].t = -1;" + NL +
    "          for (int k = 0; k < MAX_ENEMY; k++) e[k].t = -1;" + NL +
    "          for (int k = 0; k < MAX_BULLETS; k++) b[k].age = -1;" + NL +
    "          score = 0; lives = 3; invuln = 120; game_over = 0;" + NL + "        }" + NL + "      }" + NL + "    }")
p.write_text(s, encoding="utf-8", newline="\n")
print("gyrocks patched")
