# SPDX-License-Identifier: GPL-2.0-or-later
"""Draw the VecDrive sunset overlay (846x1080 RGBA) for the core's Overlay Tools.

Same hardware-measured mapping as VecBreak: px = CX + x / KX, py = CY - y / KY.
Alpha: 255 solid print, 51 coloured filter, 0 clear. No anti-aliasing.

    python -I make_overlay.py [out.png]
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 846, 1080
KX, CX = 41.4, 416
KY, CY = 41.5, 534
HORIZON = 2500                       # match vecdrive.c
F = 51                               # filter alpha

FRAME = (22, 8, 44, 255)
RIM = (255, 90, 60, 255)
TITLE = (255, 170, 40, 255)
SUB = (190, 150, 220, 255)
SKY = [  # (top_y_from_horizon_px, colour) from the horizon upwards
    (40, (255, 140, 40)),            # orange glow at the horizon
    (100, (255, 80, 110)),           # coral
    (190, (220, 50, 170)),           # magenta
    (300, (140, 50, 210)),           # violet
    (9999, (60, 50, 190)),           # deep blue up to the top
]
GROUND = [(140, (210, 60, 170)), (9999, (40, 170, 210))]   # pink near the horizon, then teal


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
    d.fontmode = "1"
    win = (14, 64, W - 14, 996)
    d.rounded_rectangle((win[0] - 6, win[1] - 6, win[2] + 6, win[3] + 6), radius=30, fill=RIM)
    d.rounded_rectangle(win, radius=26, fill=(0, 0, 0, 0))

    hz = py(HORIZON)
    lo = hz
    for top, col in SKY:                            # bands upwards from the horizon
        hi = max(win[1], hz - top)
        d.rectangle((win[0], hi, win[2], lo - 1), fill=col + (F,))
        lo = hi
        if hi == win[1]:
            break
    lo = hz
    for depth, col in GROUND:                       # bands downwards
        hi = min(win[3], hz + depth)
        d.rectangle((win[0], lo, win[2], hi), fill=col + (F,))
        lo = hi + 1
        if hi == win[3]:
            break
    # re-cut the rounded window corners the bands painted over
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle(win, radius=26, fill=255)
    frame_layer = Image.new("RGBA", (W, H), FRAME)
    rim_layer = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(rim_layer).rounded_rectangle((win[0] - 6, win[1] - 6, win[2] + 6, win[3] + 6), radius=30, fill=RIM)
    outside = Image.alpha_composite(frame_layer, rim_layer)
    img = Image.composite(img, outside, mask)
    d = ImageDraw.Draw(img)
    d.fontmode = "1"

    d.text((W // 2, (win[3] + H) // 2 - 12), "VECDRIVE", font=font(62), fill=TITLE, anchor="mm")
    d.text((W // 2, H - 10), "PiTrex on MiSTer", font=font(17), fill=SUB, anchor="mm")
    d.text((W // 2, 30), "STICK: STEER    BUTTON 1: GAS    BUTTON 2: BRAKE", font=font(21), fill=SUB, anchor="mm")

    img.save(out)
    print(f"{out}: {W}x{H}, {len(img.getcolors(1 << 16))} colours, horizon row {hz}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).with_name("VecDrive.png")))
