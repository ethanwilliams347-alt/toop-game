"""Generates bg_tarn: a mountain lake at golden hour, for the perspective rig.

    python tools/generate_bg_tarn.py

Writes assets/bg_tarn/ (eleven layers, a composite preview and a README) and
assets/bg_tarn_material.bmp / bg_tarn_albedo.bmp (the playable scene).

Inspired by art_src/Background_1 (Ethan's bg1): the same family of places -- a
teal mountain range, a ladder of sand and red-rock hills with a gold rim line, a
still lake, an olive meadow -- moved to the hour when the sun is low behind the
range, so every far value leans toward the sky and every lit edge toward gold.

What is different about this set is not the paint, it is the contract the paint
signs with the renderer. render/depth_rig.h explains it in full; the part that
binds this file is:

  1. Every layer that stands on the ground has ONE foot row, and that row IS its
     parallax factor. The rig is a pinhole camera: a row of the ground plane at
     `r` scrolls at (r - HORIZON) / (CONTACT - HORIZON), and anything standing on
     row r has to scroll at the same rate or its feet slide. So nothing here
     chooses a factor. It chooses where a thing stands, and the factor falls out.
     render/rig_backdrop.h carries the foot rows; rig_test reads each BMP and
     checks that its lowest painted row is exactly that number.

  2. The ground plane is not banded. It is drawn one row at a time, each row at
     its own factor (line scroll -- what the SNES did with HDMA), so it has no
     cut to hide and needs no flat runs. That frees the plane to carry detail at
     every depth: shoreline, reflections, grass.

  3. Every layer tiles left to right. All horizontal structure comes from
     functions periodic in W (integer-frequency sines, a value-noise lattice that
     wraps), so column W-1 runs straight into column 0. Wrapping is what lets a
     factor exceed 1.0 -- the foreground here is 1.30 -- which a world-sized,
     non-wrapping layer can never do without gapping at the world's edge.

Deterministic and standard-library-only, like every generator in tools/: two
runs produce byte-identical files, so a diff in assets/bg_tarn/ is a real change
to this script. Every colour is a PALETTE name; validate_palette.py comes back
clean on every layer.
"""

import math
import os
import shutil
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, 'tools'))
from pixel_art import (LEGEND_EMPTY, LEGEND_WALL, COLOR_KEY,  # noqa: E402
                       assert_legend_matches_header, bayer_threshold, color_of,
                       dither_mix, write_bmp, write_png)

DST_DIR = os.path.join(REPO_ROOT, 'assets', 'bg_tarn')
BUILD_DST_DIRS = [os.path.join(REPO_ROOT, 'build', cfg, 'assets', 'bg_tarn')
                  for cfg in ('Release', 'Debug', '')]

W = 688
H = 288
KEY = COLOR_KEY

# --- the rig ---------------------------------------------------------------
#
# These two rows are the camera. render/rig_backdrop.h states the same two
# numbers; rig_test checks the plane BMP is transparent above HORIZON and opaque
# from it down, which is the half of that agreement the art can be held to.
#
# HORIZON is where the plane vanishes (factor 0). CONTACT is where it meets the
# playable world (factor 1), and it is the terrain's surface row, because the
# world is the one thing in the frame that is locked to the camera at 1.00.
HORIZON = 200
CONTACT = 264
FLOOR_ROW = CONTACT


def factor_at(row):
    return (row - HORIZON) / float(CONTACT - HORIZON)


# Where each standing layer's feet are. A factor is printed next to each so the
# depth ladder is readable here, but it is derived, never typed.
MOUNTAIN_FOOT = 202   # 0.031 - the far range, almost on the horizon
RIDGE_FOOT = 204      # 0.063
MESA_FOOT = 207       # 0.109
DUNE_FOOT = 210       # 0.156 - the far shore of the lake
PINE_FAR_FOOT = 240   # 0.625 - the near shore
PINE_NEAR_FOOT = 252  # 0.813 - the meadow in front of it

