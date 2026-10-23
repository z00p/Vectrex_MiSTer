# SPDX-License-Identifier: GPL-2.0-or-later
"""An original overlay for Roger Boesch's Gong Rider (846x1080 RGBA, for the core's Overlay Tools).

The game draws two diagonals through the screen centre; the four wedges they make hold the
four gongs (up, right, down, left: the stick direction that strikes each). Each wedge gets
its own gong colour, with a warm lamp glow over the tunnel in the centre. Night-blue frame
with a gold rim. No artwork from the game or from Master of the Lamps is used.

    python -I make_overlay.py [out.png]
then, from core/Overlay Tools:  python build_art.py --max-colors 256 build <out.png> -o <dir>
"""
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

W, H = 846, 1080
CX, CY = 416, 534                    # the screen origin (measured, as for VecBreak/VecDrive)
F = 56                               # filter alpha: coloured tint, vectors show through

FRAME = (10, 8, 34, 255)
RIM = (230, 180, 70, 255)
TITLE = (245, 200, 90, 255)
SUB = (200, 170, 230, 255)
GONGS = {                             # wedge colours, one per gong
    "up": (255, 190, 60),            # gold
    "right": (235, 60, 70),          # ruby
    "down": (40, 190, 120),          # emerald
    "left": (70, 120, 255),          # sapphire
}
GLOW = [(150, (255, 150, 40)), (95, (255, 200, 90)), (45, (255, 240, 170))]  # outside in


def font(size):
    for name in ("impact.ttf", "arialbd.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default()


def main(out):
    win = (14, 64, W - 14, 996)
    tint = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(tint)
    far = 3000                       # wedges bounded by the 45-degree diagonals through the origin
    d.polygon([(CX, CY), (CX - far, CY - far), (CX + far, CY - far)], fill=GONGS["up"] + (F,))
    d.polygon([(CX, CY), (CX + far, CY - far), (CX + far, CY + far)], fill=GONGS["right"] + (F,))
    d.polygon([(CX, CY), (CX + far, CY + far), (CX - far, CY + far)], fill=GONGS["down"] + (F,))
    d.polygon([(CX, CY), (CX - far, CY + far), (CX - far, CY - far)], fill=GONGS["left"] + (F,))
    glow = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    for r, col in GLOW:
        g.ellipse((CX - r, CY - r, CX + r, CY + r), fill=col + (F + 10,))
    glow = glow.filter(ImageFilter.GaussianBlur(18))
    tint = Image.alpha_composite(tint, glow)

    # frame and rim outside a rounded window; the tint shows only inside it
    mask = Image.new("L", (W, H), 0)
    ImageDraw.Draw(mask).rounded_rectangle(win, radius=26, fill=255)
    outside = Image.new("RGBA", (W, H), FRAME)
    ImageDraw.Draw(outside).rounded_rectangle((win[0] - 6, win[1] - 6, win[2] + 6, win[3] + 6),
                                              radius=30, fill=RIM)
    img = Image.composite(tint, outside, mask)

    d = ImageDraw.Draw(img)
    d.fontmode = "1"
    d.text((W // 2, 30), "STICK: STEER AND STRIKE THE GONGS    1-4: GONGS", font=font(21), fill=SUB, anchor="mm")
    d.text((W // 2, (win[3] + H) // 2 - 12), "GONG RIDER", font=font(56), fill=TITLE, anchor="mm")
    d.text((W // 2, H - 10), "a game by Roger Boesch", font=font(17), fill=SUB, anchor="mm")

    img = img.quantize(colors=256).convert("RGBA")
    img.save(out)
    print(f"{out}: {W}x{H}, {len(img.getcolors(1 << 16))} colours")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else str(Path(__file__).with_name("GongRider.png")))
