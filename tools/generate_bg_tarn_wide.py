"""Generates bg_tarn_wide: bg_tarn's lake as a long walk along its shore.

    python tools/generate_bg_tarn_wide.py

Writes assets/bg_tarn_wide/ (thirteen layers, a composite preview and a README)
and assets/bg_tarn_wide_material.bmp / bg_tarn_wide_albedo.bmp (the playable
scene).

--- what it is ------------------------------------------------------------------

The same place as bg_tarn -- a mountain lake at sunset under a far range -- but
a world 2752 cells wide, eight screens at 3440x1440 and fourteen at 1920x1080,
which the camera follows from one end to the other. Ethan asked for it in the
style of Cast n Chill / Wild n Chill, which is a particular way of drawing a
shore:

  - Backlit. The sun is low and ahead of the viewer, so every layer is a
    silhouette, lit only along its top edge. Nothing is modelled with a lit face
    and a shaded face except the far range, which is big enough to have them.
  - Atmospheric. Value and hue climb toward the sky with distance in clean,
    flat steps: near pines are indigo-teal, the far treeline violet, the range
    lavender. Each layer is essentially one colour plus a rim, and the depth is
    in the ladder between layers, not inside any of them.
  - Soft and banded. The sky is wide flat bands with stepped edges, not a
    dithered ramp, and so is the lake, which mirrors it.
  - Cozy. Autumn maples and birches among the pines are the one saturated thing
    in the middle ground, and the walk passes a camp, a dock, an island with a
    canoe, a cabin with its windows lit, and a waterfall in a rocky narrows.

--- what is new in the contract ----------------------------------------------

Everything bg_tarn's generator says about the rig still binds this one (feet
are factors, the plane is line-scrolled and undithered, every layer tiles).
Two things are new.

  1. Layers are not all the set's width. backdrop_set.h ("why a layer may be
     narrower than its set") has the argument: over the whole walk a layer at
     factor f shows V + f * (W - V) columns, so the sky needs 345 and the near
     shore nearly all 2752. The far layers are 688 or 1376 wide and say so with
     width= in backdrop.txt; backdrop_set_test checks each is wide enough that
     no 3440-wide window ever sees it twice.

  2. Things are placed in world columns, not art columns. A layer at factor f
     shows art column  WC + f * (wx - WC)  at the screen's centre when the
     camera's centre is at world column wx (WC is the world's middle, where the
     standing anchor puts the painting exactly). That is independent of the
     window size, so `place(wx, f)` puts the cabin behind the stretch of shore
     the player is walking on at every resolution. The zones below are world
     columns for that reason.

The far shore's reflection is a layer of its own, on_plane at the far shore's
factor: a reflection scrolls with the thing it reflects, not with the water it
lies on, which is the glint's argument (bg_tarn) applied at a factor that is not
zero.

Deterministic and standard-library-only, like every generator in tools/. Every
colour is a PALETTE name (the wt_ entries in tools/pixel_art.py).
"""

import math
import os
import shutil
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, 'tools'))
from pixel_art import (  # noqa: E402
    COLOR_KEY,
    LEGEND_EMPTY,
    LEGEND_WALL,
    assert_legend_matches_header,
    color_of,
    write_bmp,
    write_png,
)

NAME = 'bg_tarn_wide'
DST_DIR = os.path.join(REPO_ROOT, 'assets', NAME)
BUILD_DST_DIRS = [
    os.path.join(REPO_ROOT, 'build', cfg, 'assets', NAME) for cfg in ('Release', 'Debug', '')
]

W = 2752  # the world, and the near layers
H = 288
WC = W // 2  # the world's middle: where the standing anchor makes the painting exact
KEY = COLOR_KEY

# The rig is bg_tarn's, so the two scenes feel like the same lake: the same eye
# height, the same depth ladder, the same vertical strength.
HORIZON = 200
CONTACT = 264


def factor_at(row):
    return (row - HORIZON) / float(CONTACT - HORIZON)


MOUNTAIN_FOOT = 202  # 0.031
RIDGE_FOOT = 204  # 0.063
HILL_FOOT = 207  # 0.109
FAR_SHORE_FOOT = 210  # 0.156 - the far treeline, and what the water mirrors
ISLET_FOOT = 224  # 0.375 - islands out on the lake
SHORE_FOOT = 240  # 0.625 - the near shore's trees
NEAR_FOOT = 252  # 0.8125 - the meadow in front of them
FG_BASE = H - 1

F_FAR_SHORE = factor_at(FAR_SHORE_FOOT)

# Tile widths. Each is the smallest of 688 / 1376 / 2752 that is at least
# 345 + f * (2752 - 345): the widest window the game offers (3440 / 10 + 1)
# plus everything the walk pans past it.
SKY_W = 688  # f 0.00 needs 345
CLOUD_W = 688  # drifts; laps by design
MOUNTAIN_W = 688  # f 0.031 needs 421
RIDGE_W = 688  # 0.063 needs 496
HILL_W = 688  # 0.109 needs 608
FAR_SHORE_W = 1376  # 0.156 needs 721
ISLET_W = 1376  # 0.375 needs 1248
FG_W = 1376  # 1.30: faster than the world, so it repeats at any width

LAKE_TOP = 211  # the far shoreline, straight: at this distance it is a ruled line
LAKE_BOTTOM = 234  # the near one, +-3 rows of wobble

# The sun, in the sky's own columns. The sky is f = 0, so this column is drawn
# at the window's centre plus SUN_DX whatever the resolution: the centre is
# column WC of the world, and WC is a multiple of SKY_W.
SUN_DX = 26
SUN_X = SUN_DX % SKY_W
SUN_Y = 171
SUN_R = 11


def place(wx, f, w):
    """The art column, in a layer `w` wide at factor `f`, that is on screen
    over world column `wx`."""
    return int(round(WC + f * (wx - WC))) % w


# --- periodic primitives -----------------------------------------------------


def wsin(x, cycles, w, phase=0.0):
    return math.sin(2.0 * math.pi * (cycles * x / w) + phase)


def _hash(i, seed):
    h = (i * 374761393 + seed * 668265263) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def rnd(i, seed):
    return _hash(i, seed)


def wnoise(x, cells, w, seed):
    """Value noise in 0..1 with `cells` lattice points across w, wrapping."""
    u = (x % w) * cells / float(w)
    i = int(math.floor(u))
    f = u - i
    a = _hash(i % cells, seed)
    b = _hash((i + 1) % cells, seed)
    t = f * f * (3.0 - 2.0 * f)
    return a + (b - a) * t


class Img:
    """One layer: KEY everywhere until painted. Columns wrap; rows clip."""

    def __init__(self, w, fill=KEY):
        self.w = w
        self.px = [[fill] * w for _ in range(H)]

    def put(self, x, y, c):
        if 0 <= y < H:
            self.px[y][x % self.w] = c

    def get(self, x, y):
        return self.px[y][x % self.w]

    def flat(self):
        return [self.px[y][x] for y in range(H) for x in range(self.w)]


# --- shapes ------------------------------------------------------------------
#
# Cast n Chill's trees are two shapes: a conifer of stacked saw-tooth tiers and
# a broadleaf of overlapping round lobes. Both are one body colour with a rim
# along whatever edge faces up -- the sun is low and ahead, so it catches tops,
# not sides -- and a darker colour under each tier or lobe for weight.


