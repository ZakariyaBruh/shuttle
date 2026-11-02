#!/usr/bin/env python3
"""Generates the launcher artwork (front cover) into Source/launcher/:
   card.png 350x155, icon.png 32x32, launchImage.png 400x240.  Needs Pillow.  Run: python3 tools/make_art.py"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__), "..", "Source", "launcher")
BOLD = "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"
REG = "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"
BLACK, WHITE = 0, 255


def waves(d, cx, cy, r0, r1, step, width, span=70, sx=1.0):
    """Radio waves: arcs fanning out to the right of (cx, cy), thinner and sparser the further they travel."""
    r = r0
    i = 0
    while r <= r1:
        w = max(1, width - i // 2)
        box = [cx - r, cy - r, cx + r, cy + r]
        d.arc(box, -span, span, fill=WHITE, width=w)
        r += step + i
        i += 1


def dotted(d, cx, cy, r, n, size):
    for k in range(n):
        a = -80 + 160 * k / (n - 1)
        x, y = cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))
        d.ellipse([x - size, y - size, x + size, y + size], fill=WHITE)


def canvas(w, h):
    return Image.new("L", (w, h), BLACK)


def finish(img, name):
    # a little dither on the grey antialiasing keeps the edges crisp but smooth
    img.convert("1", dither=Image.Dither.FLOYDSTEINBERG).save(os.path.join(OUT, name))


def card():
    im = canvas(350, 155)
    d = ImageDraw.Draw(im)
    # transmitter dot + waves on the left
    d.ellipse([22, 67, 42, 87], fill=WHITE)
    waves(d, 32, 77, 22, 120, 14, 5, 65)
    # wordmark
    f = ImageFont.truetype(BOLD, 50)
    d.text((144, 34), "Airwave", font=f, fill=WHITE)
    d.rectangle([148, 98, 326, 100], fill=WHITE)
    t = ImageFont.truetype(REG, 15)
    d.text((146, 108), "radio  podcasts  TV  chat", font=t, fill=WHITE)
    finish(im, "card.png")


def icon():
    im = canvas(32, 32)
    d = ImageDraw.Draw(im)
    d.ellipse([3, 13, 9, 19], fill=WHITE)
    for k, r in enumerate((8, 14, 20)):
        d.arc([6 - r, 16 - r, 6 + r, 16 + r], -50, 50, fill=WHITE, width=2)
    finish(im, "icon.png")


def launch():
    im = canvas(400, 240)
    d = ImageDraw.Draw(im)
    d.ellipse([26, 108, 50, 132], fill=WHITE)
    waves(d, 38, 120, 26, 112, 14, 6, 62)
    f = ImageFont.truetype(BOLD, 60)
    d.text((152, 70), "Airwave", font=f, fill=WHITE)
    d.rectangle([156, 148, 384, 150], fill=WHITE)
    t = ImageFont.truetype(REG, 15)
    d.text((156, 160), "radio  podcasts  TV  chat", font=t, fill=WHITE)
    finish(im, "launchImage.png")


if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    card(); icon(); launch()
    print("wrote", sorted(os.listdir(OUT)))
