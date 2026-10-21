# SPDX-License-Identifier: GPL-2.0-or-later
"""An original overlay for the UVMC2 Tac/Scan port (846x1080 RGBA, for the core's Overlay Tools).

Radial colour bands centred on the screen origin, where the game's warp tunnel is: a warm
core through orange and magenta out to deep blue, so the tunnel and the formations change
colour as they move outwards. Rounded window and rim in the same style as VecDrive's.
No artwork from the arcade game is used.

    python -I make_overlay.py [out.png]
then, from core/Overlay Tools:  python build_art.py --max-colors 256 build <out.png> -o <dir>
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 846, 1080
CX, CY = 416, 534                    # the screen origin (measured, as for VecBreak/VecDrive)
F = 51                               # filter alpha: coloured tint, vectors show through

FRAME = (8, 6, 26, 255)
RIM = (70, 200, 255, 255)
TITLE = (120, 220, 255, 255)
SUB = (150, 140, 220, 255)
BANDS = [  # (radius in pixels, colour) from the outside in; each disc paints over the last
    (9999, (40, 60, 200)),           # deep blue at the edges
    (360, (110, 50, 210)),           # violet
    (250, (210, 50, 170)),           # magenta
    (160, (255, 110, 60)),           # orange
    (80, (255, 210, 70)),            # warm yellow core
]


def font(size):
    for name in ("impact.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main(out):
    win = (14, 64, W - 14, 996)
    bands = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(bands)
    for r, col in BANDS:
        r = min(r, 2000)
        d.ellipse((CX - r, CY - r, CX + r, CY + r), fill=col + (F,))

    # frame and rim outside a rounded window; the bands show only inside it
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle(win, radius=26, fill=255)
    outside = Image.new("RGBA", (W, H), FRAME)
    ImageDraw.Draw(outside).rounded_rectangle((win[0] - 6, win[1] - 6, win[2] + 6, win[3] + 6),
                                              radius=30, fill=RIM)
    img = Image.composite(bands, outside, mask)

    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    d.text((W // 2, 30), "1: FIRE    2: GRAB    3: COIN    4: START", font=font(22), fill=SUB, anchor="mm")
    d.text((W // 2, (win[3] + H) // 2 - 12), "UVMC2 ARCADE", font=font(54), fill=TITLE, anchor="mm")
    d.text((W // 2, H - 10), "UVMC2 on MiSTer", font=font(17), fill=SUB, anchor="mm")

    img.save(out)
    print(f"{out}: {W}x{H}, {len(img.getcolors(1 << 16))} colours")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).with_name("TacScan.png")))