def pine(img, cx, foot, height, body, shade, rim, trunk, seed, rim_p=0.85):
    """A conifer as a few big overlapping triangles, Cast n Chill's way: each
    tier a skirt that is wider than the one above and tucks under it. Lit along
    the sun-side slope of every tier; shaded under the skirt away from it."""
    half = max(2, int(round(height * 0.26)))
    trunk_h = max(1, height // 8)
    crown = height - trunk_h
    n = max(2, int(round(crown / 8.0)))
    top = foot - height
    step = crown / float(n)
    tiers = []
    for i in range(n):
        ty0 = top + int(round(i * step * 0.8))
        ty1 = top + int(round((i + 1) * step))
        hw = half * (0.3 + 0.7 * (i + 1) / n)
        tiers.append((ty0, ty1, hw))
    # Bottom tier first, so each tier above overlaps the one below it.
    for i in range(n - 1, -1, -1):
        ty0, ty1, hw = tiers[i]
        rows = max(1, ty1 - ty0)
        for y in range(ty0, ty1 + 1):
            u = (y - ty0) / float(rows)
            w = (0.5 if i else 0.0) + hw * u**1.15
            wl = int(round(w + 0.6 * (rnd(y * 13 + cx, seed) - 0.5)))
            wr = int(round(w + 0.6 * (rnd(y * 17 + cx, seed + 1) - 0.5)))
            # The two rows under the skirt of the tier above are in its shadow.
            under = i > 0 and y <= tiers[i - 1][1] + 2
            for dx in range(-wl, wr + 1):
                c = body
                if under or dx < -w * 0.1 + (u - 0.5) * 2.0:
                    c = shade
                img.put(cx + dx, y, c)
            if rnd(y * 7 + cx, seed + 2) < rim_p:
                img.put(cx + wr, y, rim)
            if y == ty1 and wr > 1:
                # the skirt's lit tip
                img.put(cx + wr - 1, y, rim)
    img.put(cx, top, rim)
    for y in range(foot - trunk_h + 1, foot + 1):
        img.put(cx, y, trunk)
        if height > 30:
            img.put(cx + 1, y, trunk)


def broadleaf(img, cx, foot, height, body, deep, lit, trunk, seed, width=1.0, trunk_c=None):
    """A round crown of lobes on a short trunk. `lit` rims the top of every lobe."""
    trunk_c = trunk_c or trunk
    crown_r = height * 0.36 * width
    crown_cy = foot - height + crown_r * 0.95
    trunk_top = int(crown_cy + crown_r * 0.6)
    for y in range(trunk_top, foot + 1):
        img.put(cx, y, trunk_c)
        if height > 26:
            img.put(cx - 1, y, trunk_c)
    # Lobes: a big centre one and a ring of smaller ones round its top half.
    lobes = [(0.0, 0.0, crown_r * 0.8)]
    n = 5 + int(height / 10)
    for k in range(n):
        a = math.pi * (1.05 + 0.9 * k / max(1, n - 1)) + 0.3 * (rnd(k, seed) - 0.5)
        r = crown_r * (0.42 + 0.22 * rnd(k, seed + 1))
        d = crown_r * 0.62
        lobes.append((math.cos(a) * d * 1.15, math.sin(a) * d * 0.9, r))
    # Lobes lower down on the sides, for the round underside.
    lobes.append((-crown_r * 0.55, crown_r * 0.3, crown_r * 0.5))
    lobes.append((crown_r * 0.55, crown_r * 0.3, crown_r * 0.5))
    top_of = {}
    for ox, oy, r in lobes:
        lx0 = int(math.floor(ox - r))
        lx1 = int(math.ceil(ox + r))
        for dx in range(lx0, lx1 + 1):
            for dy in range(int(math.floor(oy - r)), int(math.ceil(oy + r)) + 1):
                if (dx - ox) ** 2 + ((dy - oy) * 1.1) ** 2 > r * r:
                    continue
                x = cx + dx
                y = int(round(crown_cy + dy))
                # Shade the lower part of each lobe, so the lobes read as lobes.
                below = (dy - oy) / r
                c = deep if below > 0.45 else body
                img.put(x, y, c)
                if y < top_of.get(dx, 10**6):
                    top_of[dx] = y
        # The lobe's own top edge catches the light.
        for dx in range(lx0, lx1 + 1):
            u = (dx - ox) / r
            if abs(u) >= 1.0:
                continue
            y = int(round(crown_cy + oy - r * math.sqrt(1.0 - u * u) / 1.1))
            if abs(u) < 0.8:
                img.put(cx + dx, y, lit)
                if abs(u) < 0.4 and rnd(dx + seed, 7) < 0.5:
                    img.put(cx + dx, y + 1, lit)


def treeline(img, foot, rows, body, rim, seed, rim_p=0.9, spire=0.25, bump=0.4):
    """A continuous distant forest: a rolling base `rows(x)` tall, toothed by
    pines and lumped by broadleaves, filled to `foot`. Returns the crest."""
    w = img.w
    crest = [foot - rows(x) for x in range(w)]
    x = 0
    k = 0
    while x < w:
        r = rnd(k, seed)
        if r < spire:
            h = 4 + int(rnd(k, seed + 1) * 7)
            hw = 1 + h // 3
            for dx in range(-hw, hw + 1):
                y = foot - rows(x + dx) - int(h * (1.0 - abs(dx) / float(hw + 1)))
                cx = (x + dx) % w
                crest[cx] = min(crest[cx], y)
            x += hw + 1 + int(rnd(k, seed + 2) * 3)
        elif r < spire + bump:
            rad = 2 + int(rnd(k, seed + 3) * 3)
            for dx in range(-rad, rad + 1):
                y = foot - rows(x + dx) - int(round(math.sqrt(rad * rad - dx * dx) * 0.8))
                cx = (x + dx) % w
                crest[cx] = min(crest[cx], y)
            x += rad + 1
        else:
            # small teeth
            h = 1 + int(rnd(k, seed + 4) * 3)
            for dx in (-1, 0, 1):
                cx = (x + dx) % w
                crest[cx] = min(crest[cx], foot - rows(x + dx) - (h if dx == 0 else h // 2))
            x += 2
        k += 1
    for x in range(w):
        for y in range(crest[x], foot + 1):
            img.put(x, y, body)
        if rnd(x, seed + 9) < rim_p:
            img.put(x, crest[x], rim)
    return crest


# --- 13 sky (opaque, 688) ----------------------------------------------------
SKY_STOPS = [  # (row where this band begins, colour)
    (0, 'wt_sky_0'),
    (70, 'wt_sky_1'),
    (118, 'wt_sky_2'),
    (148, 'wt_sky_3'),
    (166, 'wt_sky_4'),
    (180, 'wt_sky_5'),
    (192, 'wt_sky_6'),
]


def sky_band(x, y, w):
    # Stepped, wavy band edges and no dither: the edges wander two or three rows
    # so the bands read as haze, and every step is a clean pixel step.
    wob = 2.0 * wsin(x, 2, w, 0.3 + y * 0.01) + 1.2 * wsin(x, 5, w, 1.7)
    yy = y - wob
    name = SKY_STOPS[0][1]
    for r0, c in SKY_STOPS:
        if yy >= r0:
            name = c
    return name


def gen_sky():
    img = Img(SKY_W)
    for y in range(H):
        for x in range(SKY_W):
            img.px[y][x] = color_of(sky_band(x, y, SKY_W))
    # The sun: a pale disc in two flat rings, the way Cast n Chill draws one --
    # concentric bands, not a glow.
    for y in range(SUN_Y - 40, SUN_Y + 40):
        for x in range(SUN_X - 60, SUN_X + 61):
            d = math.hypot(x - SUN_X, (y - SUN_Y) * 1.05)
            if d <= SUN_R:
                img.put(x, y, color_of('wt_sun'))
            elif d <= SUN_R + 3:
                img.put(x, y, color_of('wt_sun_ring'))
            elif d <= SUN_R + 7:
                img.put(x, y, color_of('wt_sun_ring_2'))
    # A thin bright streak level with the sun, as the horizon glow.
    for x in range(SUN_X - 70, SUN_X + 71):
        if abs(x - SUN_X) > SUN_R + 7 and rnd(x, 3) < 1.0 - abs(x - SUN_X) / 70.0:
            img.put(x, SUN_Y + 4, color_of('wt_sun_ring_2'))
    return img


# --- 12 clouds (688, drifting) -----------------------------------------------
#
# Flat-bottomed cumulus in a few long banks. The sun is below them, so the
# underside is the lit side.
CLOUDS = [  # (centre x, base row, length, puff height)
    (40, 170, 90, 7),
    (200, 160, 60, 5),
    (330, 172, 110, 6),
    (470, 152, 70, 6),
    (600, 166, 80, 5),
    (120, 138, 70, 5),
    (420, 126, 90, 6),
    (560, 104, 60, 4),
    (260, 112, 80, 5),
    (660, 132, 50, 4),
]
STREAKS = [(90, 150, 70), (300, 146, 90), (520, 142, 60), (640, 182, 50), (250, 186, 40)]


def gen_clouds():
    img = Img(CLOUD_W)
    lit = color_of('wt_cloud_lit')
    body = color_of('wt_cloud')
    shade = color_of('wt_cloud_shade')
    for n, (cx, base, length, ph) in enumerate(CLOUDS):
        # Puffs along the bank, biggest in the middle.
        puffs = []
        k = 0
        x = -length // 2
        while x < length // 2:
            u = x / (length / 2.0)
            r = ph * (0.55 + 0.45 * (1.0 - u * u)) * (0.7 + 0.5 * rnd(k, 40 + n))
            puffs.append((x, r))
            x += int(max(3, r * 1.3))
            k += 1
        for px_, r in puffs:
            for dx in range(-int(r) - 1, int(r) + 2):
                for dy in range(-int(r) - 1, 1):
                    if dx * dx + (dy * 1.25) ** 2 > r * r:
                        continue
                    x = cx + px_ + dx
                    y = base + dy
                    c = body
                    # A darker crescent on the upper left of every puff.
                    if (dx + r * 0.35) ** 2 + ((dy + r * 0.35) * 1.25) ** 2 > (
                        r * 0.9
                    ) ** 2 and dx < 0:
                        c = shade
                    img.put(x, y, c)
        # The flat, lit underside.
        x0 = cx - length // 2 - 1
        x1 = cx + length // 2 + 1
        for x in range(x0, x1):
            if img.get(x, base) != KEY:
                img.put(x, base, lit)
                if img.get(x, base - 1) != KEY and rnd(x, 50 + n) < 0.6:
                    img.put(x, base - 1, lit)
    for n, (cx, row, length) in enumerate(STREAKS):
        for x in range(cx - length // 2, cx + length // 2):
            u = abs(x - cx) / (length / 2.0)
            if rnd(x, 60 + n) < 1.0 - u * u:
                img.put(x, row, color_of('wt_cloud_far'))
                if u < 0.4:
                    img.put(x + 3, row - 1, color_of('wt_cloud_far'))
    return img


# --- 08 mountains (688) ------------------------------------------------------
PEAKS = [  # (x, height above foot, half base)
    (110, 22, 34),
    (175, 44, 52),
    (250, 26, 40),
    (330, 30, 44),
    (400, 20, 30),
    (470, 40, 50),
    (545, 28, 38),
    (590, 34, 46),
    (640, 22, 30),
]


def mountain_profile(x):
    """(height, index of the peak that owns this column)."""
    best, who = 3.0, -1
    for i, (px_, ph, hb) in enumerate(PEAKS):
        d = min(abs(x - px_), MOUNTAIN_W - abs(x - px_))
        u = d / float(hb)
        if u >= 1.0:
            continue
        # Slightly concave flanks under a sharp summit: an alpine peak.
        h = ph * (1.0 - u) ** 1.25
        if h > best:
            best, who = h, i
    return best, who


def gen_mountains():
    img = Img(MOUNTAIN_W)
    lit = color_of('wt_peak')
    shade = color_of('wt_peak_shade')
    snow = color_of('wt_snow')
    snow_s = color_of('wt_snow_shade')
    rim = color_of('wt_peak_rim')
    for x in range(MOUNTAIN_W):
        h, who = mountain_profile(x)
        h += 1.4 * wnoise(x, 172, MOUNTAIN_W, 3)
        top = MOUNTAIN_FOOT - int(round(h))
        px_, ph, _hb = PEAKS[who] if who >= 0 else (x, 0, 1)
        d = (x - px_ + MOUNTAIN_W // 2) % MOUNTAIN_W - MOUNTAIN_W // 2
        # Light from the right (the sun is right of centre): the right face is lit.
        # The ridge line wanders a little off the summit's column as it falls.
        for y in range(top, MOUNTAIN_FOOT + 1):
            ridge = int(round(0.25 * (y - (MOUNTAIN_FOOT - ph)) * wsin(px_, 1, 97, 0.0)))
            face_lit = d > ridge
            c = lit if face_lit else shade
            # A cap with a ragged lower edge, and a few couloirs carrying snow
            # further down the lit face.
            summit = MOUNTAIN_FOOT - ph
            snow_line = summit + ph * 0.3 + 3.0 * wnoise(x, 172, MOUNTAIN_W, 9) - 1.5
            if face_lit and wnoise(x, 140, MOUNTAIN_W, 10) > 0.72:
                snow_line += 5
            if who >= 0 and ph > 24 and y < snow_line:
                c = snow if face_lit else snow_s
            # A shoulder line on the shaded face, one contour, for form.
            elif not face_lit and y == top + 7 + int(2 * wnoise(x, 60, MOUNTAIN_W, 11)):
                c = lit
            img.put(x, y, c)
        if who >= 0 and ph > 24:
            img.put(x, top, rim if d >= 0 else snow)
    return img


# --- 07 ridge (688) ----------------------------------------------------------
def gen_ridge():
    img = Img(RIDGE_W)

    def rows(x):
        return int(round(4 + 7 * wnoise(x, 7, RIDGE_W, 31) + 3 * wnoise(x, 19, RIDGE_W, 32)))

    treeline(
        img,
        RIDGE_FOOT,
        rows,
        color_of('wt_ridge'),
        color_of('wt_ridge_rim'),
        100,
        rim_p=0.7,
        spire=0.3,
        bump=0.3,
    )
    return img


# --- 06 hills (688) ----------------------------------------------------------
def gen_hills():
    img = Img(HILL_W)

    def rows(x):
        return int(round(2 + 6 * wnoise(x, 5, HILL_W, 41) ** 1.5 + 2 * wnoise(x, 17, HILL_W, 42)))

    treeline(
        img,
        HILL_FOOT,
        rows,
        color_of('wt_hill'),
        color_of('wt_hill_rim'),
        200,
        rim_p=0.85,
        spire=0.35,
        bump=0.35,
    )
    return img


# --- 05 far shore (1376) and 10 its reflection -------------------------------
#
# Zone E's narrows: a cliff with a waterfall, placed over world column 2420.
FALLS_WX = 2420


def far_shore_rows(x):
    return int(round(2 + 3 * wnoise(x, 23, FAR_SHORE_W, 51) + 1.5 * wnoise(x, 61, FAR_SHORE_W, 52)))


def gen_far_shore():
    img = Img(FAR_SHORE_W)
    crest = treeline(
        img,
        FAR_SHORE_FOOT,
        far_shore_rows,
        color_of('wt_shore_far'),
        color_of('wt_shore_far_rim'),
        300,
        rim_p=0.95,
        spire=0.45,
        bump=0.3,
    )
    # The cliff and the falls.
    fx = place(FALLS_WX, F_FAR_SHORE, FAR_SHORE_W)
    cliff = color_of('wt_cliff')
    cliff_s = color_of('wt_cliff_shade')
    cliff_top = FAR_SHORE_FOOT - 17
    for dx in range(-26, 27):
        u = abs(dx) / 26.0
        top = cliff_top + int(round(10 * u**2.2 + 1.2 * wnoise(dx + 1000, 40, 1376, 53)))
        x = fx + dx
        for y in range(top, FAR_SHORE_FOOT + 1):
            c = cliff_s if (y % 4 == 0 and rnd(dx + y * 3, 58) < 0.8) or dx < -18 else cliff
            img.put(x, y, c)
        img.put(x, top, color_of('wt_shore_far_rim'))
        # Pines on the clifftop.
        if rnd(dx + 500, 54) < 0.18 and abs(dx) > 3:
            pine(
                img,
                x,
                top,
                5 + int(rnd(dx, 55) * 5),
                color_of('wt_shore_far'),
                color_of('wt_shore_far'),
                color_of('wt_shore_far_rim'),
                color_of('wt_shore_far'),
                56 + dx,
            )
    falls = color_of('wt_falls')
    falls_s = color_of('wt_falls_shade')
    for y in range(cliff_top + 1, FAR_SHORE_FOOT + 1):
        for dx in (-1, 0, 1, 2):
            c = falls_s if dx == 2 or (dx == -1 and (y // 3) % 2) else falls
            img.put(fx + dx, y, c)
    # Mist at the foot.
    for dx in range(-5, 7):
        if rnd(dx, 57) < 0.7:
            img.put(fx + dx, FAR_SHORE_FOOT, falls_s)
            if abs(dx) < 4:
                img.put(fx + dx, FAR_SHORE_FOOT - 1, falls)
    img.crest = crest
    return img


REFLECT_ROW0 = LAKE_TOP
REFLECT_ROW1 = LAKE_TOP + 12


def gen_reflection(shore):
    """The far shore upside down in the water, broken into ripple rows: every
    third row is gapped where the surface tilts away. Same width and factor as
    the shore, so the pair stay aligned however the camera moves."""
    img = Img(FAR_SHORE_W)
    col = color_of('wt_reflect')
    falls = color_of('wt_falls_shade')
    for x in range(FAR_SHORE_W):
        # Height of the shore at this column, from the shore's own paint.
        top = FAR_SHORE_FOOT
        for y in range(FAR_SHORE_FOOT - 30, FAR_SHORE_FOOT + 1):
            if shore.px[y][x] != KEY:
                top = y
                break
        length = int(round((FAR_SHORE_FOOT - top + 1) * 0.7))
        for k in range(length):
            y = REFLECT_ROW0 + k
            if y >= REFLECT_ROW1:
                break
            # Thinning toward the viewer: further rows break up more.
            # Gaps run along the row, as dashes a few cells long, never as single
            # pixels: the plane stretches these rows vertically when the camera
            # climbs, and a pixel speckle stretched is a field of vertical lines.
            gap = wnoise(x, FAR_SHORE_W // 4, FAR_SHORE_W, 70 + k) < 0.25 + k / 16.0
            if gap:
                continue
            src = shore.px[FAR_SHORE_FOOT - k][x]
            img.put(x, y, falls if src in (color_of('wt_falls'), falls) else col)
    return img


# --- 11 ground plane (2752, line scrolled) -----------------------------------
DOCKS = [920, 2010]  # world columns


def lake_bottom(x):
    return LAKE_BOTTOM + int(round(2.0 * wsin(x, 5, W, 2.0) + 1.0 * wsin(x, 17, W, 0.3)))


LAKE_BANDS = [  # (rows below the far shoreline, colour): the sky, mirrored
    (0, 'wt_lake_gold'),
    (4, 'wt_lake_rose'),
    (7, 'wt_lake_violet'),
    (11, 'wt_lake_mid'),
    (15, 'wt_lake'),
    (19, 'wt_lake_deep'),
]


def gen_plane():
    img = Img(W)
    for x in range(W):
        lb = lake_bottom(x)
        for y in range(HORIZON, H):
            if y < LAKE_TOP:
                name = 'wt_shore_far'
            elif y < lb:
                d = y - LAKE_TOP + 0.8 * wsin(x, 31, W, y * 0.7) + 0.6 * wsin(x, 7, W, 1.0)
                name = LAKE_BANDS[0][1]
                for r0, c in LAKE_BANDS:
                    if d >= r0:
                        name = c
            elif y == lb:
                name = 'wt_foam'
            elif y <= lb + 1:
                name = 'wt_beach_wet'
            elif y <= lb + 4 + int(1.5 * wnoise(x, 120, W, 81)):
                name = 'wt_beach'
            elif y < 250 + int(2 * wsin(x, 9, W, 0.4)):
                name = 'wt_meadow_far'
            elif y < 258 + int(2 * wsin(x, 13, W, 1.4)):
                name = 'wt_meadow'
            else:
                name = 'wt_meadow_near'
            img.px[y][x] = color_of(name)

    # Shimmer: one-row dashes on the water, longer nearer (a ripple at depth d is
    # foreshortened in height, not width).
    for y in range(LAKE_TOP + 2, LAKE_BOTTOM + 2):
        near = (y - LAKE_TOP) / float(LAKE_BOTTOM - LAKE_TOP)
        length = 2 + int(near * 9)
        count = int(W / 688 * (14 - near * 5))
        for k in range(count):
            x0 = int(rnd(y * 131 + k, 11) * W)
            c = color_of('wt_shimmer_warm' if near < 0.35 else 'wt_shimmer')
            for dx in range(length):
                x = (x0 + dx) % W
                if y < lake_bottom(x) - 1:
                    img.px[y][x] = c

    # Grass strokes and flowers on the meadow, nearer is denser.
    for x in range(W):
        lb = lake_bottom(x)
        for y in range(lb + 6, H):
            near = (y - lb) / float(H - lb)
            r = rnd(x * 977 + y, 21)
            if r < 0.01 + 0.05 * near:
                img.px[y][x] = color_of('wt_grass_lit')
            elif r > 0.996:
                img.px[y][x] = color_of('wt_flower' if r > 0.998 else 'wt_flower_2')

    # Docks: drawn on the plane, row by row, each row at its own place for its
    # own depth -- so the dock converges toward the horizon by itself. Flat, so
    # line scroll is exactly right for it (posts would not be).
    for wx in DOCKS:
        far_row = LAKE_TOP + 11
        for y in range(far_row, LAKE_BOTTOM + 4):
            f = factor_at(y + 0.5)
            cx = WC + f * (wx - WC)
            half = max(1.0, 4.5 * f)
            x0 = int(math.floor(cx - half))
            x1 = int(math.ceil(cx + half))
            for x in range(x0, x1 + 1):
                if y == far_row:
                    c = 'wt_dock_lit'
                elif (y - far_row) % 2 == 1:
                    c = 'wt_dock_gap'
                else:
                    c = 'wt_dock'
                if x in (x0, x1) and c == 'wt_dock':
                    c = 'wt_dock_lit'
                img.put(x, y, color_of(c))
    return img


# --- 09 glint (688, factor 0, on the plane) ----------------------------------
GLINT_ROW0 = LAKE_TOP + 1
GLINT_ROW1 = LAKE_BOTTOM - 3


def gen_glint():
    img = Img(SKY_W)
    hot = color_of('wt_glint')
    dim = color_of('wt_glint_dim')
    for y in range(GLINT_ROW0, GLINT_ROW1):
        near = (y - GLINT_ROW0) / float(GLINT_ROW1 - GLINT_ROW0)
        if (y - GLINT_ROW0) % 2 == 1 and near > 0.3:
            continue
        spread = 5 + int(near * 22)
        for k in range(3 + int(near * 4)):
            off = int((rnd(y * 31 + k, 41) - 0.5) * 2 * spread)
            length = 2 + int(rnd(y * 7 + k, 42) * (3 + near * 7))
            for dx in range(length):
                img.put(SUN_X + off + dx - length // 2, y, hot if abs(off) < spread * 0.45 else dim)
    return img


# --- 04 islets (1376) --------------------------------------------------------
ISLETS = [  # (world column, half width, height, trees)
    (700, 14, 4, ((-4, 12, 'p'), (3, 9, 'p'), (8, 6, 'p'))),
    (1320, 22, 5, ((-9, 15, 'p'), (-3, 19, 'p'), (4, 11, 'b'), (11, 8, 'p'))),
    (1560, 8, 3, ((1, 9, 'p'),)),
    (2150, 16, 4, ((-5, 10, 'b'), (4, 14, 'p'))),
]
CANOE_WX = 1450


def gen_islets():
    img = Img(ISLET_W)
    f = factor_at(ISLET_FOOT)
    body = color_of('wt_islet')
    rim = color_of('wt_islet_rim')
    for n, (wx, half, ht, trees) in enumerate(ISLETS):
        cx = place(wx, f, ISLET_W)
        for dx in range(-half, half + 1):
            u = dx / float(half)
            h = int(round(ht * (1.0 - u * u) ** 0.6 + 0.6 * rnd(dx + n * 50, 90)))
            top = ISLET_FOOT - max(0, h)
            for y in range(top, ISLET_FOOT + 1):
                img.put(cx + dx, y, body)
            if h > 0:
                img.put(cx + dx, top, rim)
        for k, (ox, th, kind) in enumerate(trees):
            u = ox / float(half)
            base = ISLET_FOOT - int(round(ht * (1.0 - u * u) ** 0.6)) + 1
            if kind == 'p':
                pine(img, cx + ox, base, th, body, body, rim, body, 91 + n * 10 + k, rim_p=0.6)
            else:
                broadleaf(img, cx + ox, base, th, body, body, rim, body, 95 + n * 10 + k)
    # A canoe with someone fishing in it, out on the water between the islands.
    cx = place(CANOE_WX, f, ISLET_W)
    canoe = color_of('wt_canoe')
    for dx in range(-5, 6):
        img.put(cx + dx, ISLET_FOOT - 1, canoe)
        if abs(dx) < 4:
            img.put(cx + dx, ISLET_FOOT, body)
    img.put(cx - 6, ISLET_FOOT - 2, canoe)
    img.put(cx + 6, ISLET_FOOT - 2, canoe)
    for y in range(ISLET_FOOT - 5, ISLET_FOOT - 1):
        img.put(cx - 1, y, body)
    img.put(cx - 1, ISLET_FOOT - 6, rim)  # hat brim catching the sun
    img.put(cx - 2, ISLET_FOOT - 6, body)
    img.put(cx, ISLET_FOOT - 6, body)
    # The rod: a fine line out over the water.
    for k in range(9):
        img.put(cx + k, ISLET_FOOT - 5 - k // 2, rim if k > 6 else body)
    for y in range(ISLET_FOOT - 8, ISLET_FOOT - 1):
        if y % 2 == 0:
            img.put(cx + 9, y, color_of('wt_shimmer'))
    return img


# --- 03 shore (2752, foot 240) and 02 near (2752, foot 252) ------------------
#
# The walk, in world columns:
#
#     0 -  520   pinewood camp: dense pines, the tent and the fire
#   520 - 1150   autumn shore: maples and birches, the first dock
#  1150 - 1750   the open tarn: low shore, islands and the canoe in view
#  1750 - 2250   cabin cove: the cabin with its windows lit, the second dock
#  2250 - 2752   rocky narrows: boulders, pines, the waterfall across the water
ZONES = [
    (0, 520, 'camp'),
    (520, 1150, 'autumn'),
    (1150, 1750, 'open'),
    (1750, 2250, 'cabin'),
    (2250, W, 'narrows'),
]


def zone_at(wx):
    for a, b, z in ZONES:
        if a <= wx < b:
            return z
    return ZONES[-1][2]


# What each zone plants on the shore layer, per world column it walks: how
# likely a pine, a broadleaf, a bush, a rock or a reed clump is at each stride.
SHORE_MIX = {  # zone: (stride, pine, broadleaf, birch, bush, rock, reeds)
    'camp': (5, 0.70, 0.08, 0.02, 0.10, 0.00, 0.05),
    'autumn': (6, 0.22, 0.42, 0.14, 0.10, 0.00, 0.08),
    'open': (9, 0.12, 0.10, 0.04, 0.22, 0.04, 0.30),
    'cabin': (6, 0.50, 0.20, 0.05, 0.12, 0.03, 0.05),
    'narrows': (6, 0.48, 0.06, 0.02, 0.06, 0.30, 0.04),
}
NEAR_MIX = {  # sparser and bigger
    'camp': (18, 0.62, 0.10, 0.00, 0.14, 0.00),
    'autumn': (24, 0.22, 0.44, 0.12, 0.16, 0.00),
    'open': (28, 0.08, 0.12, 0.04, 0.26, 0.06),
    'cabin': (20, 0.42, 0.26, 0.04, 0.16, 0.04),
    'narrows': (24, 0.34, 0.06, 0.00, 0.10, 0.36),
}


def bush(img, cx, foot, r, body, rim, seed):
    for dx in range(-r - 1, r + 2):
        for dy in range(-r, 1):
            u = dx / float(r + 1)
            if dy < -r * (1.0 - u * u) ** 0.5 - 0.2 * rnd(dx, seed):
                continue
            img.put(cx + dx, foot + dy, body)
        top = foot - int(r * (1.0 - (dx / float(r + 1)) ** 2) ** 0.5)
        if abs(dx) <= r and rnd(dx + 31, seed) < 0.75:
            img.put(cx + dx, top, rim)


def rock(img, cx, foot, w, h, body, shade, rim, seed):
    for dx in range(-w, w + 1):
        u = dx / float(w + 0.5)
        top = foot - int(round(h * max(0.0, 1.0 - u * u) ** 0.5 + 0.6 * rnd(dx + seed, 33)))
        for y in range(top, foot + 1):
            img.put(cx + dx, y, shade if dx < -w // 3 or y > foot - h // 3 else body)
        img.put(cx + dx, top, rim)


def reeds(img, cx, foot, n, body, rim, seed):
    for k in range(n):
        x = cx + k * 2 - n
        h = 5 + int(rnd(k, seed) * 7)
        lean = rnd(k, seed + 1) - 0.5
        for j in range(h):
            img.put(int(round(x + lean * j * j / h)), foot - j, body)
        tip = int(round(x + lean * h))
        if rnd(k, seed + 2) < 0.5:
            for j in range(h - 4, h - 1):
                img.put(tip, foot - j, rim)
                img.put(tip + 1, foot - j, body)


def plant_walk(img, foot, f, mix, scale, palette, seed, skip=()):
    """Walk the world left to right, planting per the zone's mix at each stride.
    `skip` is a list of (world x0, x1) kept clear for a set piece."""
    wx = 4
    k = 0
    while wx < W - 4:
        zone = zone_at(wx)
        stride, p_pine, p_leaf, p_birch, p_bush, p_rock = mix[zone][:6]
        p_reed = mix[zone][6] if len(mix[zone]) > 6 else 0.0
        x = place(wx, f, img.w)
        clear = any(a <= wx <= b for a, b in skip)
        r = rnd(k, seed)
        size = 0.75 + 0.5 * rnd(k, seed + 1)
        if clear:
            pass
        elif r < p_pine:
            pine(
                img,
                x,
                foot,
                int(scale * 24 * size),
                palette['pine'],
                palette['pine_shade'],
                palette['pine_rim'],
                palette['trunk'],
                seed + k,
            )
        elif r < p_pine + p_leaf:
            body, deep, lit = palette['leaf'][int(rnd(k, seed + 2) * len(palette['leaf']))]
            broadleaf(
                img, x, foot, int(scale * 22 * size), body, deep, lit, palette['trunk'], seed + k
            )
        elif r < p_pine + p_leaf + p_birch:
            body, deep, lit = palette['leaf'][-1]
            broadleaf(
                img,
                x,
                foot,
                int(scale * 26 * size),
                body,
                deep,
                lit,
                palette['trunk'],
                seed + k,
                width=0.8,
                trunk_c=palette['birch'],
            )
        elif r < p_pine + p_leaf + p_birch + p_bush:
            bush(
                img,
                x,
                foot,
                max(2, int(scale * 3.5 * size)),
                palette['bush'],
                palette['bush_rim'],
                seed + k,
            )
        elif r < p_pine + p_leaf + p_birch + p_bush + p_rock:
            rock(
                img,
                x,
                foot,
                max(2, int(scale * 5 * size)),
                max(2, int(scale * 4 * size)),
                palette['rock'],
                palette['rock_shade'],
                palette['rock_rim'],
                seed + k,
            )
        elif r < p_pine + p_leaf + p_birch + p_bush + p_rock + p_reed:
            reeds(
                img,
                x,
                foot,
                3 + int(rnd(k, seed + 3) * 4),
                palette['bush'],
                palette['bush_rim'],
                seed + k,
            )
        wx += max(2, int(stride * (0.6 + 0.8 * rnd(k, seed + 4)) / max(0.3, f)))
        k += 1


def palettes():
    shore = {
        'pine': color_of('wt_pine_mid'),
        'pine_shade': color_of('wt_pine_mid_shade'),
        'pine_rim': color_of('wt_pine_mid_rim'),
        'trunk': color_of('wt_trunk'),
        'leaf': [
            (color_of('wt_autumn'), color_of('wt_autumn_deep'), color_of('wt_autumn_gold')),
            (color_of('wt_autumn_deep'), color_of('wt_autumn_shade'), color_of('wt_autumn')),
            (color_of('wt_autumn_gold'), color_of('wt_autumn'), color_of('wt_flower_2')),
        ],
        'birch': color_of('wt_birch'),
        'bush': color_of('wt_bush'),
        'bush_rim': color_of('wt_bush_rim'),
        'rock': color_of('wt_rock'),
        'rock_shade': color_of('wt_rock_shade'),
        'rock_rim': color_of('wt_rock_rim'),
    }
    near = {
        'pine': color_of('wt_pine_near'),
        'pine_shade': color_of('wt_pine_near_shade'),
        'pine_rim': color_of('wt_pine_near_rim'),
        'trunk': color_of('wt_trunk'),
        'leaf': [
            (
                color_of('wt_maple_near'),
                color_of('wt_maple_near_deep'),
                color_of('wt_maple_near_lit'),
            ),
            (
                color_of('wt_maple_near_deep'),
                color_of('wt_autumn_shade'),
                color_of('wt_maple_near'),
            ),
            (color_of('wt_autumn_gold'), color_of('wt_maple_near'), color_of('wt_flower_2')),
        ],
        'birch': color_of('wt_birch'),
        'bush': color_of('wt_pine_near'),
        'bush_rim': color_of('wt_pine_near_rim'),
        'rock': color_of('wt_rock'),
        'rock_shade': color_of('wt_rock_shade'),
        'rock_rim': color_of('wt_rock_rim'),
    }
    return shore, near


def gen_shore():
    img = Img(W)
    shore, _ = palettes()
    plant_walk(
        img,
        SHORE_FOOT,
        factor_at(SHORE_FOOT),
        SHORE_MIX,
        1.0,
        shore,
        1000,
        skip=[(d - 30, d + 30) for d in DOCKS],
    )
    return img


# --- set pieces on the near layer --------------------------------------------
TENT_WX = 150
FIRE_WX = 196
LANTERN_WX = [880, 1980]
CABIN_WX = 1900
BENCH_WX = 1450
WOODPILE_WX = 1945


def tent(img, cx, foot):
    canvas = color_of('wt_canvas')
    shade = color_of('wt_canvas_shade')
    rim = color_of('wt_window')
    h = 13
    for dy in range(h):
        y = foot - dy
        half = int(round((h - dy) * 1.25))
        for dx in range(-half, half + 1):
            c = canvas if dx > 0 else shade
            img.put(cx + dx, y, c)
        img.put(cx + half, y, rim if dy > 2 else canvas)
    # The door, glowing: someone left a lamp on inside.
    for dy in range(6):
        for dx in range(-2 + dy // 3, 3 - dy // 3):
            img.put(cx + dx, foot - dy, color_of('wt_window_glow') if dy < 5 else shade)
    img.put(cx, foot - h, color_of('wt_trunk'))
    img.put(cx, foot - h - 1, color_of('wt_trunk'))


def campfire(img, cx, foot):
    for dx in range(-4, 5):
        img.put(cx + dx, foot, color_of('wt_stone'))
    for dx in range(-3, 4):
        img.put(cx + dx, foot - 1, color_of('wt_log'))
    img.put(cx - 4, foot - 1, color_of('wt_stone'))
    img.put(cx + 4, foot - 1, color_of('wt_stone'))
    flame = [
        (0, 2, 'wt_fire_core'),
        (-1, 2, 'wt_fire'),
        (1, 2, 'wt_fire'),
        (0, 3, 'wt_fire_core'),
        (-1, 3, 'wt_fire'),
        (1, 3, 'wt_ember'),
        (0, 4, 'wt_fire'),
        (0, 5, 'wt_fire'),
        (-1, 4, 'wt_ember'),
        (1, 5, 'wt_ember'),
        (-2, 2, 'wt_ember'),
        (2, 2, 'wt_ember'),
        (0, 6, 'wt_ember'),
        (2, 7, 'wt_ember'),
    ]
    for dx, dy, c in flame:
        img.put(cx + dx, foot - dy, color_of(c))
    # Two logs to sit on.
    for side in (-1, 1):
        for dx in range(5):
            img.put(cx + side * (8 + dx), foot, color_of('wt_log'))
            img.put(cx + side * (8 + dx), foot - 1, color_of('wt_log_lit'))
        img.put(cx + side * 8 if side < 0 else cx + side * 12, foot - 1, color_of('wt_log_end'))


def lantern_post(img, cx, foot):
    for dy in range(16):
        img.put(cx, foot - dy, color_of('wt_log'))
    for dx in range(0, 4):
        img.put(cx + dx, foot - 16, color_of('wt_log'))
    for dy in range(1, 4):
        img.put(cx + 3, foot - 16 + dy, color_of('wt_window') if dy > 1 else color_of('wt_log'))
        img.put(
            cx + 4, foot - 16 + dy, color_of('wt_window_glow') if dy > 1 else color_of('wt_log')
        )
    img.put(cx + 2, foot - 13, color_of('wt_window_glow'))


def cabin(img, cx, foot):
    log = color_of('wt_log')
    log_lit = color_of('wt_log_lit')
    end = color_of('wt_log_end')
    roof = color_of('wt_roof')
    roof_lit = color_of('wt_roof_lit')
    half = 17
    wall_h = 13
    # Log walls: alternating courses, log ends showing at the corners.
    for dy in range(wall_h):
        y = foot - dy
        for dx in range(-half, half + 1):
            img.put(cx + dx, y, log_lit if dy % 3 == 2 else log)
        if dy % 3 != 1:
            img.put(cx - half - 1, y, end)
            img.put(cx + half + 1, y, end)
    # Roof: a steep gable with an overhang, rim-lit along its ridge.
    roof_h = 13
    for dy in range(roof_h + 1):
        y = foot - wall_h - dy
        w = half + 4 - int(round(dy * (half + 4) / float(roof_h + 1)))
        for dx in range(-w, w + 1):
            img.put(cx + dx, y, roof_lit if dx > w - 2 or dy == roof_h else roof)
        img.put(cx - w, y, roof_lit)
    # Windows, lit warm, with a glow sill under each.
    for wx_ in (-10, 7):
        for dy in range(4, 10):
            for dx in range(0, 5):
                c = color_of('wt_window')
                if dx == 2 or dy == 7:
                    c = log
                img.put(cx + wx_ + dx, foot - dy, c)
        for dx in range(-1, 6):
            img.put(cx + wx_ + dx, foot - 3, color_of('wt_window_glow'))
    # Door.
    for dy in range(0, 9):
        for dx in range(-2, 3):
            img.put(cx + dx, foot - dy, roof if dx != 2 else log_lit)
    img.put(cx + 1, foot - 4, color_of('wt_window'))
    # Chimney, stone, on the sun side.
    for dy in range(wall_h + 5, wall_h + roof_h + 3):
        for dx in (10, 11, 12):
            img.put(cx + dx, foot - dy, color_of('wt_stone'))
    img.put(cx + 12, foot - wall_h - roof_h - 3, color_of('wt_rock_rim'))
    # Smoke: a few soft puffs drifting right, getting fainter.
    for k, (dx, dy, r) in enumerate(((11, 30, 2), (13, 35, 2), (16, 40, 3), (20, 46, 3))):
        for ox in range(-r, r + 1):
            for oy in range(-r, r + 1):
                if ox * ox + oy * oy <= r * r and rnd(ox * 7 + oy + k * 31, 77) < 0.75 - k * 0.12:
                    img.put(cx + dx + ox, foot - dy + oy, color_of('wt_cloud_shade'))


def woodpile(img, cx, foot):
    for row in range(4):
        for k in range(5 - row // 2):
            x = cx + k * 3 + (row % 2)
            y = foot - row * 2
            img.put(x, y, color_of('wt_log_end'))
            img.put(x + 1, y, color_of('wt_log_lit'))
            img.put(x, y - 1, color_of('wt_log_lit'))
            img.put(x + 1, y - 1, color_of('wt_log'))


def bench(img, cx, foot):
    for dx in range(-6, 7):
        img.put(cx + dx, foot - 4, color_of('wt_log_lit'))
        img.put(cx + dx, foot - 8, color_of('wt_log'))
    for dy in range(0, 9):
        img.put(cx - 5, foot - dy, color_of('wt_log'))
        img.put(cx + 5, foot - dy, color_of('wt_log'))


def gen_near():
    img = Img(W)
    _, near = palettes()
    f = factor_at(NEAR_FOOT)
    keep_clear = [
        (TENT_WX - 30, FIRE_WX + 30),
        (CABIN_WX - 45, WOODPILE_WX + 25),
        (BENCH_WX - 25, BENCH_WX + 25),
    ] + [(x - 12, x + 12) for x in LANTERN_WX]
    plant_walk(img, NEAR_FOOT, f, NEAR_MIX, 2.0, near, 2000, skip=keep_clear)
    tent(img, place(TENT_WX, f, W), NEAR_FOOT)
    campfire(img, place(FIRE_WX, f, W), NEAR_FOOT)
    for wx in LANTERN_WX:
        lantern_post(img, place(wx, f, W), NEAR_FOOT)
    cabin(img, place(CABIN_WX, f, W), NEAR_FOOT)
    woodpile(img, place(WOODPILE_WX, f, W) + 6, NEAR_FOOT)
    bench(img, place(BENCH_WX, f, W), NEAR_FOOT)
    return img


# --- 01 foreground (1376, factor 1.30) ---------------------------------------
#
# Grass tufts and ferns as near-black silhouettes along the bottom edge, rim-lit
# warm on top. Sparse, and short: this layer is in front of the player and
# crosses the window faster than the world.
def gen_foreground():
    img = Img(FG_W)
    body = color_of('wt_fg')
    rim = color_of('wt_fg_rim')
    x = 0
    k = 0
    while x < FG_W:
        kind = rnd(k, 801)
        if kind < 0.55:
            # A grass tuft: blades fanning out of one root.
            n = 4 + int(rnd(k, 802) * 5)
            h = 10 + int(rnd(k, 803) * 14)
            for j in range(n):
                lean = (j - n / 2.0) / n * 1.6
                bh = int(h * (0.6 + 0.4 * rnd(j + k * 9, 804)))
                for t in range(bh):
                    bx = x + int(round(lean * t * t / float(bh)))
                    img.put(bx, FG_BASE - t, rim if t == bh - 1 else body)
                    if t < bh // 3:
                        img.put(bx + 1, FG_BASE - t, body)
        else:
            # A fern: a curved stem with leaflets down both sides.
            h = 16 + int(rnd(k, 805) * 14)
            lean = (rnd(k, 806) - 0.5) * 1.2
            for t in range(h):
                sx = x + int(round(lean * t * t / float(h)))
                img.put(sx, FG_BASE - t, body)
                if t > 3 and t % 2 == 0:
                    leaf = int((h - t) * 0.35) + 1
                    for d in range(1, leaf + 1):
                        img.put(sx - d, FG_BASE - t + d // 2, body)
                        img.put(sx + d, FG_BASE - t + d // 2, body)
                    img.put(sx - leaf, FG_BASE - t + leaf // 2, rim)
                    img.put(sx + leaf, FG_BASE - t + leaf // 2, rim)
        x += 30 + int(rnd(k, 807) * 70)
        k += 1
    return img


# --- the stack, back to front ------------------------------------------------
def build_layers():
    shore_far = gen_far_shore()
    return [
        ('13_sky', gen_sky()),
        ('12_clouds', gen_clouds()),
        ('11_plane', gen_plane()),
        ('10_reflection', gen_reflection(shore_far)),
        ('09_glint', gen_glint()),
        ('08_mountains', gen_mountains()),
        ('07_ridge', gen_ridge()),
        ('06_hills', gen_hills()),
        ('05_far_shore', shore_far),
        ('04_islets', gen_islets()),
        ('03_shore', gen_shore()),
        ('02_near', gen_near()),
        ('01_foreground', gen_foreground()),
    ]


# --- the playable scene ------------------------------------------------------
#
# The terrain's surface is CONTACT, where the plane's factor reaches 1.00, with
# features along the walk. Every feature is material, so all of it can be dug,
# drained, burned or brought down: the bridge is Wood over real Water.
SCENE_SAND = 0xEEDD82
SCENE_WATER = 0x4444FF
SCENE_WOOD = 0x6B4423

# (left, right, height): rises above the floor, built of earth (Wall).
MOUNDS = [(0, 36, 18), (300, 430, 7), (1290, 1470, 12), (2180, 2420, 11), (2716, W, 20)]
# (left, right, depth): water-filled dips.
PONDS = [(560, 700, 9), (1700, 1790, 6)]
BRIDGE = (548, 712)  # the plank bridge over the first pond, one row above the floor
SANDBANKS = [(900, 1080, 5)]  # a beach to dig
ROCKS = [(1330, 1430, 9)]  # the knoll's stone cap


def rgb(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def bump(x, left, right, height):
    if not left <= x <= right:
        return 0
    u = (x - left) / float(right - left) * 2.0 - 1.0
    return int(round(height * (1.0 - u * u) ** 0.7))


def surface(x):
    h = 0
    for left, right, ht in MOUNDS:
        h = max(h, bump(x, left, right, ht))
    # A slow roll, so the floor is never quite a ruler.
    h += int(round(1.2 * wsin(x, 9, W, 0.5) + 0.8 * wsin(x, 23, W, 1.1) + 1.0))
    return CONTACT - h


def pond_bottom(x):
    for left, right, depth in PONDS:
        if left <= x <= right:
            return CONTACT + bump(x, left, right, depth)
    return None


def scene_cell(x, y):
    """(legend colour, albedo colour) for the cell, or None for air."""
    pb = pond_bottom(x)
    top = surface(x) if pb is None else CONTACT
    if pb is not None and CONTACT <= y < pb:
        return rgb(SCENE_WATER), color_of('wt_lake')
    bl, br = BRIDGE
    if bl <= x <= br and y == CONTACT - 1:
        return rgb(SCENE_WOOD), color_of('wt_ground_plank' if x % 4 else 'wt_ground_plank_2')
    if bl <= x <= br and y == CONTACT - 2 and (x - bl) % 18 == 0:
        return rgb(SCENE_WOOD), color_of('wt_ground_plank_2')  # a post
    for left, right, ht in SANDBANKS:
        if left <= x <= right and top - bump(x, left, right, ht) <= y < top:
            alt = (x * 7 + y * 3) % 5 == 0
            return rgb(SCENE_SAND), color_of('wt_ground_sand_2' if alt else 'wt_ground_sand')
    if y < top:
        return None
    if pb is not None and y < pb:
        return None
    depth = y - top
    for left, right, ht in ROCKS:
        if left <= x <= right and depth < bump(x, left, right, ht):
            # Bare stone, lit along its top, flecked, darker toward its base.
            if depth == 0:
                return LEGEND_WALL, color_of('wt_rock_rim')
            alt = depth > bump(x, left, right, ht) * 0.6 or rnd(x * 53 + y, 124) < 0.12
            return LEGEND_WALL, color_of('wt_ground_rock_2' if alt else 'wt_ground_rock')
    if depth == 0:
        alb = color_of('wt_ground_lit')
    elif depth < 3 or (depth == 3 and rnd(x, 120) < 0.5):
        alb = color_of('wt_ground_grass')
    elif depth < 10 + int(3 * wnoise(x, 172, W, 121)):
        alb = color_of('wt_ground_dirt')
        if rnd(x * 31 + y, 122) < 0.03:
            alb = color_of('wt_ground_rock')  # a pebble
    else:
        alb = color_of('wt_ground_deep')
        if rnd(x * 31 + y, 123) < 0.025:
            alb = color_of('wt_ground_dirt')
    return LEGEND_WALL, alb


def write_scene():
    assert_legend_matches_header()
    mat, alb = [], []
    for y in range(H):
        for x in range(W):
            cell = scene_cell(x, y)
            if cell is None:
                mat.append(LEGEND_EMPTY)
                alb.append(LEGEND_EMPTY)
            else:
                mat.append(cell[0])
                alb.append(cell[1])
    write_bmp(os.path.join(REPO_ROOT, 'assets', f'{NAME}_material.bmp'), W, H, mat)
    write_bmp(os.path.join(REPO_ROOT, 'assets', f'{NAME}_albedo.bmp'), W, H, alb)
    return alb


def stage(path):
    for d in BUILD_DST_DIRS:
        if os.path.isdir(os.path.dirname(d)):
            os.makedirs(d, exist_ok=True)
            shutil.copyfile(path, os.path.join(d, os.path.basename(path)))


def composite_standing(layers, scene_alb):
    """A flat preview of the whole world as the standing camera sees it, every
    layer at its own factor -- each column is what is behind that world column
    when the camera is centred on it. Not what any one frame shows; a map of the
    walk, for the README."""
    out = [KEY] * (W * H)
    for tag, img in layers:
        if tag == '09_glint':
            continue  # only ever under the sun, so not a property of a column
        if tag in ('11_plane',):
            for y in range(HORIZON, H):
                f = max(0.0, min(1.0, factor_at(y + 0.5)))
                for wx in range(W):
                    c = img.get(place(wx, f, img.w), y)
                    if c != KEY:
                        out[y * W + wx] = c
            continue
        f = {
            '13_sky': 0.0,
            '12_clouds': 0.01,
            '08_mountains': factor_at(MOUNTAIN_FOOT),
            '07_ridge': factor_at(RIDGE_FOOT),
            '06_hills': factor_at(HILL_FOOT),
            '10_reflection': F_FAR_SHORE,
            '05_far_shore': F_FAR_SHORE,
            '04_islets': factor_at(ISLET_FOOT),
            '03_shore': factor_at(SHORE_FOOT),
            '02_near': factor_at(NEAR_FOOT),
            '01_foreground': 1.3,
        }[tag]
        for wx in range(W):
            x = place(wx, f, img.w)
            for y in range(H):
                c = img.px[y][x]
                if c != KEY:
                    out[y * W + wx] = c
    for i, c in enumerate(scene_alb):
        if c != LEGEND_EMPTY:
            out[i] = c
    return out


def main():
    os.makedirs(DST_DIR, exist_ok=True)
    layers = build_layers()
    for tag, img in layers:
        path = os.path.join(DST_DIR, f'{NAME}_{tag}.bmp')
        write_bmp(path, img.w, H, img.flat())
        stage(path)
        feet = max((y for y in range(H) for x in range(img.w) if img.px[y][x] != KEY), default=-1)
        print(
            f'wrote {os.path.basename(path):<34} {img.w:4d} wide, lowest painted row {feet:3d} '
            f'(factor {factor_at(feet):.3f})'
        )
    alb = write_scene()
    write_png(os.path.join(DST_DIR, 'composite_preview.png'), W, H, composite_standing(layers, alb))
    print(f'wrote {NAME}_material.bmp, {NAME}_albedo.bmp, composite_preview.png')


if __name__ == '__main__':
    main()