LAKE_TOP = 211        # far shoreline, +-1 row of wobble
LAKE_BOTTOM = 236     # near shoreline, +-2 rows of wobble

SUN_X = 362
SUN_Y = 176
SUN_R = 8


# --- periodic primitives -----------------------------------------------------
#
# Everything horizontal is built from these two, which is the whole of how the
# layers tile: a sine with an integer number of cycles across W, and a value-noise
# lattice whose last cell interpolates back into its first.

def wsin(x, cycles, phase=0.0):
    return math.sin(2.0 * math.pi * (cycles * x / W) + phase)


def _hash(i, seed):
    # Integer hash, the usual xorshift-multiply mix. Stated here rather than taken
    # from `random` so the stream cannot change under a Python upgrade.
    h = (i * 374761393 + seed * 668265263) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    return ((h ^ (h >> 16)) & 0xFFFF) / 65535.0


def wnoise(x, cells, seed):
    """Value noise in 0..1 with `cells` lattice points across W, wrapping."""
    u = (x % W) * cells / float(W)
    i = int(math.floor(u))
    f = u - i
    a = _hash(i % cells, seed)
    b = _hash((i + 1) % cells, seed)
    t = f * f * (3.0 - 2.0 * f)
    return a + (b - a) * t


def wdist(a, b):
    """Horizontal distance on the wrapped strip."""
    d = abs((a - b) % W)
    return min(d, W - d)


def rnd(i, seed):
    return _hash(i, seed)


def blank():
    return [[KEY] * W for _ in range(H)]


def sun_closeness(x, reach):
    """1 at the sun's column, 0 at `reach` columns away - how lit a rim is."""
    return max(0.0, 1.0 - wdist(x, SUN_X) / float(reach))


# --- 11 sky (opaque) ---------------------------------------------------------
#
# bg1's sky is flat bands with soft, wavy edges rather than a smooth ramp, and
# that is kept: each stop is a band, and only the few rows between two bands are
# dithered. The bands are anchored to HORIZON, the plane's own top edge, for the
# reason bg_forest's README records -- anchored lower, the ramp is still passing
# through the mountains' value when it reaches them and the range disappears.
SKY_STOPS = [  # (row the band is fully reached, colour)
    (40, 'tarn_sky_top'),
    (120, 'tarn_sky_high'),
    (160, 'tarn_sky_mid'),
    (184, 'tarn_sky_low'),
    (198, 'tarn_sky_horizon'),
]


def sky_colour(x, y):
    # A slow wobble on the band edges so they read as layers of haze rather than
    # as a ruled gradient.
    yy = y + 3.0 * wsin(x, 3, 0.4 * y / 40.0) + 1.5 * wsin(x, 7, 1.3)
    if yy <= SKY_STOPS[0][0]:
        return color_of(SKY_STOPS[0][1])
    for (r0, c0), (r1, c1) in zip(SKY_STOPS, SKY_STOPS[1:]):
        if yy < r1:
            # Flat for the first part of the interval, dithered for the last 12
            # rows: banding, not a ramp.
            span = r1 - r0
            t = (yy - (r1 - min(12, span))) / float(min(12, span))
            return dither_mix(x, y, color_of(c0), color_of(c1), max(0.0, min(1.0, t)))
    return color_of(SKY_STOPS[-1][1])


