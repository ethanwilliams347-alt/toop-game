"""Generates the backdrop parallax layers: sky and stars, a mountain silhouette,
and the receding ground plane (sky/stars and mountain silhouettes, plus the plane).

This file is the single source of the parallax factors, and --header is how the
C++ side gets them:

    python tools/generate_backdrop.py --header

writes src/render/backdrop_layers.h from the table below, the same way
tools/player_sheet.py --header writes src/render/player_sprite.h. Do not edit
that header -- it is overwritten work. Keeping the factors in both languages
with a comment asking a human to grep before changing one is not enforcement:
the failure it describes is a seam at the pan limit, invisible until somebody
pans to the edge of the world. The header carries each layer's generated size
as well as its factors, so main.cpp can compare what it loaded against what
this script would have produced and say so at startup.

One asset set covers all three display modes, and which mode sizes it is the
counter-intuitive part. The window is switchable at runtime at a fixed number
of screen pixels per cell, so a wider window sees more cells and therefore has
less world left to pan across. The largest pan range -- the case a layer can
run out of image in -- belongs to the smallest window, and the widest window
needs the largest window-sized base. So the layers are sized from the smallest
viewport and the largest window dimensions, taken independently, and every
other mode uses a sub-rectangle of the same file.

A consequence of that formula worth knowing before adding a band: the nearer
the band, the bigger its file. Size grows with the parallax factor, so a near
band is the most expensive layer in the stack and not the cheapest. --sizes
prints the table. A wrapping layer retires the relationship entirely.

Run from the repo root:
    python tools/generate_backdrop.py            # the shipped layers
    python tools/generate_backdrop.py --header   # regenerate the C++ header
    python tools/generate_backdrop.py --sizes    # what each layer would cost
"""
import random
import sys
from pixel_art import (PALETTE, COLOR_KEY, asset_path, bayer_threshold,
                       src_path, write_bmp)


