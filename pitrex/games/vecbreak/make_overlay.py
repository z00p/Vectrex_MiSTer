# SPDX-License-Identifier: GPL-2.0-or-later
"""Draw the VecBreak overlay (846x1080 RGBA) for the core's Overlay Tools.

Game coordinates map to overlay pixels as px = CX + x / KX, py = CY - y / KY,
measured on hardware (20 Oct 2026) from the walls, brick rows and bat with this overlay loaded.
Alpha follows the core's convention: 255 solid print, 51 coloured filter, 0 clear.
Everything is drawn without anti-aliasing so the palette stays small.

    python -I make_overlay.py [out.png]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 846, 1080
KX, CX = 42.4, 417     # horizontal: units per pixel, centre column
KY, CY = 41.5, 534     # vertical: units per pixel, centre row
FIELD_L, FIELD_R, FIELD_T, FIELD_B = -15000, 15000, 14500, -17500   # match vecbreak.c
BRICK_TOP, BRICK_STEP, ROWS = 11500, 1300, 5
BAT_Y = -15000

FRAME = (14, 16, 44, 255)          # deep navy print
RIM = (255, 70, 170, 255)          # neon pink rim
TITLE = (255, 200, 40, 255)
SUB = (150, 160, 200, 255)
FILTER_A = 51
ROW_COLOURS = [(235, 45, 45), (245, 140, 25), (245, 225, 40), (50, 210, 70), (40, 170, 245)]
BAT_COLOUR = (90, 130, 255)


def px(x): return round(CX + x / KX)
def py(y): return round(CY - y / KY)


def font(size):
    for name in ("impact.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main(out):
    img = Image.new("RGBA", (W, H), FRAME)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"                                  # no anti-aliasing

    # play window: clear, a little outside the walls and score line
    win = (px(FIELD_L) - 14, 96, px(FIELD_R) + 14, py(FIELD_B) + 12)
    d.rounded_rectangle((win[0] - 6, win[1] - 6, win[2] + 6, win[3] + 6), radius=26, fill=RIM)
    d.rounded_rectangle(win, radius=22, fill=(0, 0, 0, 0))

    # coloured filters over the brick rows and the bat zone
    half = BRICK_STEP // 2 - 60
    for r in range(ROWS):
        y = BRICK_TOP - r * BRICK_STEP
        d.rectangle((win[0], py(y + half), win[2], py(y - half)), fill=ROW_COLOURS[r] + (FILTER_A,))
    d.rectangle((win[0], py(BAT_Y + 800), win[2], py(BAT_Y - 800)), fill=BAT_COLOUR + (FILTER_A,))

    # title panel
    t = font(64)
    d.text((W // 2, (win[3] + H) // 2 - 16), "VECBREAK", font=t, fill=TITLE, anchor="mm")
    d.text((W // 2, H - 12), "PiTrex on MiSTer", font=font(18), fill=SUB, anchor="mm")
    d.text((W // 2, 46), "JOYSTICK: BAT     BUTTON 1: SERVE", font=font(22), fill=SUB, anchor="mm")

    img.save(out)
    colours = len(img.getcolors(1 << 16))
    print(f"{out}: {W}x{H}, {colours} colours, window {win}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).with_name("VecBreak.png")))