def gen_sky():
    img = blank()
    halo = color_of('tarn_sun_halo')
    sun = color_of('tarn_sun')
    for y in range(H):
        for x in range(W):
            c = sky_colour(x, y)
            dx = wdist(x, SUN_X)
            d = math.hypot(dx, (y - SUN_Y) * 1.15)
            if d <= SUN_R:
                c = sun
            elif d <= SUN_R + 2:
                c = halo
            elif d <= SUN_R + 14:
                # The halo thins outward through the ordered dither, so it is
                # still only two colours on the edge of the disc.
                t = 1.0 - (d - SUN_R - 2) / 12.0
                c = dither_mix(x, y, c, halo, t * t * 0.9)
            # A horizontal warm streak through the sun - the sky's brightest
            # band is level with it, not a circle around it.
            elif abs(y - (SUN_Y + 6)) <= 1 and dx < 70:
                c = dither_mix(x, y, c, halo, 0.6 * (1.0 - dx / 70.0))
            img[y][x] = c
    return img


# --- 10 clouds (drifting) ----------------------------------------------------
#
# Long flat stratus, bg1's cloud language, with a lit underside where the sun is
# under them. Drawn on their own layer so the renderer can drift it with time --
# the one thing in the frame that moves when the camera does not.
#
# Most of them sit low, rows 168..186, in the band a standing camera sees above
# the peaks; the rest are higher up for the flight. A cloud above row 164 is only
# ever on screen when the player is in the air.
CLOUDS = [  # (centre x, row, half length, thickness)
    (40, 176, 110, 3), (250, 170, 70, 2), (380, 184, 120, 3), (560, 174, 90, 3),
    (140, 186, 80, 2), (470, 168, 60, 2), (640, 150, 60, 2), (300, 132, 90, 2),
    (520, 118, 100, 3), (60, 104, 70, 2),
]


def gen_clouds():
    img = blank()
    lit = color_of('tarn_cloud_lit')
    body = color_of('tarn_cloud')
    shade = color_of('tarn_cloud_shade')
    for n, (cx, cy, half, th) in enumerate(CLOUDS):
        for x in range(W):
            d = wdist(x, cx)
            if d > half:
                continue
            # Lens profile: thickest in the middle, a puff or two on top.
            edge = 1.0 - (d / float(half)) ** 2
            top = th * edge + 1.6 * wnoise(x, 40, 70 + n) * edge
            rows = int(round(top))
            if rows <= 0:
                continue
            warm = sun_closeness(x, 220)
            for k in range(rows + 1):
                y = cy - k
                if k == 0:
                    # The underside: lit warm toward the sun, the body colour away.
                    img[y][x] = dither_mix(x, y, body, lit, min(1.0, 0.25 + warm))
                elif k == rows:
                    img[y][x] = dither_mix(x, y, body, shade, 0.5)
                else:
                    img[y][x] = body
    return img


# --- 09 ground plane (line scrolled) -----------------------------------------
#
# Rows HORIZON..H, opaque everywhere, transparent above. Laid out as it is seen
# from the anchor camera -- far rows compressed, near rows large -- because the
# renderer draws each row at its own depth and never resamples rows, so the
# perspective has to be in the paint.
def lake_top(x):
    return LAKE_TOP + int(round(1.2 * wsin(x, 5, 0.7) + 0.6 * wsin(x, 13)))


def lake_bottom(x):
    return LAKE_BOTTOM + int(round(2.2 * wsin(x, 3, 2.0) + 1.0 * wsin(x, 8, 0.3)))