# --- banded ramps ----------------------------------------------------------
#
# N flat tones rather than two-colour dithering, and the distinction looks like
# the dithering rule being broken until you count the colours.
#
# dither_mix picks between exactly two colours, so a "ten band" gradient built
# out of it contains two colours and no more -- the bands are ten different
# proportions of the same pair. That works while the pair is far apart and
# collapses when it is not: on a ramp between two near values the proportion
# saturates part-way down, and the rest of the gradient is flat.
#
# The rule is "transitions between flat tones, not a per-pixel smooth blend",
# and ten flat tones with dithered hand-offs is still that. What it is not is
# two flat tones, which is what the rule had quietly become.
def band_tone(a, b, t):
    """One flat tone t of the way from a to b. Not per-pixel: a whole band."""
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def banded_ramp(x, y, height, a, b, bands, dither_rows=2):
    """Colour at (x, y) of a vertical ramp of `bands` flat tones from a to b,
    with the last `dither_rows` rows of each band ordered-dithered into the next
    so the seam is a stepped hand-off rather than a hard line."""
    band = min(y * bands // height, bands - 1)
    this = band_tone(a, b, band / (bands - 1))
    if band == bands - 1:
        return this
    next_start = (band + 1) * height // bands
    if next_start - y <= dither_rows:
        nxt = band_tone(a, b, (band + 1) / (bands - 1))
        share = 1.0 - (next_start - y) / (dither_rows + 1)
        return nxt if bayer_threshold(x, y) < share else this
    return this

# --- must match main.cpp -----------------------------------------------
GRID_WIDTH, GRID_HEIGHT = 1920, 1080
SCALE = 4  # Camera::SCALE

# main.cpp's DisplayMode table, in window pixels.
MODES = [(1920, 1080), (2560, 1440), (3440, 1440)]

PARALLAX_SKY = (0.04, 0.02)  # (x, y) factors
PARALLAX_MOUNTAIN = (0.15, 0.06)

# --- the ground plane ---------------------------------------------------
#
# Two factors, because a receding plane has no single depth. The plane is drawn
# as horizontal strips, each at its own factor, interpolated between the far edge
# (the horizon) and the near one; see plane_strip() in
# src/render/backdrop_wrap.h for the relation and render/frame.cpp for the draw.
#
# Both numbers are stated derivations and not measurements, because the
# reference cannot supply a parallax factor at all -- its frames are separately
# generated scenes rather than one camera pan. Parallax is inverse depth, so the
# defensible construction is a geometric ladder between the two factors this
# project already ships: 0.04 -> 0.08 -> 0.15 -> 0.28 -> 0.52 -> 1.00, a ratio
# of about 1.9. It lands the existing mountains on 0.15 without moving them,
# which is a check on the construction rather than a coincidence to lean on. The
# plane occupies the two rungs between the mountains and the world.
#
# The y factor follows the ratio the two existing layers already use between
# their axes (roughly 0.4), so the plane's horizon drifts with the camera rather
# than being glued to the window.
PARALLAX_GROUND_FAR = (0.28, 0.11)
PARALLAX_GROUND_NEAR_X = 0.52

# The ground plane is a wrapping layer, so this is a tile size and not an image
# size, and it must never be run through layer_size(). That function encodes the
# relationship wrapping layers exist to retire -- window plus the whole pan range
# at the layer's own factor -- and at this factor it would price the band at tens
# of megabytes. A tile has no relationship to the pan range at all:
# render/frame.cpp asks backdrop_wrap::wrap_axis how many copies the window needs
# and issues that many.
#
# The width is a compromise the strip loop makes visible: narrower means more
# SDL_RenderCopy calls per strip per frame, wider means the repeat is easier to
# see. The height is the plane's whole depth -- the tile is sampled top to bottom
# exactly once across the band, never wrapped vertically, because its rows are
# the recession.
GROUND_TILE = (256, 256)

# There was briefly a mid-ground band here, between the mountains and the world,
# because reference frames carry most of their depth there. It came out on the
# check its own reasoning asked for: a simulated world already fills that band
# with terrain, where a hand-painted one cannot.
#
# Do not re-add one without checking the same way. The reopen trigger is a
# location whose terrain does not fill the band: a flatter scene with a lower
# horizon, or a zoomed-out camera. Neither exists today.
# -------------------------------------------------------------------------

# Padded viewport, in cells: VIEWPORT + 1 on each axis, matching main.cpp's
# PADDED_WIDTH/HEIGHT. The smallest of each is what maximises the pan range.
MIN_VIEWPORT_W = min(w // SCALE for w, _ in MODES) + 1
MIN_VIEWPORT_H = min(h // SCALE for _, h in MODES) + 1

# The largest window is what sets the base size every layer must at least cover;
# the smallest viewport is what sets how far it then has to scroll.
WINDOW_W = max(w for w, _ in MODES)
WINDOW_H = max(h for _, h in MODES)

MAX_SHIFT_X = (GRID_WIDTH - MIN_VIEWPORT_W) * SCALE
MAX_SHIFT_Y = (GRID_HEIGHT - MIN_VIEWPORT_H) * SCALE


def layer_size(factor):
    fx, fy = factor
    # A margin beyond the exact shift range so a float rounding error never
    # exposes a one-pixel gap at the pan limit.
    w = WINDOW_W + int(MAX_SHIFT_X * fx) + 8
    h = WINDOW_H + int(MAX_SHIFT_Y * fy) + 8
    return w, h


# --- sky: banded gradient, ordered-dithered transitions, sparse stars ------
def generate_sky():
    w, h = layer_size(PARALLAX_SKY)
    top = PALETTE['sky_deep']
    bottom = PALETTE['sky_horizon']

    # Ten flat tones with dithered hand-offs -- see banded_ramp above for why
    # this is not dither_mix between the endpoints.
    #
    # The ramp runs bright to dark downward. Running it the other way puts the
    # frame's brightest row immediately above the row that is supposed to be its
    # darkest pinch.
    bands = 10
    pixels = [None] * (w * h)
    for y in range(h):
        for x in range(w):
            pixels[y * w + x] = banded_ramp(x, y, h, top, bottom, bands)

    rng = random.Random(7)
    star = PALETTE['star']
    for _ in range(70):
        sx = rng.randrange(w)
        sy = rng.randrange(int(h * 0.65))  # stars stay above the skyline band
        pixels[sy * w + sx] = star

    write_bmp(asset_path('backdrop_sky.bmp'), w, h, pixels)
    print(f'wrote assets/backdrop_sky.bmp ({w}x{h})')


# --- mountains: jagged silhouette, colour-keyed, drawn onto the sky's tone -
#
# The two constants below are a composition, not a texture. The silhouette has
# to sit in the frame's upper middle so there is room below it for a plane at
# all: the plane's BMP is opaque with no colour key and its layer row is drawn
# after the mountains', so a horizon authored above the peaks paints over the
# whole band and the mountains are covered rather than faint.
#
# That is only half the fix. The other half is in render/frame.cpp, which
# derives the plane's horizon from MOUNTAINS_SKYLINE_MAX_ROW instead of a
# free-floating window fraction, so the two can no longer be authored into a
# contradiction.
MOUNTAIN_BASE_FRACTION = 0.31
MOUNTAIN_PEAK_FRACTION = 0.14
MOUNTAIN_SEGMENT_W = 48


# The skyline, as one row per column. Hoisted out of generate_mountains because
# the header generator needs it too -- the plane's horizon is derived from this
# curve's deepest row, and a second copy of the walk in the header path is the
# duplicated-constant failure the generated header exists to retire. It is a pure
# function of its seed, so both callers get the same curve without either of them
# having to open the BMP.
def mountain_skyline(w, h):
    rng = random.Random(3)
    base_y = int(h * MOUNTAIN_BASE_FRACTION)
    peak_span = int(h * MOUNTAIN_PEAK_FRACTION)

    # A jagged skyline as a random walk between segment points, the same
    # seeded, blocky-rather-than-smooth texture the scene generator uses --
    # deliberate variation, not noise.
    points = []
    x = 0
    while x <= w:
        points.append((x, base_y + rng.randint(-peak_span, peak_span // 2)))
        x += MOUNTAIN_SEGMENT_W + rng.randint(-10, 10)
    points.append((w, points[-1][1]))

    rows = []
    seg = 0
    for px in range(w):
        while seg < len(points) - 2 and px > points[seg + 1][0]:
            seg += 1
        x0, y0 = points[seg]
        x1, y1 = points[seg + 1]
        t = 0 if x1 == x0 else (px - x0) / (x1 - x0)
        rows.append(int(round(y0 + (y1 - y0) * t)))
    return rows


def generate_mountains():
    w, h = layer_size(PARALLAX_MOUNTAIN)
    mountain = PALETTE['mountain']
    rim = PALETTE['mountain_rim']

    rows = mountain_skyline(w, h)
    pixels = [COLOR_KEY] * (w * h)
    for px in range(w):
        sy = rows[px]
        for py in range(max(sy, 0), h):
            pixels[py * w + px] = rim if py < sy + 2 else mountain

    write_bmp(asset_path('backdrop_mountains.bmp'), w, h, pixels)
    print(f'wrote assets/backdrop_mountains.bmp ({w}x{h}) - '
          f'skyline rows {min(rows)}..{max(rows)}')


# --- ground plane: a tile whose rows are distance, not height --------------
#
# The art is cheap on purpose: one ramp and one mark colour. What makes it read
# as ground is the strip loop's geometry, not the painting.
#
# Two things are baked into the tile and cannot be anywhere else:
#
# 1. The value ramp, far edge at the top to near edge at the bottom. The plane is
#    the one band that reads as receding within itself, which is where the
#    into-the-page effect comes from, and no count of flat layers buys it. A
#    per-layer Grade multiplies uniformly and cannot produce it.
# 2. The marks. They are uniform in the tile, which is uniform in world
#    distance, so the strip loop compresses them toward the horizon and magnifies
#    them toward the viewer for free.
#
# The horizon edge is the darkest row in the frame, deliberately: the reference's
# row-mean luminance bottoms out at the waterline. ground_far graded lands under
# both the sky and the graded mountains, so the dark pinch at the horizon
# survives the water going away.
def generate_ground():
    w, h = GROUND_TILE
    far = PALETTE['ground_far']
    near = PALETTE['ground_near']
    mark = PALETTE['ground_mark']

    # Flat bands with dithered hand-offs, not a per-pixel smooth blend. Ten
    # bands, matching the sky's, so the two graded surfaces are made of the same
    # size of step -- ten distinct tones, not ten proportions of two.
    bands = 10
    pixels = [None] * (w * h)
    for y in range(h):
        for x in range(w):
            pixels[y * w + x] = banded_ramp(x, y, h, far, near, bands)

    # Horizontal dashes -- the ground's texture, and the thing whose apparent
    # width the strip loop varies. Every dash wraps in x, because this is a
    # tiling texture and a dash clipped at the right edge is a hard vertical
    # seam repeating across the whole band at every tile boundary.
    #
    # The marks are weighted toward the near edge and start below MARK_START.
    # The tile's rows are world distance, so the strip loop compresses its top
    # rows into a handful of screen rows near the horizon; a mark up there is
    # point-sampled many to one, which is a speckle that flickers as the camera
    # moves rather than texture. It is also the wrong end of the frame for it:
    # contrast should grow with nearness, and the horizon row should be the
    # frame's clean dark pinch.
    MARK_START = 0.30
    rng = random.Random(11)
    placed = 0
    for _ in range(1200):
        my = rng.randrange(h)
        depth = my / (h - 1)
        if depth < MARK_START:
            continue
        # Rejection weighting: keep a mark with probability rising from 0 at
        # MARK_START to 1 at the near edge.
        if rng.random() > (depth - MARK_START) / (1.0 - MARK_START):
            continue
        mx = rng.randrange(w)
        length = rng.randint(3, 11)
        for i in range(length):
            pixels[my * w + (mx + i) % w] = mark
        placed += 1

    write_bmp(asset_path('backdrop_ground.bmp'), w, h, pixels)
    print(f'wrote assets/backdrop_ground.bmp ({w}x{h}) - a tile, not a '
          f'pan-sized layer; {placed} marks, all below row {int(h * MARK_START)}')


# --- the generated header --------------------------------------------------
#
# `tile` is None for a pan-sized layer and a (w, h) pair for a wrapping one. A
# wrapping row must not be sized through layer_size() -- see the comment at
# GROUND_TILE -- and print_sizes below says so in its own output rather than
# quietly printing a number that would be wrong.
LAYERS = [
    ('SKY', 'backdrop_sky', PARALLAX_SKY, None),
    ('MOUNTAINS', 'backdrop_mountains', PARALLAX_MOUNTAIN, None),
    ('GROUND', 'backdrop_ground', PARALLAX_GROUND_FAR, GROUND_TILE),
]


def print_sizes():
    print(f'{"layer":<12}{"factor":<14}{"size":<14}{"BMP":>10}')
    for name, _key, factor, tile in LAYERS:
        w, h = tile if tile else layer_size(factor)
        mb = w * h * 3 / 1024 / 1024
        note = '  (tile - wraps, no pan relationship)' if tile else ''
        print(f'{name.lower():<12}{str(factor):<14}{f"{w}x{h}":<14}{mb:>9.1f}M{note}')


def generate_header(path=None):
    # Resolved here rather than as a default argument so it is anchored to the
    # repo this file lives in, not to the caller's working directory. This header
    # is overwritten wholesale, so writing it to the wrong tree is not a
    # missing-file error -- it is a silently damaged source tree.
    if path is None:
        path = src_path('render', 'backdrop_layers.h')
    out = []
    out.append('#pragma once')
    out.append('')
    out.append('// GENERATED FILE - do not edit.')
    out.append('//   python tools/generate_backdrop.py --header')
    out.append('//')
    out.append('// These four numbers per layer used to exist twice - once in the C++ that')
    out.append('// draws the layer and once in the Python that sizes its image - with a')
    out.append('// comment in each asking a human to keep them in step. The failure mode of')
    out.append('// that arrangement is a seam at the pan limit: a layer runs out of image')
    out.append('// before the camera runs out of world, and nothing says so until somebody')
    out.append('// walks to the edge of the map.')
    out.append('//')
    out.append('// tools/generate_backdrop.py is the source. It derives both the factors and')
    out.append('// the sizes, so a change on that side cannot leave this side stale.')
    out.append('//')
    out.append('// `width`/`height` are what the generator would write for this layer at')
    out.append('// these factors. main.cpp compares them against what it actually loaded and')
    out.append('// warns on a mismatch, which turns the seam from a pixel nobody reaches')
    out.append('// into a line at startup.')
    out.append('namespace backdrop_layers {')
    out.append('')
    out.append('// A wrapping layer\'s width/height is its tile size and is exact, where a')
    out.append('// pan-sized layer\'s is a minimum. main.cpp\'s warning reads it the second')
    out.append('// way for both, which is the right direction for the case that matters: a')
    out.append('// tile smaller than generated repeats sooner than the art was drawn for.')
    out.append('struct Layer {')
    out.append('    float parallax_x;')
    out.append('    float parallax_y;')
    out.append('    int width;  // the BMP size generate_backdrop.py produces at these factors')
    out.append('    int height;')
    out.append('};')
    out.append('')
    for name, key, factor, tile in LAYERS:
        w, h = tile if tile else layer_size(factor)
        out.append(f'// assets/{key}.bmp' + ('  (a tile - this layer wraps)' if tile else ''))
        out.append(f'inline constexpr Layer {name}{{{factor[0]}f, {factor[1]}f, {w}, {h}}};')
    out.append('')
    out.append('// The ground plane is drawn as strips between two depths, so it needs a')
    out.append('// second x factor that no other layer has. GROUND above carries the far edge')
    out.append('// (the horizon); this is the near one. Both are derivations off the')
    out.append('// geometric ladder - see tools/generate_backdrop.py, where the argument')
    out.append('// lives.')
    out.append(f'inline constexpr float GROUND_NEAR_X = {PARALLAX_GROUND_NEAR_X}f;')
    out.append('')
    mw, mh = layer_size(PARALLAX_MOUNTAIN)
    skyline = mountain_skyline(mw, mh)
    out.append('// Where the plane\'s far edge goes, as a row of the mountains BMP rather')
    out.append('// than a fraction of the window. A horizon authored independently of where')
    out.append('// the mountain silhouette sits contradicts it at every camera position, and')
    out.append('// the plane is opaque and drawn after the mountains, so it covers the whole')
    out.append('// band. Deriving the horizon from the silhouette makes that')
    out.append('// unrepresentable rather than unlikely.')
    out.append('//')
    out.append('// This is the deepest row the skyline reaches, so the whole jagged edge is')
    out.append('// above the plane and the solid band below it is what the plane may cover.')
    out.append('//')
    out.append('// Generated because it is a fact about the art: mountain_skyline() is a pure')
    out.append('// function of its seed and the two composition fractions, so a change to')
    out.append('// either moves this number without anyone having to remember to.')
    out.append('//')
    out.append('// A fraction of the layer\'s height and not a row index, which is not')
    out.append('// cosmetic: the renderer multiplies it by whatever mountains texture was')
    out.append('// actually loaded, so the horizon lands on the silhouette for any mountain')
    out.append('// image - including the small synthetic one the golden-frame fixture')
    out.append('// builds. Stated as a row it is a number only one image can satisfy.')
    out.append(f'// Shipped BMP: {mh} rows, skyline {min(skyline)}..{max(skyline)}.')
    out.append(f'inline constexpr float MOUNTAINS_SKYLINE_MAX = {max(skyline) / mh:.6f}f;')
    out.append('')
    out.append('// The inputs the sizes above are derived from, so a reader can tell whether')
    out.append('// a mismatch is a stale asset or a changed display table.')
    out.append(f'inline constexpr int GRID_WIDTH = {GRID_WIDTH};')
    out.append(f'inline constexpr int GRID_HEIGHT = {GRID_HEIGHT};')
    out.append(f'inline constexpr int SCALE = {SCALE};')
    out.append('')
    out.append('} // namespace backdrop_layers')
    out.append('')
    text = '\n'.join(out)
    with open(path, 'w', newline='\n') as f:
        f.write(text)
    print(f'wrote {path}')


if __name__ == '__main__':
    args = sys.argv[1:]
    if '--header' in args:
        generate_header()
    elif '--sizes' in args:
        print_sizes()
    else:
        generate_sky()
        generate_mountains()
        generate_ground()
