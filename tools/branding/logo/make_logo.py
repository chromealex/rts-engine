#!/usr/bin/env python3
"""RTSEngine logo: the one source of the mark's geometry and every file made from it.

    python3 tools/branding/logo/make_logo.py          # rewrites the outputs below (needs Pillow)

Outputs (committed):
  engine/assets/branding/splash_blade_{top,left,right}.png, splash_diamond.png
      layers of the default loading screen (engine/splash/SplashDefault.cpp), white / orange on
      transparent, all in the same 1024 frame so they line up with the same anchor and scale
  engine/assets/branding/mark.png              the whole mark on transparent (white blades)
  apps/editor/branding/AppIcon.icns, RTSEditor.ico, icon_1024.png   editor application icon
  docs/images/icon.png                         README

The default game icon is drawn from the same polygons in C++ (tools/branding/Icons.cpp,
defaultIcon) so host tools need no file; keep both in sync when the mark changes.
"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

# The mark in a 1000 x 1000 frame (centre 500, 500). Blades: top chevron, two side blades mirrored.
TOP = [(500, 94), (373, 179), (373, 476), (500, 397), (627, 476), (627, 179)]
RIGHT = [(567, 741), (853, 907), (977, 833), (977, 719), (693, 561), (693, 663)]
LEFT = [(1000 - x, y) for x, y in RIGHT]
DIAMOND = [(500, 438), (350, 588), (500, 738), (650, 588)]

INK = (21, 21, 21, 255)
ORANGE = (254, 111, 3, 255)
WHITE = (255, 255, 255, 255)
SS = 4   # supersampling


def render(size, shapes, bg=(0, 0, 0, 0), inset=0.0, dy=0):
    """shapes: [(polygon, colour)]; inset: fraction of the side left empty on each edge; dy: frame shift."""
    big = size * SS
    img = Image.new("RGBA", (big, big), bg)
    d = ImageDraw.Draw(img)
    k = big * (1 - 2 * inset) / 1000.0
    o = big * inset
    for poly, col in shapes:
        d.polygon([(o + x * k, o + (y - dy) * k) for x, y in poly], fill=col)
    return img.resize((size, size), Image.LANCZOS)


def rounded(size, body, radius, bg):
    """Opaque rounded square of `body` px (centred) on a transparent canvas: the macOS icon shape."""
    big = size * SS
    img = Image.new("RGBA", (big, big), (0, 0, 0, 0))
    m = (size - body) / 2 * SS
    ImageDraw.Draw(img).rounded_rectangle([m, m, big - m, big - m], radius=radius * body * SS, fill=bg)
    return img.resize((size, size), Image.LANCZOS)


def out(*parts):
    p = os.path.join(ROOT, *parts)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    return p


def save(img, *parts):
    img.save(out(*parts), optimize=True)


def main():
    # loading screen layers: the frame is centred on the diamond so it turns around its own centre
    c = 88
    save(render(1024, [(TOP, WHITE)], dy=c), "engine", "assets", "branding", "splash_blade_top.png")
    save(render(1024, [(LEFT, WHITE)], dy=c), "engine", "assets", "branding", "splash_blade_left.png")
    save(render(1024, [(RIGHT, WHITE)], dy=c), "engine", "assets", "branding", "splash_blade_right.png")
    save(render(1024, [(DIAMOND, ORANGE)], dy=c), "engine", "assets", "branding", "splash_diamond.png")
    save(render(1024, [(TOP, WHITE), (LEFT, WHITE), (RIGHT, WHITE), (DIAMOND, ORANGE)]), "engine", "assets", "branding", "mark.png")

    # editor icon: Big Sur grid (824 px body of 1024, radius 0.225), mark inset inside the body
    ink = [(TOP, INK), (LEFT, INK), (RIGHT, INK), (DIAMOND, ORANGE)]
    body = rounded(1024, 824, 0.225, WHITE)
    body.alpha_composite(render(1024, ink, inset=0.19))
    save(body, "apps", "editor", "branding", "icon_1024.png")
    body.save(out("apps", "editor", "branding", "AppIcon.icns"))
    square = render(256, ink, bg=WHITE, inset=0.06)
    square.save(out("apps", "editor", "branding", "RTSEditor.ico"), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])

    save(render(512, ink, bg=WHITE, inset=0.08), "docs", "images", "icon.png")


if __name__ == "__main__":
    main()
