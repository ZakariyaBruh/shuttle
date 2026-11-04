#!/usr/bin/env python3
"""Airwave artwork: launcher card/icon/launch image, and the pieces the start-up animation draws (mark parts + wordmark letters).
The wordmark is the Playdate's own Roobert pixel font (from the SDK) scaled by whole pixels; the mark is drawn pixel by pixel.
Needs Pillow + numpy and PLAYDATE_SDK_PATH.   python3 tools/make_art.py"""
import os
import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "Source")
SDK = os.environ.get("PLAYDATE_SDK_PATH", os.path.expanduser("~/PlaydateSDK"))
TABLE = os.path.join(SDK, "CoreLibs/assets/keyboard/Roobert-24-Keyboard-Medium-table-36-36.png")
WHITE, BLACK = 255, 0
WORD = "airwave"

def glyph(ch):
    """Tight 1-bit glyph (white ink on black) from the Roobert table: 16 columns of 36x36 cells starting at ASCII 32."""
    t = Image.open(TABLE).convert("LA")
    i = ord(ch) - 32
    cell = t.crop(((i % 16) * 36, (i // 16) * 36, (i % 16) * 36 + 36, (i // 16) * 36 + 36))
    a = np.array(cell.getchannel("A")) > 127
    ys, xs = np.where(a)
    g = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    return g, ys.min()                      # bitmap, top offset inside the cell (for the baseline)

def to_img(bits, scale=1):
    im = Image.fromarray((bits * 255).astype(np.uint8), "L")
    return im.resize((im.width * scale, im.height * scale), Image.NEAREST) if scale != 1 else im

def wordmark(scale):
    """-> list of (letter image, x, y) laid out on a canvas, plus canvas size. Baseline-aligned."""
    gl = [glyph(c) for c in WORD]
    base = max(g.shape[0] + top for g, top in gl)            # common baseline (descender-free word, but keep it generic)
    x, parts = 0, []
    for g, top in gl:
        parts.append((to_img(g, scale), x, (top + (base - top - g.shape[0])) * scale))
        x += (g.shape[1] + 2) * scale
    h = base * scale
    return parts, x - 2 * scale, h

# ---- the mark: a dot and three arcs, drawn on a 24x24 grid then scaled
MARK_N = 24
def mark_part(part, scale):
    im = Image.new("L", (MARK_N, MARK_N), BLACK)
    d = ImageDraw.Draw(im)
    cx, cy = 4, 12
    if part == 0:
        d.ellipse([cx - 3, cy - 3, cx + 3, cy + 3], fill=WHITE)
    else:
        r = 4 + part * 5
        d.arc([cx - r, cy - r, cx + r, cy + r], -52, 52, fill=WHITE, width=2)
    return im.resize((MARK_N * scale, MARK_N * scale), Image.NEAREST)

def bayer(n):
    m = np.array([[0]])
    while m.shape[0] < n:
        m = np.block([[4 * m, 4 * m + 2], [4 * m + 3, 4 * m + 1]])
    return (m + 0.5) / (n * n)

def glow(w, h, cx, cy, r, strength=1.0):
    """Ordered-dither radial glow: a soft halftone halo (white on black)."""
    y, x = np.mgrid[0:h, 0:w]
    v = np.clip(1 - np.hypot(x - cx, y - cy) / r, 0, 1) ** 1.6 * strength
    th = np.tile(bayer(8), (h // 8 + 1, w // 8 + 1))[:h, :w]
    return Image.fromarray(((v > th) * 255).astype(np.uint8), "L")

def paste_white(dst, src, xy):
    """Compose white-on-black art onto dst (lighten)."""
    from PIL import ImageChops
    box = (xy[0], xy[1], xy[0] + src.width, xy[1] + src.height)
    dst.paste(ImageChops.lighter(dst.crop(box), src), box)

def compose(w, h, mark_scale, word_scale, mark_xy, word_xy, tagline=True, halo=True):
    im = Image.new("L", (w, h), BLACK)
    if halo:
        paste_white(im, glow(w, h, mark_xy[0] + 4 * mark_scale, mark_xy[1] + 12 * mark_scale, max(w, h) * 0.62, 0.55), (0, 0))
        # keep the halo behind the artwork: clear a dark plate under the mark and the wordmark so they stay crisp
    d = ImageDraw.Draw(im)
    plate_m = (mark_xy[0] - 4, mark_xy[1] - 4, mark_xy[0] + MARK_N * mark_scale + 4, mark_xy[1] + MARK_N * mark_scale + 4)
    d.rounded_rectangle(plate_m, radius=10 * (mark_scale > 1), fill=BLACK)
    for p in range(4):
        paste_white(im, mark_part(p, mark_scale), mark_xy)
    parts, ww, wh = wordmark(word_scale)
    d.rectangle((word_xy[0] - 6, word_xy[1] - 6, word_xy[0] + ww + 6, word_xy[1] + wh + 6 + (22 if tagline else 0)), fill=BLACK)
    for g, x, y in parts:
        paste_white(im, g, (word_xy[0] + x, word_xy[1] + y))
    if tagline:
        # tagline in the same pixel font at 1x
        tx = word_xy[0] + 2
        for c in "radio  tv  chat":
            if c == " ":
                tx += 5; continue
            g, top = glyph(c)
            paste_white(im, to_img(g, 1), (tx, word_xy[1] + wh + 14 + top - 4))
            tx += g.shape[1] + 2
    return im

def save1(im, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.convert("1", dither=Image.Dither.NONE).save(path)

def main():
    # ---- launcher art
    ww, wh = wordmark(2)[1:]
    mark = 3
    card = compose(350, 155, mark, 2, (14, 41), (14 + MARK_N * mark + 20, 44), tagline=True)
    save1(card, os.path.join(SRC, "launcher", "card.png"))
    icon = Image.new("L", (32, 32), BLACK)
    for p in range(4):
        paste_white(icon, mark_part(p, 1), (4, 4))
    save1(icon.resize((32, 32)), os.path.join(SRC, "launcher", "icon.png"))
    launch = compose(400, 240, 3, 2, (36, 84), (36 + MARK_N * 3 + 24, 88), tagline=True)
    save1(launch, os.path.join(SRC, "launcher", "launchImage.png"))

    # ---- start-up animation pieces (opaque, black background, drawn with plain copy)
    for p in range(4):
        save1(mark_part(p, 3), os.path.join(SRC, "images", f"mark{p}.png"))
    parts, ww, wh = wordmark(2)
    lay = ["#ifndef ART_LAYOUT_H", "#define ART_LAYOUT_H", "/* generated by tools/make_art.py */",
           f"#define MARK_SIZE {MARK_N * 3}", f"#define WORD_W {ww}", f"#define WORD_H {wh}", "#define WORD_N %d" % len(parts),
           "static const struct { int x, y, w, h; } WORD_LETTERS[] = {"]
    for i, (g, x, y) in enumerate(parts):
        save1(g, os.path.join(SRC, "images", f"letter{i}.png"))
        lay.append(f"\t{{ {x}, {y}, {g.width}, {g.height} }},")
    lay += ["};", "#endif", ""]
    open(os.path.join(HERE, "..", "src", "art_layout.h"), "w").write("\n".join(lay))
    print("wordmark", ww, "x", wh, "at x3")

if __name__ == "__main__":
    main()
