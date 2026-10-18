"""Generates the material detail tiles in assets/materials/.

    python tools/make_material_tiles.py

Writes assets/materials/materials_<name>.bmp for every tile below, plus a contact
sheet for people at art_src/previews/materials.png. Deterministic and
standard-library-only, like every generator in tools/: two runs produce
byte-identical files, so a diff in assets/materials/ is a change to this script.

What a tile is, for whoever writes the shader that reads one (the per-material
look spec is assets/materials/README.md):

  - **A luminance-detail map, not a colour.** The cell's colour stays the
    simulation's (Element::color is replay state; the renderer may not repaint
    it), and the tile only moves its brightness:
        out = albedo * (1 + strength * (tile - 0.5))
    where tile is 0..1 and strength is the mood file's `material ... strength=`
    (the plan's R4a formula).
    Mid-grey (128) is "no change"; every tile is re-centred so its mean sits on
    128, which is what keeps a textured wall the same average brightness as the
    flat one it replaces -- the albedo and the grade stay the authority on value.
    strength=1 swings a black or white texel by half the albedo either way; the
    moods use 0.4..0.9.
  - **Greyscale, written as 24-bit BMP** with r = g = b, because the BMP codec is
    24-bit only (tools/pixel_art.py). Read any channel.
  - **One texel per world cell, world-anchored:** texel (x mod 64, y mod 64) for
    the cell at (x, y), so the pattern stays put as the camera scrolls. The cost
    is that a slab that breaks off and falls slides *under* its texture rather
    than carrying it -- acceptable for a fall that lasts a second, and the reason
    powders (sand) get no tile at all: a grain falling through a world-anchored
    pattern shimmers.
  - **Seamless in both axes.** Every function below is periodic in TILE: value
    noise on a lattice that wraps, Worley points measured with wrapped distance,
    sines with whole cycles.
  - **Not on the palette.** tools/validate_palette.py is for colour art; these
    are data maps, like a material map, and it will report them as off-palette.

The detail is pre-shaded the way Rain World pre-shades its tiles: crack lips and
plank tops carry a faint top-lit bevel so the texture reads as relief even under
flat light, small enough that a light from below does not make it look wrong.
"""

import math
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, 'tools'))
from pixel_art import ROOT, asset_path, rnd, stage, write_bmp, write_png

TILE = 64
SET = 'materials'


# --- periodic 2D primitives ---------------------------------------------------
#
# pixel_art's wnoise/wsin are 1D; the tiles need the same wrapping in two axes.
# rnd() is the shared, version-proof hash, so a Python upgrade cannot move a texel.


def _lattice(ix, iy, cells, seed):
    """0..1 at a wrapped lattice point."""
    return rnd((iy % cells) * cells + (ix % cells), seed)


def vnoise(x, y, cells, seed):
    """Value noise in 0..1 with `cells` lattice points across TILE, both axes."""
    u = (x % TILE) * cells / TILE
    v = (y % TILE) * cells / TILE
    ix, iy = int(math.floor(u)), int(math.floor(v))
    fx, fy = u - ix, v - iy
    sx, sy = fx * fx * (3 - 2 * fx), fy * fy * (3 - 2 * fy)
    a = _lattice(ix, iy, cells, seed)
    b = _lattice(ix + 1, iy, cells, seed)
    c = _lattice(ix, iy + 1, cells, seed)
    d = _lattice(ix + 1, iy + 1, cells, seed)
    top = a + (b - a) * sx
    bot = c + (d - c) * sx
    return top + (bot - top) * sy


def fbm(x, y, octaves, seed):
    """Sum of vnoise octaves, `octaves` = [(cells, weight)], centred on zero."""
    total = 0.0
    for i, (cells, weight) in enumerate(octaves):
        total += (vnoise(x, y, cells, seed + 101 * i) - 0.5) * weight
    return total


def _wrap_delta(d):
    """Shortest signed distance on a ring TILE around."""
    d %= TILE
    return d - TILE if d > TILE / 2 else d