def gen_plane():
    img = blank()
    shore_far = color_of('tarn_shore_far')
    m_far = color_of('tarn_meadow_far')
    lake_sky = color_of('tarn_lake_sky')
    lake = color_of('tarn_lake')
    deep = color_of('tarn_lake_deep')
    warm = color_of('tarn_lake_warm')
    meadow = color_of('tarn_meadow')
    near = color_of('tarn_meadow_near')
    lit = color_of('tarn_meadow_lit')
    flower = color_of('tarn_flower')

    for x in range(W):
        lt = lake_top(x)
        lb = lake_bottom(x)
        # Flat bands with wavy edges, and no ordered dither anywhere in the plane.
        # The plane is the one layer the rig stretches vertically -- up to 2.9x with
        # the camera at the ceiling -- and a dither stretched in one axis turns into
        # vertical stripes. A wavy edge stretched is just a taller wave.
        far_edge = HORIZON + 6 + int(round(1.5 * wsin(x, 11, 0.5)))
        sky_edge = lt + 2 + int(round(0.7 * wsin(x, 17, 1.1) + 0.5))
        deep_edge = lt + 12 + int(round(2.0 * wsin(x, 6, 2.4) + wsin(x, 19)))
        seam = lb + 10 + int(round(1.5 * wsin(x, 9, 0.9) + 0.8 * wsin(x, 23)))
        for y in range(HORIZON, H):
            if y < lt:
                c = shore_far if y < far_edge else m_far
            elif y < lb:
                # Water: the reflected sky at the far edge going to deep water at
                # the near one - a lake is a mirror at a grazing angle far away and
                # a window into itself up close.
                c = lake_sky if y < sky_edge else (lake if y < deep_edge else deep)
            elif y == lb:
                c = lit  # the waterline catches the light
            else:
                c = meadow if y < seam else near
            img[y][x] = c

    # Reflection streaks: one-row dashes, longer nearer, because a ripple at
    # depth d is foreshortened by 1/d in height and not in width. Warm toward the
    # sun's column, sky-coloured away from it.
    for y in range(LAKE_TOP + 1, LAKE_BOTTOM + 3):
        nearness = (y - LAKE_TOP) / float(LAKE_BOTTOM - LAKE_TOP)
        length = 2 + int(nearness * 8)
        count = 12 - int(nearness * 5)
        for k in range(count):
            x0 = int(rnd(y * 131 + k, 11) * W)
            for dx in range(length):
                x = (x0 + dx) % W
                if not (lake_top(x) < y < lake_bottom(x)):
                    continue
                c = warm if sun_closeness(x, 150) > rnd(k, 12) * 0.9 else lake_sky
                img[y][x] = c

    # Grass on the near meadow: short strokes, more and taller nearer, with the
    # odd flower. Below CONTACT the terrain covers the plane, so it stops there.
    for x in range(W):
        lb = lake_bottom(x)
        for y in range(lb + 2, CONTACT):
            nearness = (y - lb) / float(CONTACT - lb)
            r = rnd(x * 977 + y, 21)
            if r < 0.015 + 0.06 * nearness:
                img[y][x] = lit
                if nearness > 0.55 and y - 1 > lb:
                    img[y - 1][x] = lit
            elif r > 0.995:
                img[y][x] = flower
    return img


# --- 08 sun glint (on the water, factor 0) -----------------------------------
#
# The sun's reflection is not on the lake, it is in the lake's mirror, so it
# stays under the sun however the camera moves. It is its own layer at the sun's
# factor (0), drawn over the plane and rippled sideways per row by the renderer.
GLINT_TOP = LAKE_TOP + 1
GLINT_BOTTOM = LAKE_BOTTOM - 2


def gen_glint():
    img = blank()
    hot = color_of('tarn_glint')
    dim = color_of('tarn_glint_dim')
    for y in range(GLINT_TOP, GLINT_BOTTOM):
        nearness = (y - GLINT_TOP) / float(GLINT_BOTTOM - GLINT_TOP)
        if (y - GLINT_TOP) % 2 == 1 and nearness > 0.25:
            continue  # gaps between ripples open up as they come closer
        spread = 4 + int(nearness * 26)
        for k in range(3 + int(nearness * 5)):
            off = int((rnd(y * 31 + k, 41) - 0.5) * 2 * spread)
            length = 2 + int(rnd(y * 7 + k, 42) * (3 + nearness * 8))
            for dx in range(length):
                x = SUN_X + off + dx - length // 2
                if not (lake_top(x) < y < lake_bottom(x)):
                    continue
                img[y][x % W] = hot if abs(off) < spread * 0.4 else dim
    return img


