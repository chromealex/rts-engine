#!/usr/bin/env python3
"""Generates small seamless (tiling) terrain textures + normal maps for the terrain splat materials
(docs/game-format.md "Terrain materials"). Output is procedural (no third-party art): CC0.

    python3 tools/terrain/make_textures.py games/frostcrown/textures/terrain --preset frost

File names end in _repeat so the atlas packer never puts them on an atlas page (wrap needs its own
texture). Needs numpy + Pillow (host tool only)."""
import sys
import numpy as np
from PIL import Image

N = 256


def periodic_noise(rng, size, cells):
    """Tileable value noise: random lattice of `cells` x `cells` with wrap, smooth interpolation."""
    g = rng.random((cells, cells))
    t = np.arange(size) * cells / size
    i0 = np.floor(t).astype(int)
    f = t - i0
    f = f * f * (3 - 2 * f)
    i1 = (i0 + 1) % cells
    a = g[np.ix_(i0, i0)]; b = g[np.ix_(i0, i1)]; c = g[np.ix_(i1, i0)]; d = g[np.ix_(i1, i1)]
    fx = f[None, :]; fy = f[:, None]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(rng, octaves=(4, 8, 16, 32, 64), gains=None):
    gains = gains or [0.5 ** i for i in range(len(octaves))]
    v = sum(gn * periodic_noise(rng, N, o) for o, gn in zip(octaves, gains))
    return (v - v.min()) / (v.max() - v.min() + 1e-9)


def normal_map(h, strength):
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * strength
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * strength
    n = np.stack([-dx, dy, np.ones_like(h)], -1)
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return Image.fromarray(((n * 0.5 + 0.5) * 255).astype(np.uint8), "RGB")


def colorize(h, lo, hi, detail=None, dk=0.0):
    lo = np.array(lo, float); hi = np.array(hi, float)
    c = lo[None, None, :] * (1 - h[..., None]) + hi[None, None, :] * h[..., None]
    if detail is not None:
        c *= (1 - dk + dk * 2 * detail[..., None])
    return Image.fromarray(np.clip(c * 255, 0, 255).astype(np.uint8), "RGB")


def frost(out, rng):
    # snow: soft drifts + sparkle
    h = fbm(rng, (4, 8, 16, 32))
    sp = (rng.random((N, N)) > 0.995).astype(float)
    colorize(np.clip(h * 0.8 + sp * 0.6, 0, 1), (0.78, 0.83, 0.90), (0.97, 0.98, 1.0)).save(out + "/snow_repeat.png")
    normal_map(h, 2.0).save(out + "/snow_n_repeat.png")
    # frozen earth: dark soil with frost speckles
    h = fbm(rng, (8, 16, 32, 64))
    fr = fbm(rng, (16, 64))
    c = colorize(h, (0.30, 0.27, 0.24), (0.48, 0.44, 0.40), fr, 0.25)
    a = np.asarray(c).astype(float) / 255
    frost_mask = (fr > 0.62)[..., None]
    a = np.where(frost_mask, a * 0.4 + 0.6 * np.array([0.85, 0.89, 0.95]), a)
    Image.fromarray((a * 255).astype(np.uint8), "RGB").save(out + "/earth_repeat.png")
    normal_map(h, 4.0).save(out + "/earth_n_repeat.png")
    # packed path: compacted snow + gravel
    h = fbm(rng, (16, 32, 64))
    grav = (rng.random((N, N)) > 0.97).astype(float)
    colorize(np.clip(h * 0.7 + grav * 0.5, 0, 1), (0.62, 0.60, 0.58), (0.84, 0.84, 0.86)).save(out + "/path_repeat.png")
    normal_map(h + grav * 0.3, 3.0).save(out + "/path_n_repeat.png")
    # ice rock (cliffs): layered strata + cracks
    y = np.arange(N)[:, None] / N
    strata = 0.5 + 0.5 * np.sin((y * 9 + fbm(rng, (4, 8)) * 1.5) * 2 * np.pi)
    h = np.clip(strata * 0.5 + fbm(rng, (8, 16, 32, 64)) * 0.6, 0, 1)
    colorize(h, (0.30, 0.33, 0.38), (0.62, 0.66, 0.72)).save(out + "/rock_repeat.png")
    normal_map(h, 6.0).save(out + "/rock_n_repeat.png")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    rng = np.random.default_rng(11)
    frost(sys.argv[1], rng)
    print("wrote", sys.argv[1])