def worley(x, y, n, seed, jitter=0.8):
    """Distances to the nearest and second-nearest jittered points of an n x n
    grid over the tile, with wrapped distance: (F1, F2, id of nearest)."""
    step = TILE / n
    gx, gy = int(x // step), int(y // step)
    best = [(1e9, -1), (1e9, -1)]
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            cx, cy = (gx + dx) % n, (gy + dy) % n
            k = cy * n + cx
            px = (cx + 0.5 + (rnd(k, seed) - 0.5) * jitter) * step
            py = (cy + 0.5 + (rnd(k, seed + 7) - 0.5) * jitter) * step
            d = math.hypot(_wrap_delta(x - px), _wrap_delta(y - py))
            if d < best[0][0]:
                best = [(d, k), best[0]]
            elif d < best[1][0]:
                best[1] = (d, k)
    return best[0][0], best[1][0], best[0][1]


def normalise(values, spread):
    """Re-centres a field on 128 with standard deviation `spread` levels, clamps
    to 0..255, then nudges once more so the clamp does not move the mean. A tile
    whose mean is not 128 brightens or darkens the material as a whole, which is
    the grade's job, not the texture's."""
    n = len(values)
    mean = sum(values) / n
    sd = math.sqrt(sum((v - mean) ** 2 for v in values) / n) or 1.0
    out = [128 + (v - mean) / sd * spread for v in values]
    for _ in range(3):
        clamped = [min(255.0, max(0.0, v)) for v in out]
        shift = 128 - sum(clamped) / n
        if abs(shift) < 0.25:
            break
        out = [v + shift for v in out]
    return [int(round(min(255.0, max(0.0, v)))) for v in out]


# --- the tiles ------------------------------------------------------------------


def rock(x, y):
    """Wall: weathered rock. Mottled grime, faint warped strata, broken cracks
    with a lit upper lip, and sparse mineral flecks that catch light (Backbone's
    wet sparkle; the moods' `spec` decides how much they do)."""
    grime = fbm(x, y, [(4, 0.55), (8, 0.35), (16, 0.2), (32, 0.12)], 11)
    warp = vnoise(x, y, 4, 23) * 1.4
    strata = 0.16 * math.sin(2 * math.pi * (3 * y / TILE + warp))

    def crack_at(cx, cy):
        f1, f2, _ = worley(cx, cy, 5, 31)
        mask = vnoise(cx, cy, 8, 37)  # cracks are broken runs, not a honeycomb
        return (f2 - f1) < 0.9 and mask > 0.52

    v = grime + strata
    if crack_at(x, y):
        v -= 0.75
    elif crack_at(x, y + 1):
        v += 0.28  # the lip above a crack faces the sky
    elif crack_at(x, y - 1):
        v -= 0.2  # and the face under one is in its shadow
    if rnd(y * TILE + x, 41) > 0.985:
        v += 0.45
    return v


def wood(x, y):
    """Wood: horizontal planks eight cells tall, which is how the levels lay it.
    Grain lines run along the plank and wander, every plank has its own tone,
    one staggered butt joint each, a knot in some, and a bevel at the seams."""
    plank_h = 8
    p = (y // plank_h) % (TILE // plank_h)
    row = y % plank_h
    tone = (rnd(p, 53) - 0.5) * 0.35
    warp = vnoise(x, p * plank_h, 4, 59 + p) * 1.3 + vnoise(x, y, 16, 61) * 0.25
    phase = row * 0.42 + warp + rnd(p, 67) * 3
    grain = -0.4 if (phase % 1.0) < 0.22 else 0.06 * math.sin(2 * math.pi * phase)
    v = tone + grain + fbm(x, y, [(32, 0.12)], 71)

    joint = int(rnd(p, 73) * TILE)
    if (x - joint) % TILE == 0:
        v -= 0.85
    elif (x - joint) % TILE == 1:
        v += 0.15
    if rnd(p, 79) > 0.5:  # a knot, in about half the planks
        kx, ky = int(rnd(p, 83) * TILE), p * plank_h + 3.5
        d = math.hypot(_wrap_delta(x - kx) / 2.2, _wrap_delta(y - ky))
        if d < 1.0:
            v -= 0.7
        elif d < 2.2:
            v -= 0.25 * math.cos((d - 1.0) * math.pi)
    if row == plank_h - 1:
        v -= 0.95  # seam
    elif row == 0:
        v += 0.25  # top edge of the plank below the seam catches light
    return v


def charred(x, y):
    """Charred wood: alligator crazing, the cracked scales burnt wood breaks
    into. Scales are domed (brighter toward their middle) and the cracks are the
    one flat value, the tile's minimum (56 as committed), with nothing else within
    60 levels of it -- so `tile < 0.35` is a clean crack mask, which the shader
    may reuse as where embers glow on a burning (Charred) cell."""
    f1, f2, k = worley(x, y, 7, 89, jitter=0.9)
    if f2 - f1 < 0.7:
        return -1.1
    dome = 0.35 * (1 - min(1.0, f1 / 6.0))
    return dome + (rnd(k, 97) - 0.5) * 0.25 + fbm(x, y, [(32, 0.15), (16, 0.1)], 101)


def caustics(x, y):
    """Water: a caustic net -- bright filaments where Worley cells meet, domain
    warped so the net is organic rather than cellular. Mid-grey is "no light
    added". Meant to be sampled twice at two scroll speeds and combined with min()
    so it crawls rather than slides (R4b)."""
    wx = x + 3.0 * (vnoise(x, y, 4, 103) - 0.5)
    wy = y + 3.0 * (vnoise(x, y, 4, 107) - 0.5)
    f1, f2, _ = worley(wx, wy, 4, 109)
    edge = f2 - f1
    return math.exp(-(edge * edge) / 2.0) + 0.05 * (vnoise(x, y, 16, 113) - 0.5)


# name, field, spread (std dev in grey levels), what reads it
TILES = [
    ('rock', rock, 30, 'Wall'),
    ('wood', wood, 30, 'Wood'),
    ('charred', charred, 34, 'Charred'),
    ('caustics', caustics, 26, 'Water'),
]


def build(field, spread):
    values = [field(x, y) for y in range(TILE) for x in range(TILE)]
    return normalise(values, spread)


def contact_sheet(tiles):
    """art_src/previews/materials.png: each tile repeated 2x2 (so a seam would
    show) and magnified 3x, side by side, with an 8-pixel gutter."""
    mag, rep, gutter = 3, 2, 8
    cell = TILE * rep * mag
    w = len(tiles) * cell + (len(tiles) + 1) * gutter
    h = cell + 2 * gutter
    px = [(40, 40, 40)] * (w * h)
    for i, grey in enumerate(tiles):
        ox = gutter + i * (cell + gutter)
        for y in range(cell):
            for x in range(cell):
                g = grey[((y // mag) % TILE) * TILE + (x // mag) % TILE]
                px[(gutter + y) * w + ox + x] = (g, g, g)
    path = os.path.join(ROOT, 'art_src', 'previews', 'materials.png')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    write_png(path, w, h, px)
    return path


def main():
    os.makedirs(asset_path(SET), exist_ok=True)
    sheets = []
    for name, field, spread, reader in TILES:
        grey = build(field, spread)
        path = asset_path(SET, f'{SET}_{name}.bmp')
        write_bmp(path, TILE, TILE, [(g, g, g) for g in grey])
        stage(path, SET)
        mean = sum(grey) / len(grey)
        print(
            f'{SET}_{name}.bmp  {TILE}x{TILE}  mean {mean:.1f}  '
            f'min {min(grey)} max {max(grey)}  ({reader})'
        )
        sheets.append(grey)
    print(contact_sheet(sheets))


if __name__ == '__main__':
    main()