# --- 07 far mountains --------------------------------------------------------
#
# bg1's teal range, hazed toward the sky, with snow and a rim of sunlight along
# the crest near the sun. A saddle sits under the sun so it sets into the range
# rather than behind a peak.
PEAKS = [  # (x, height above foot, slope)
    (20, 28, 0.95), (95, 21, 0.8), (170, 32, 1.05), (240, 18, 0.7), (305, 27, 0.9),
    (428, 19, 0.65), (496, 22, 0.75), (545, 31, 1.0), (618, 24, 0.85),
]

# The summits top out at row 170, six rows under where a standing 1080p camera's
# window begins (view-y 163.8), so the peaks are in the frame the player actually
# sees rather than only in the one they fly up to.


def mountain_form(x):
    h = 6.0
    for px, ph, sl in PEAKS:
        h = max(h, ph - wdist(x, px) * sl)
    return MOUNTAIN_FOOT - h


def mountain_crest(x):
    h = MOUNTAIN_FOOT - mountain_form(x)
    h += 3.0 * wnoise(x, 172, 3) + 1.5 * wnoise(x, 344, 4)  # crags
    return MOUNTAIN_FOOT - int(round(h))


def paint_massif(img, crest, foot, body, shade, rim, rim_reach, snow=None,
                 snow_shade=None, snow_line=None, ridge_lines=(), form=None):
    form = form or crest
    for x in range(W):
        top = crest(x)
        # Which way the slope faces, read off `form` -- the crest without its
        # crags -- over nine columns. Read off the crest itself, every crag flips
        # a column between lit and shaded and the face turns to vertical stripes.
        # Flat colours either side, the way bg1 paints a face; no dither inside a
        # mass, because a dithered field at this size reads as noise, not shade.
        rising = form(x + 4) - form(x - 4)  # rows: positive means falling right
        facing_sun = (rising > 0) == (x < SUN_X) if wdist(x, SUN_X) > 4 else True
        for y in range(top, foot + 1):
            c = body if facing_sun else shade
            if snow and y < snow_line(x) and y - top < 9:
                c = snow if facing_sun else snow_shade
            img[y][x] = c
        if rim_reach and rnd(x, 5) < sun_closeness(x, rim_reach) * 1.4:
            img[top][x] = rim
    # Ridge lines: a one-cell darker line from each peak down its shaded side,
    # the way bg1 draws its rock faces.
    for px, ph, sl in ridge_lines:
        direction = -1 if px > SUN_X else 1
        y = crest(px) + 1
        x = px
        while y < foot - 2:
            if img[y][x % W] != KEY:
                img[y][x % W] = shade
            y += 1
            if (y + px) % 2 == 0:
                x += direction
    return img


def gen_mountains():
    img = blank()
    return paint_massif(
        img, mountain_crest, MOUNTAIN_FOOT,
        color_of('tarn_peak'), color_of('tarn_peak_shade'), color_of('tarn_peak_rim'),
        150, color_of('tarn_snow'), color_of('tarn_snow_shade'),
        lambda x: 184 + int(3 * wnoise(x, 86, 9)), ridge_lines=PEAKS,
        form=mountain_form)


# --- 06 ridge (grey-green foothills) -----------------------------------------
def ridge_form(x):
    # Rolling, not jagged: these are bg1's grey-green foothills. A dip under the
    # sun so the foothills do not cut it.
    h = 5.0 + 11.0 * wnoise(x, 9, 31) + 3.0 * wnoise(x, 23, 32)
    h -= 9.0 * sun_closeness(x, 70)
    return RIDGE_FOOT - max(2.0, h)


def ridge_crest(x):
    return int(round(ridge_form(x) - 0.8 * wnoise(x, 172, 33)))


