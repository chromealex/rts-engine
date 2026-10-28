#!/usr/bin/env python3
"""Placeholder branding art for the sample games: icons and splash images (CC0, generated here).

    python3 games/_tools/brandart.py            # writes games/skirmish/branding/*.png

Everything is drawn from geometric shapes (no fonts, no third-party art), supersampled 4x and
downscaled, so the output is clean at every icon size. Requires Pillow. The PNGs are committed;
re-run only to change the art. Released under CC0 1.0 (public domain), like the output.
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SS = 4  # supersampling

NAVY = (16, 26, 40)
NAVY_LIGHT = (40, 62, 88)
IRON = (120, 132, 146)
IRON_DARK = (70, 80, 92)
IRON_LIGHT = (176, 186, 198)
GOLD = (232, 182, 64)
GOLD_DARK = (168, 112, 30)
GOLD_LIGHT = (255, 226, 140)


def canvas(w, h, color=(0, 0, 0, 0)):
    return Image.new("RGBA", (w * SS, h * SS), color)


def finish(img, w, h):
    return img.resize((w, h), Image.LANCZOS)


def radial(w, h, inner, outer, cx=0.5, cy=0.45, power=1.4):
    img = Image.new("RGBA", (w, h))
    px = img.load()
    rmax = math.hypot(max(cx, 1 - cx) * w, max(cy, 1 - cy) * h)
    for y in range(h):
        for x in range(w):
            t = min(1.0, (math.hypot(x - cx * w, y - cy * h) / rmax)) ** power
            px[x, y] = tuple(int(inner[i] + (outer[i] - inner[i]) * t) for i in range(3)) + (255,)
    return img


def shield_points(cx, cy, s):
    """A heater shield: flat top, curved sides meeting in a point."""
    pts = [(cx - s, cy - s * 0.95), (cx + s, cy - s * 0.95)]
    for i in range(1, 21):
        a = i / 20
        pts.append((cx + s * (1 - a ** 1.8), cy - s * 0.95 + a * s * 2.15))
    for i in range(20, 0, -1):
        a = i / 20
        pts.append((cx - s * (1 - a ** 1.8), cy - s * 0.95 + a * s * 2.15))
    return pts


def star(cx, cy, r_out, r_in, n=5, rot=-math.pi / 2):
    pts = []
    for i in range(n * 2):
        r = r_out if i % 2 == 0 else r_in
        a = rot + i * math.pi / n
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def emblem(size, with_glow=True, medallion=True):
    """Iron shield with a gold star medallion (transparent background); without it a dark
    socket the splash spins its coin in."""
    img = canvas(size, size)
    d = ImageDraw.Draw(img)
    S = size * SS
    cx, cy, s = S / 2, S * 0.47, S * 0.36
    if with_glow:
        glow = canvas(size, size)
        ImageDraw.Draw(glow).polygon(shield_points(cx, cy, s * 1.04), fill=GOLD + (150,))
        img = Image.alpha_composite(img, glow.filter(ImageFilter.GaussianBlur(S * 0.03)))
        d = ImageDraw.Draw(img)
    d.polygon(shield_points(cx, cy, s), fill=GOLD_DARK)                 # gold rim
    d.polygon(shield_points(cx, cy + s * 0.02, s * 0.9), fill=IRON_DARK)  # iron plate
    d.polygon(shield_points(cx, cy, s * 0.86), fill=IRON)
    # bevel highlight on the left half
    hl = [p for p in shield_points(cx, cy, s * 0.86) if p[0] <= cx]
    d.polygon(hl + [(cx, cy + s * 1.1), (cx, cy - s * 0.82)], fill=IRON_LIGHT + (90,))
    # rivets
    for rx in (-0.68, 0.68):
        for ry in (-0.68, 0.2):
            r = s * 0.05
            d.ellipse((cx + rx * s - r, cy + ry * s - r, cx + rx * s + r, cy + ry * s + r), fill=IRON_LIGHT)
    mr = s * 0.42
    if not medallion:
        d.ellipse((cx - mr, cy - mr, cx + mr, cy + mr), fill=GOLD_DARK)
        d.ellipse((cx - mr * 0.88, cy - mr * 0.88, cx + mr * 0.88, cy + mr * 0.88), fill=NAVY)
        return finish(img, size, size)
    # gold medallion with a star
    d.ellipse((cx - mr, cy - mr, cx + mr, cy + mr), fill=GOLD_DARK)
    d.ellipse((cx - mr * 0.88, cy - mr * 0.88, cx + mr * 0.88, cy + mr * 0.88), fill=GOLD)
    d.polygon(star(cx, cy, mr * 0.66, mr * 0.28), fill=GOLD_LIGHT)
    return finish(img, size, size)


def icon_master(size=1024):
    bg = radial(size, size, NAVY_LIGHT, NAVY)
    fg = emblem(size)
    big = fg.resize((int(size * 1.08), int(size * 1.08)), Image.LANCZOS)
    o = (size - big.width) // 2
    out = bg.copy()
    out.alpha_composite(big, (o, o + int(size * 0.03)))
    return out


def monochrome(size=1024):
    e = emblem(size, with_glow=False)
    a = e.split()[3]
    white = Image.new("RGBA", (size, size), (255, 255, 255, 0))
    white.putalpha(a)
    return white


def studio_mark(size=512):
    """Engine 'studio' mark: three gold chevrons inside an iron ring."""
    img = canvas(size, size)
    d = ImageDraw.Draw(img)
    S = size * SS
    c = S / 2
    d.ellipse((S * 0.06, S * 0.06, S * 0.94, S * 0.94), outline=IRON_LIGHT, width=int(S * 0.05))
    for i, k in enumerate((-0.2, 0.0, 0.2)):
        y = c + k * S
        w = S * (0.26 - i * 0.03)
        t = S * 0.07
        col = (GOLD, GOLD_LIGHT, GOLD)[i]
        d.polygon([(c - w, y + w * 0.55), (c, y - w * 0.15), (c + w, y + w * 0.55), (c + w, y + w * 0.55 - t), (c, y - w * 0.15 - t), (c - w, y + w * 0.55 - t)], fill=col)
    return finish(img, size, size)


def coin_sheet(frame=128, frames=8, cols=4):
    """A spinning gold coin, frames left to right, top to bottom (sprite sheet for the splash)."""
    rows = (frames + cols - 1) // cols
    sheet = Image.new("RGBA", (frame * cols, frame * rows), (0, 0, 0, 0))
    face = canvas(frame, frame)
    d = ImageDraw.Draw(face)
    S, c = frame * SS, frame * SS / 2
    r = S * 0.42
    d.ellipse((c - r, c - r, c + r, c + r), fill=GOLD_DARK)
    d.ellipse((c - r * 0.88, c - r * 0.88, c + r * 0.88, c + r * 0.88), fill=GOLD)
    d.polygon(star(c, c, r * 0.58, r * 0.24), fill=GOLD_LIGHT)
    face = finish(face, frame, frame)
    for i in range(frames):
        k = max(0.08, abs(math.cos(i / frames * math.pi)))   # width of the turning coin
        img = Image.new("RGBA", (frame, frame), (0, 0, 0, 0))
        edge = face.resize((max(1, int(frame * k)), frame), Image.LANCZOS)
        if k < 0.99:   # the coin's edge peeks out on the side it turns away from
            rim = Image.new("RGBA", edge.size, GOLD_DARK + (255,))
            rim.putalpha(edge.split()[3])
            img.alpha_composite(rim, ((frame - edge.width) // 2 + max(1, int(frame * 0.03 * (1 - k))), 0))
        img.alpha_composite(edge, ((frame - edge.width) // 2, 0))
        sheet.alpha_composite(img, ((i % cols) * frame, (i // cols) * frame))
    return sheet


def splash_background(w=480, h=270):
    return radial(w, h, (30, 46, 66), (8, 12, 18), cy=0.5, power=1.2)


def main():
    out = os.path.join(ROOT, "skirmish", "branding")
    os.makedirs(out, exist_ok=True)
    files = {
        "icon.png": icon_master(1024),
        "icon_foreground.png": emblem(1024),
        "icon_monochrome.png": monochrome(1024),
        "studio.png": studio_mark(512),
        "logo.png": emblem(768),
        "logo_socket.png": emblem(768, medallion=False),
        "coin_sheet.png": coin_sheet(),
        "splash_bg.png": splash_background(),
    }
    for name, img in files.items():
        path = os.path.join(out, name)
        img.save(path, optimize=True)
        print(f"{path}: {img.width}x{img.height}, {os.path.getsize(path) // 1024} KB")
    with open(os.path.join(out, "LICENSE.txt"), "w") as f:
        f.write("Placeholder art generated by games/_tools/brandart.py. CC0 1.0 Universal (public domain).\n")


if __name__ == "__main__":
    main()