def gen_ridge():
    # Not paint_massif's lit/shaded faces. Rolling hills have no faces to speak of,
    # and splitting them by slope direction turns a gentle hill into a row of
    # vertical walls. bg1 shades its hills with contour lines instead -- a second
    # crest line a few rows under the first, following its shape -- and so does
    # this: a lit cap, a contour, and the shaded body under it.
    img = blank()
    body = color_of('tarn_ridge')
    shade = color_of('tarn_ridge_shade')
    rim = color_of('tarn_ridge_rim')
    for x in range(W):
        top = ridge_crest(x)
        contour = top + 3 + int(round(2.0 * wnoise(x, 21, 34)))
        for y in range(top, RIDGE_FOOT + 1):
            img[y][x] = body if y < contour else (rim if y == contour else shade)
        if rnd(x, 5) < 0.85:
            img[top][x] = rim
    return img


# --- 05 mesas (red banded rock) ----------------------------------------------
MESAS = [  # (left, right, height)
    (40, 96, 13), (180, 214, 9), (272, 306, 14), (520, 590, 11), (626, 660, 8),
]


def gen_mesas():
    img = blank()
    body = color_of('tarn_mesa')
    band = color_of('tarn_mesa_band')
    shade = color_of('tarn_mesa_shade')
    lit = color_of('tarn_mesa_lit')
    for n, (l, r, ht) in enumerate(MESAS):
        top = MESA_FOOT - ht
        for x in range(l - 6, r + 7):
            # Steep talus either side of a flat cap.
            if x < l:
                t = top + (l - x) * 2
            elif x > r:
                t = top + (x - r) * 2
            else:
                t = top + int(round(0.8 * wnoise(x, 172, 50 + n)))
            for y in range(t, MESA_FOOT + 1):
                c = band if (y - top) % 4 == 2 else body
                if x > r - 3 if x < SUN_X else x < l + 3:
                    c = shade  # the face away from the sun
                img[y][x % W] = c
            if l <= x <= r:
                img[t][x % W] = lit
    return img


# --- 04 dunes (sand hills on the far shore) ----------------------------------
DUNES = [(10, 48, 7), (120, 70, 10), (230, 40, 6), (330, 60, 9), (520, 50, 8),
         (600, 64, 11)]


def gen_dunes():
    img = blank()
    body = color_of('tarn_dune')
    shade = color_of('tarn_dune_shade')
    rim = color_of('tarn_dune_rim')
    for cx, half, ht in DUNES:
        for dx in range(-half, half + 1):
            x = (cx + dx) % W
            u = dx / float(half)
            h = ht * (1.0 - u * u) ** 0.8
            if h < 0.5:
                continue
            top = DUNE_FOOT - int(round(h))
            for y in range(top, DUNE_FOOT + 1):
                # Shade on the far side of the crest, one dithered column where the
                # two faces meet so the turn reads as round rather than folded.
                # The terminator leans with depth into the dune, so the shade wraps
                # round the hump instead of folding it down the middle.
                side = dx if cx < SUN_X else -dx
                img[y][x] = shade if side > -(y - top) * 0.9 + half * 0.15 else body
            img[top][x] = rim
    return img


# --- 03 / 02 pines -----------------------------------------------------------
def paint_pine(img, base_x, foot, height, body, shade, rim, trunk):
    half = max(3, int(height * 0.28))
    trunk_h = max(2, height // 7)
    tiers = max(3, height // 6)
    canopy_top = foot - height
    canopy_bottom = foot - trunk_h
    for y in range(canopy_top, canopy_bottom + 1):
        # Tiers: the width saws back in at each tier boundary, the classic pine.
        t = (y - canopy_top) / float(max(1, canopy_bottom - canopy_top))
        tier_t = (t * tiers) % 1.0
        w = half * (0.15 + 0.85 * t) * (0.65 + 0.35 * tier_t)
        wl = int(round(w + 0.6 * (rnd(y * 13 + base_x, 61) - 0.5)))
        wr = int(round(w + 0.6 * (rnd(y * 17 + base_x, 62) - 0.5)))
        for dx in range(-wl, wr + 1):
            x = (base_x + dx) % W
            sun_side = (dx > 0) == (base_x < SUN_X)
            edge = dx == -wl or dx == wr
            c = body
            if not sun_side and dx != 0:
                c = dither_mix(x, y, body, shade, 0.65)
            if edge and sun_side and rnd(y * 3 + x, 63) < 0.75:
                c = rim
            img[y][x] = c
    img[canopy_top][base_x % W] = rim
    for y in range(canopy_bottom, foot + 1):
        img[y][base_x % W] = trunk
        if height > 24:
            img[y][(base_x + 1) % W] = trunk


PINES_FAR = [  # (x, height)
    (14, 14), (22, 11), (31, 16), (118, 12), (126, 17), (133, 13), (141, 10),
    (205, 15), (214, 12), (282, 10), (290, 13), (520, 16), (529, 12), (537, 18),
    (546, 13), (610, 11), (618, 15), (660, 13), (668, 17), (676, 12),
]

PINES_NEAR = [
    (60, 30), (74, 40), (88, 26), (236, 34), (250, 24), (268, 42), (575, 38),
    (590, 28), (650, 44),
]


def gen_pines_far():
    img = blank()
    for x, h in PINES_FAR:
        paint_pine(img, x, PINE_FAR_FOOT, h, color_of('tarn_pine_far'),
                   color_of('tarn_pine_far'), color_of('tarn_pine_far_rim'),
                   color_of('tarn_pine_far'))
    return img


def gen_pines_near():
    img = blank()
    for x, h in PINES_NEAR:
        paint_pine(img, x, PINE_NEAR_FOOT, h, color_of('tarn_pine'),
                   color_of('tarn_pine_shade'), color_of('tarn_pine_rim'),
                   color_of('tarn_trunk'))
    return img


# --- 01 foreground (reeds, in front of the player) ---------------------------
#
# Sparse on purpose: this layer covers the player, and at 1.30 it crosses the
# window fast, so a little of it reads as depth and a lot of it reads as clutter.
REEDS = [(30, 30), (36, 22), (44, 34), (300, 26), (306, 38), (313, 20),
         (470, 28), (478, 36), (640, 24), (647, 32)]
FG_BASE = H - 1


def gen_foreground():
    img = blank()
    body = color_of('tarn_fg')
    rim = color_of('tarn_fg_rim')
    for n, (x0, h) in enumerate(REEDS):
        lean = (rnd(n, 81) - 0.5) * 0.5
        for k in range(h):
            y = FG_BASE - k
            x = int(round(x0 + lean * k * k / float(h)))
            img[y][x % W] = rim if (x0 < SUN_X and k % 5 == 0) else body
            img[y][(x + 1) % W] = body
            if k < h // 3:
                img[y][(x - 1) % W] = body
        # A cattail head two cells below the tip, rim-lit on the sun's side.
        tip_x = int(round(x0 + lean * h))
        for k in range(h - 8, h - 2):
            y = FG_BASE - k
            for dx in (-1, 0, 1, 2):
                lit_side = (dx == 2) == (x0 < SUN_X)
                img[y][(tip_x + dx) % W] = rim if lit_side and dx in (-1, 2) else color_of('tarn_trunk')
    return img


# --- the stack, back to front -----------------------------------------------
LAYERS = [
    ('11_sky', gen_sky),
    ('10_clouds', gen_clouds),
    ('09_plane', gen_plane),
    ('08_glint', gen_glint),
    ('07_mountains', gen_mountains),
    ('06_ridge', gen_ridge),
    ('05_mesas', gen_mesas),
    ('04_dunes', gen_dunes),
    ('03_pines_far', gen_pines_far),
    ('02_pines_near', gen_pines_near),
    ('01_reeds', gen_foreground),
]


# --- the playable scene -------------------------------------------------------
#
# A flat floor at CONTACT, because CONTACT is where the plane's factor reaches
# 1.00 and the world is the plane's continuation. On it: a pond in a dip (real
# Water cells, so it can be drained, boiled or dug into) and a sand bank to dig.
SCENE_SAND = 0xEEDD82
SCENE_WATER = 0x4444FF
POND = (96, 152, 7)        # left, right, depth below the floor
SAND_BANK = (520, 596, 5)  # left, right, height above the floor


def rgb(v):
    return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)


def scene_cell(x, y):
    """(legend colour, albedo colour) for the cell, or None for air."""
    pl, pr, pd = POND
    if pl <= x <= pr:
        u = (x - pl) / float(pr - pl) * 2.0 - 1.0
        bottom = CONTACT + int(round(pd * (1.0 - u * u)))
        if CONTACT < y <= bottom - 1 or (y == CONTACT and abs(u) < 0.92):
            return rgb(SCENE_WATER), color_of('water_fill')
    sl, sr, sh = SAND_BANK
    if sl <= x <= sr and y < CONTACT:
        u = (x - sl) / float(sr - sl) * 2.0 - 1.0
        if y >= CONTACT - int(round(sh * (1.0 - u * u))):
            return rgb(SCENE_SAND), dither_mix(x, y, color_of('sand_lit'),
                                                 color_of('sand_mid'), 0.4)
    if y < CONTACT:
        return None
    if pl <= x <= pr:
        u = (x - pl) / float(pr - pl) * 2.0 - 1.0
        if y <= CONTACT + int(round(pd * (1.0 - u * u))) - 1:
            return None
    depth = y - CONTACT
    if depth == 0:
        alb = color_of('tarn_meadow_lit')
    elif depth < 4:
        alb = dither_mix(x, y, color_of('tarn_meadow_near'), color_of('dirt_mid'),
                         depth / 4.0)
    else:
        alb = dither_mix(x, y, color_of('dirt_mid'), color_of('dirt_fill'),
                         min(1.0, (depth - 4) / 10.0))
    return LEGEND_WALL, alb


def write_scene(composite):
    assert_legend_matches_header()
    mat, alb = [], []
    for y in range(H):
        for x in range(W):
            cell = scene_cell(x, y)
            if cell is None:
                mat.append(LEGEND_EMPTY)
                alb.append(composite[y * W + x])
            else:
                mat.append(cell[0])
                alb.append(cell[1])
    write_bmp(os.path.join(REPO_ROOT, 'assets', 'bg_tarn_material.bmp'), W, H, mat)
    write_bmp(os.path.join(REPO_ROOT, 'assets', 'bg_tarn_albedo.bmp'), W, H, alb)
    return alb


def stage(path):
    for d in BUILD_DST_DIRS:
        if os.path.isdir(os.path.dirname(d)):
            os.makedirs(d, exist_ok=True)
            shutil.copyfile(path, os.path.join(d, os.path.basename(path)))


def main():
    os.makedirs(DST_DIR, exist_ok=True)
    composite = [KEY] * (W * H)
    for tag, fn in LAYERS:
        img = fn()
        flat = [img[y][x] for y in range(H) for x in range(W)]
        path = os.path.join(DST_DIR, 'bg_tarn_%s.bmp' % tag)
        write_bmp(path, W, H, flat)
        stage(path)
        for i, c in enumerate(flat):
            if c != KEY:
                composite[i] = c
        feet = max((y for y in range(H) for x in range(W) if img[y][x] != KEY), default=-1)
        print('wrote %-28s lowest painted row %3d (factor %.3f)'
              % (os.path.basename(path), feet, factor_at(feet)))
    alb = write_scene(composite)
    write_png(os.path.join(DST_DIR, 'composite_preview.png'), W, H, alb)
    print('wrote bg_tarn_material.bmp, bg_tarn_albedo.bmp, composite_preview.png')


if __name__ == '__main__':
    main()
