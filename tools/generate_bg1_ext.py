"""Builds `bg1_ext`, a 688x288 extension of the Background_1 scene, out of
Background_1's own measured pixels.

Nothing here is an art direction decision that was not already made. Every
colour, every silhouette shape and every band boundary is read off
art_src/Background_1/*.png and re-laid-out at the larger size. The generator
holds two things the source cannot state for itself -- how the extra columns are
filled, and where the extra rows go -- and both are argued below.

    python tools/generate_bg1_ext.py

Writes assets/bg1_ext/ (nine layer BMPs, colour-keyed exactly as
convert_background_layers.py writes assets/bg1/) plus the scene's material and
albedo maps. Deterministic: the layout is driven by a fixed-seed xorshift, so a
re-run is byte-identical and a diff in assets/ means a real change here.

--- why a generator and not nine drawings ------------------------------------

Because the requirement is to match the topology of the existing layers, and the
only thing that can hold that claim is code that takes the existing layers as
input. Nine hand-drawn images would match on the day they were drawn and drift
on the day the source is retouched; this file re-derives every time it runs.

--- the vertical rule: anchor to the edge the camera can reach ----------------

688x288 is exactly twice 344x144, but the scene must not be twice as tall a
picture -- it has to keep reading as this place through a window that shows
192x108 cells. So the new rows are not distributed proportionally:

  - Bottom-anchored art keeps its distance from the bottom. The ground plane,
    all five hill ranges and the foreground rocks move down as one body, so the
    near field the player stands in is pixel-for-pixel the source's. That is
    what makes the standing view identical.
  - Top-anchored art keeps its distance from the top. The sky's strata are
    painted at the top of the frame and the camera reaches the top of the world,
    so they stay where they are and the flat band below them gets longer.
  - The mountains span the gap, so the mountains are what grows. Their base is
    the horizon (bottom-anchored) and their peaks live in the sky
    (top-anchored), so the range gains the whole of the new headroom. This is
    not a preference -- it is the only layer both rules touch.

--- the horizontal rule: place the source's own masses, twice ----------------

Each silhouette layer is reduced to a per-column record -- top row, bottom row,
and the rows of its 1px accent lines -- and cut at its transparent gaps into
whole masses. The source's layers are not busy: one is a single long ridge,
another is three rocks with a wide hole between two of them. So the extension
places two of every mass the source has, shuffled, each one mirrored or mildly
stretched, and spends the leftover width on gaps.

Two copies is the whole layout rule, and it is what makes the density come out
roughly right without being tuned: twice the width gets twice the masses at the
source's own size distribution, so the extended layer covers about the same
fraction of its columns. It is close rather than equal, and the two directions
of the gap are different things -- a layer already covering most of its width
loses columns to MIN_GAP and to the fit-down pass, and a sparse one gains them
when the stretch rolls high. Every layer prints both numbers rather than
asserting on them, because a fraction that has drifted is a thing to look at,
not a build failure.

Two consequences of splicing by column are the reason it is done this way. The
accent lines travel with the columns they were measured on, so the source's
drawing idiom -- a 1px rim on every mass's top edge, a 1px line along its foot,
interior contour lines tracing the ridges behind it -- is reproduced without
being described. And because a segment keeps its own rows, every hill's foot
lands where the source put it plus the vertical shift. That matters beyond
looks: assets/bg1/backdrop.txt bands the ground plane at the factors of whatever stands
on each band, and nothing in this file is free to move those contacts.

--- the mountains, which are the one layer not re-laid-out at all -------------

The range is one landform, so it is not spliced, it is enlarged. Every other
layer is a handful of separate masses with sky between them, which is what makes
drawing two of each and re-spacing them work. The range is not that: it descends
from a massif at one edge to low ground at the other, so its two ends are far
apart and any reordering of it is a cliff. Segment-shuffling produces a pair of
towers with a knife-edge notch between them.

So the range is resampled to the new width and scaled about its base, at the
same factor on both axes. Both axes matters: scaling only the height turns soft
ridges into a picket fence of spikes, because the peaks keep their width while
gaining their height. Its own variation then comes from jitter_ridge, a bounded
walk of the ridge line -- the only kind of variation a single continuous
landform can take without being cut.

Its summit is reconstructed before any of that, because the source's is clipped:
its leftmost columns sit at row 0, so the summit was never painted, and scaling a
clipped profile turns a mountain into a mesa. The slope leaving the massif is
measured over the ten columns beside it and the run is filled with the dome that
slope is heading for, which puts the summit above the source's frame and leaves
sky above the tallest thing in the world.

--- the band layers ----------------------------------------------------------

The sky and the ground plane are strata, not masses, so they get the other
treatment: each boundary between two strata is re-generated as a random walk
whose step distribution is the empirical one from the source's own boundary,
clamped to the rows that boundary actually occupied.

The clamp is not cosmetic. The backdrop.txt band boundaries are placed inside
runs of rows that are uniform in colour, because a band boundary is a
discontinuity in scroll offset and is invisible only on flat paint. Clamping each
generated contour to the range its source occupied keeps those runs flat at the
shifted rows, which is what lets the extended band table cut where it does.
check_flat_cuts below asserts it on the generated pixels rather than trusting the
argument, and boot_test asserts it again on the shipped BMP.
"""

import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from pixel_art import (
    ALPHA_THRESHOLD,
    COLOR_KEY,
    LEGEND_EMPTY,
    LEGEND_WALL,
    assert_legend_matches_header,
    read_png,
    write_bmp,
)

SRC_DIR = os.path.join("art_src", "Background_1")
DST_DIR = os.path.join("assets", "bg1_ext")

# The source's native size, and the extension's. Read as a pair: every offset
# below is stated as a difference between them rather than as a bare number.
SRC_W, SRC_H = 344, 144
NATIVE_W, NATIVE_H = 688, 288

# The bottom-anchored offset -- see the vertical rule at the top of this file.
SHIFT = NATIVE_H - SRC_H  # 144

# Measured off the ground plane's first opaque row. Not a constant to choose,
# which is why `main` asserts it against the art rather than using it.
SRC_HORIZON = 63
HORIZON = SRC_HORIZON + SHIFT  # 207

# The floor row is measured off the reference frame -- the argument is at
# convert_background_layers.FLOOR_ROW and is not repeated. It is bottom-anchored
# like everything else the player touches.
SRC_FLOOR_ROW = 132
FLOOR_ROW = SRC_FLOOR_ROW + SHIFT  # 276

# How much the mountain range grows -- on both axes, which is the whole point.
# It is the frame's own factor, and scaling a silhouette by it is the only way
# to make the range fill the new headroom without changing its shape. Height
# alone turns soft ridges into spikes.
RIDGE_SCALE = 200

# How far the ridge line may wander from the source's, in rows, once it has been
# scaled. Bounded rather than free: at a few rows it is the same range seen along
# a different stretch of itself, and at a large one it is a second range nobody
# drew.
RIDGE_JITTER = 7

# From src/scene/legend.h, which is frozen. Looked up there, never invented --
# the same two rows convert_background_layers.py uses, via tools/pixel_art.py,
# which checks them against the header.

# Fixed so the output is reproducible. Any value would do; this one is not
# special and nothing may be tuned by changing it -- a re-roll that "looks
# better" is an art decision made where no one can see it.
SEED = 0x62673165

# (source, destination, kind). Listed front to back, the order the art's README
# uses; the draw order is the reverse.
#
# `kind` is what the layer is made of, which decides which of the three synthesis
# paths it takes: "islands" is a keyed silhouette with gaps in it, "ridge" is a
# keyed silhouette that spans the frame, "bands" is strata.
LAYERS = [
    ("01_foreground_rocks.png", "bg1_ext_01_fg_rocks.bmp", "islands"),
    ("02_hills_near.png", "bg1_ext_02_hills_near.bmp", "islands"),
    ("03_hills_midnear.png", "bg1_ext_03_hills_midnear.bmp", "islands"),
    ("04_hills_mid.png", "bg1_ext_04_hills_mid.bmp", "islands"),
    ("05_hills_midfar.png", "bg1_ext_05_hills_midfar.bmp", "islands"),
    ("06_hills_far.png", "bg1_ext_06_hills_far.bmp", "islands"),
    ("07_distant_mountains.png", "bg1_ext_07_mountains.bmp", "ridge"),
    ("08_ground_plane.png", "bg1_ext_08_ground.bmp", "bands"),
    ("09_sky.png", "bg1_ext_09_sky.bmp", "bands"),
]

# Back to front -- the numeric-descending filename order, the same order
# convert_background_layers.DRAW_ORDER and assets/bg1_ext/backdrop.txt state.
DRAW_ORDER = [name for name, _, _ in reversed(LAYERS)]

MATERIAL_PATH = os.path.join("assets", "bg1_ext_material.bmp")
ALBEDO_PATH = os.path.join("assets", "bg1_ext_albedo.bmp")

# The smallest hole left between two placed masses. Zero would let two islands
# fuse into one wider island, which is the one way this layout can produce a
# shape the source does not contain.
MIN_GAP = 3


# --- the one source of randomness --------------------------------------------


class Rng:
    """A 32-bit xorshift, so the layout is a pure function of `SEED`.

    Python's `random` would do the same job; this is here so the sequence does
    not depend on a CPython version's Mersenne implementation, which is the same
    reason `sim_random` in the engine is a hand-written hash rather than
    `<random>`.
    """

    def __init__(self, seed):
        self.s = seed & 0xFFFFFFFF or 1

    def next(self):
        x = self.s
        x ^= (x << 13) & 0xFFFFFFFF
        x ^= x >> 17
        x ^= (x << 5) & 0xFFFFFFFF
        self.s = x
        return x

    def below(self, n):
        return self.next() % n if n > 0 else 0

    def between(self, lo, hi):
        """Inclusive both ends."""
        return lo + self.below(hi - lo + 1)

    def chance(self, percent):
        return self.below(100) < percent

    def pick(self, seq):
        return seq[self.below(len(seq))]

    def shuffle(self, seq):
        for i in range(len(seq) - 1, 0, -1):
            j = self.below(i + 1)
            seq[i], seq[j] = seq[j], seq[i]


# --- reading bg1 -------------------------------------------------------------


def read_layer(name):
    """Returns (opaque[y][x], colour[y][x]) for one Background_1 PNG.

    `colour` is meaningless where `opaque` is false. Transparency is the same
    alpha threshold `png_to_bmp.convert` uses, imported rather than repeated so
    this cannot disagree with the converter about what is solid.
    """
    path = os.path.join(SRC_DIR, name)
    width, height, pixels, alpha = read_png(path)
    if (width, height) != (SRC_W, SRC_H):
        raise SystemExit(f"error: {name} is {width}x{height}, expected {SRC_W}x{SRC_H}")
    opaque = [
        [alpha is None or alpha[y * SRC_W + x] >= ALPHA_THRESHOLD for x in range(SRC_W)]
        for y in range(SRC_H)
    ]
    colour = [[pixels[y * SRC_W + x] for x in range(SRC_W)] for y in range(SRC_H)]
    return opaque, colour


def palette_of(opaque, colour):
    """The layer's colours, most-used first.

    **This is why `bg1_ext` cannot drift off bg1's palette.** Nothing downstream
    names a colour; every pixel written is one of the tuples this returns.
    """
    counts = collections.Counter()
    for y in range(SRC_H):
        for x in range(SRC_W):
            if opaque[y][x]:
                counts[colour[y][x]] += 1
    return [c for c, _ in counts.most_common()]


# --- the silhouette layers ---------------------------------------------------
#
# A column record is (top, bot, accents): the rows of the silhouette's top and
# bottom edge in this column, and the rows of every accent-coloured pixel in it.
# Splicing at column granularity is what carries the source's 1px line idiom
# across without this file ever having to describe it.


def columns_of(opaque, colour, accent):
    """Per-column records, `None` where the column is empty."""
    out = []
    for x in range(SRC_W):
        ys = [y for y in range(SRC_H) if opaque[y][x]]
        if not ys:
            out.append(None)
            continue
        accents = tuple(y for y in ys if colour[y][x] == accent)
        out.append((ys[0], ys[-1], accents))
    return out


def runs_of(cols):
    """Split a column list into maximal runs of present columns."""
    runs = []
    x = 0
    while x < len(cols):
        if cols[x] is None:
            x += 1
            continue
        start = x
        while x < len(cols) and cols[x] is not None:
            x += 1
        runs.append(cols[start:x])
    return runs


def mirrored(seg):
    return seg[::-1]


def stretched(seg, percent):
    """Horizontally resample one segment. Vertical rows are never touched, and
    that is load-bearing: a hill's foot has to land on the shore contour it was
    painted to stand on, so the only freedom this file takes with a mass is
    which way round it is and how wide."""
    width = max(1, (len(seg) * percent) // 100)
    if width == len(seg):
        return list(seg)
    return [seg[min(len(seg) - 1, (i * len(seg)) // width)] for i in range(width)]


def spread(slack, slots, rng):
    """Split `slack` columns of empty space between `slots` gaps, unevenly.

    Even spacing is what makes a generated layer look generated, so the split is
    weighted at random - which is also what reproduces bg1's own composition,
    where `hills_near`'s three rocks are separated by a 3-column notch and a
    173-column hole.
    """
    weights = [rng.between(1, 100) for _ in range(slots)]
    total = sum(weights)
    gaps = [slack * w // total for w in weights]
    gaps[rng.below(slots)] += slack - sum(gaps)
    return gaps


def assemble_islands(runs, rng, stretch_lo, stretch_hi):
    """Two of every mass, shuffled, with the leftover width spent on gaps."""
    segs = []
    for _ in range(2):
        for run in runs:
            seg = mirrored(run) if rng.chance(50) else list(run)
            segs.append(tapered(stretched(seg, rng.between(stretch_lo, stretch_hi)), 6))
    rng.shuffle(segs)

    # Fit before placing. Two stretched copies of a layer that already covered
    # most of the source width can overrun the extension; shrinking every mass
    # by the same factor keeps their relative sizes, which a per-mass retry
    # would not.
    budget = NATIVE_W - MIN_GAP * (len(segs) - 1)
    total = sum(len(s) for s in segs)
    if total > budget:
        segs = [stretched(s, max(1, budget * 95 // total)) for s in segs]
        total = sum(len(s) for s in segs)

    gaps = spread(NATIVE_W - total - MIN_GAP * (len(segs) - 1), len(segs) + 1, rng)
    out = [None] * NATIVE_W
    x = gaps[0]
    for i, seg in enumerate(segs):
        for j, col in enumerate(seg):
            if 0 <= x + j < NATIVE_W:
                out[x + j] = col
        x += len(seg) + MIN_GAP + gaps[i + 1]
    return out


def resample(cols, width):
    """Stretch a column list to `width`, interpolating rather than duplicating.

    A column list is a height profile, so it can be resampled smoothly - which
    is what keeps the mountains' 1px silhouette when the range is doubled,
    instead of the 2px staircase a nearest-neighbour stretch would give. Accent
    rows interpolate alongside the outline when the two source columns carry the
    same number of contour lines, and snap to the nearer column when they do
    not, which is where one of those lines starts or ends.
    """
    n = len(cols)
    out = []
    for i in range(width):
        p = i * (n - 1) / (width - 1) if width > 1 else 0.0
        a = int(p)
        b = min(n - 1, a + 1)
        f = p - a
        top_a, bot_a, acc_a = cols[a]
        top_b, bot_b, acc_b = cols[b]
        if len(acc_a) == len(acc_b):
            accents = tuple(
                int(round(ya + (yb - ya) * f)) for ya, yb in zip(acc_a, acc_b, strict=True)
            )
        else:
            accents = acc_a if f < 0.5 else acc_b
        out.append(
            (
                int(round(top_a + (top_b - top_a) * f)),
                int(round(bot_a + (bot_b - bot_a) * f)),
                accents,
            )
        )
    return out


def jitter_ridge(cols, rng, amplitude, hold_percent):
    """Walk the ridge line up and down by a few rows as it crosses the frame.

    The mountains are the one layer that is *not* re-laid-out - see the section
    above - so without this the extended range would be bg1's range at twice the
    size and nothing else. A slow bounded walk changes which bumps are where
    without breaking the landform, which is the only kind of variation a single
    continuous ridge can take: every alternative tried here (shuffled segments,
    a mirrored second copy) buys its variety with a cliff.

    The foot line is left alone - it is the horizon, and it does not wander.
    """
    out, offset = [], 0
    for top, bot, accents in cols:
        if rng.chance(hold_percent):
            offset += 1 if rng.chance(50) else -1
            offset = max(-amplitude, min(amplitude, offset))
        moved = min(bot, top + offset)
        out.append(
            (
                moved,
                bot,
                tuple(y if y >= bot - 2 else min(bot, max(moved, y + offset)) for y in accents),
            )
        )
    return out


def reconstruct_clipped(cols, slope_span):
    """Give a summit back to the columns the source frame cut off at row 0.

    The slope leaving the massif is measured over the `slope_span` columns
    beside it, and the run is filled with the dome that slope is heading for: an
    arc peaking at the run's centre and coming back down to the neighbour's own
    height at both ends.

    **Symmetric rather than "keep climbing to the frame edge", and the reason is
    a join.** The extended range is assembled by butting segments together, so a
    segment that ends high has to find a neighbour that starts high or it leaves
    a sheer cliff - which is exactly what the first version of this produced, a
    pair of towers with a knife-edge notch between them. A dome ends where its
    neighbours are, so it drops into the range wherever it is placed.
    """
    tops = [c[0] if c else None for c in cols]
    out = list(cols)
    x = 0
    while x < len(cols):
        if tops[x] != 0:
            x += 1
            continue
        start = x
        while x < len(cols) and tops[x] == 0:
            x += 1
        end = x - 1
        # The height the neighbouring slope would gain over half the run, and the
        # height to come back to at both ends.
        slopes, anchor = [], 0
        if start > 0 and tops[start - 1] is not None:
            far = max(0, start - slope_span)
            slopes.append((tops[far] - tops[start - 1]) / max(1, start - 1 - far))
            anchor = tops[start - 1]
        if end + 1 < len(cols) and tops[end + 1] is not None:
            far = min(len(cols) - 1, end + 1 + slope_span)
            slopes.append((tops[far] - tops[end + 1]) / max(1, far - end - 1))
            anchor = tops[end + 1]
        if not slopes:
            continue
        half = max(1.0, (end - start + 1) / 2.0)
        # The shallower of two anchors: the steeper one extrapolated across a wide
        # massif is what produces a spike.
        rise = min(slopes) * half
        centre = (start + end) / 2.0
        for i in range(start, end + 1):
            t = (i - centre) / half
            top, bot, accents = cols[i]
            out[i] = (anchor - int(round(rise * (1.0 - t * t))), bot, accents)
        x = end + 1
    return out


def tapered(seg, span):
    """Wedge off a mass end that the source frame cut flat.

    bg1's masses that touch its left or right edge stop mid-slope, so a column
    30 rows tall sits at the boundary. That is invisible against the frame edge
    and is a vertical cliff anywhere else, which is what it became the first
    time these masses were placed inland. An end that already tapers is left
    alone, so this only touches the ends the frame made.
    """
    seg = list(seg)
    n = len(seg)
    for side in (0, 1):
        at = (lambda i: i) if side == 0 else (lambda i: n - 1 - i)
        top, bot, _ = seg[at(0)]
        if bot - top <= 3:
            continue
        k = min(max(span, bot - top), n // 2)
        for i in range(k):
            t, b, accents = seg[at(i)]
            new_top = b - ((b - t) * i) // k
            seg[at(i)] = (new_top, b, tuple(y for y in accents if new_top <= y <= b))
    return seg


def scale_about(cols, base_row, factor_percent):
    """Scale a column list's height about `base_row`, keeping the base put.

    Only the ridge layer uses this. The accent rows scale with the silhouette so
    the interior contour lines stay on the ridges they were tracing.
    """

    def lift(y):
        return base_row - ((base_row - y) * factor_percent) // 100

    out = []
    for col in cols:
        if col is None:
            out.append(None)
            continue
        top, bot, accents = col
        out.append((lift(top), lift(bot), tuple(lift(y) for y in accents)))
    return out


def paint_masses(cols, base, accent, dy):
    """Turn a column list into a keyed pixel grid.

    Fill first, then the accents, then the rim and the foot line. The rim is
    recomputed rather than taken from the record because a stretched segment can
    lose its top pixel to resampling, and bg1's idiom is that **every** column's
    top pixel is accent - measured, on every layer that is not clipped by the
    frame.
    """
    grid = [[None] * NATIVE_W for _ in range(NATIVE_H)]
    for x, col in enumerate(cols):
        if col is None:
            continue
        top, bot = col[0] + dy, col[1] + dy
        if top < 0 or bot >= NATIVE_H:
            raise SystemExit(
                f"error: column {x} runs to rows {top}..{bot}, "
                f"outside a {NATIVE_W}x{NATIVE_H} layer"
            )
        for y in range(top, bot + 1):
            grid[y][x] = base
        for y in col[2]:
            if top <= y + dy <= bot:
                grid[y + dy][x] = accent
        grid[top][x] = accent
        grid[bot][x] = accent
    return grid


# --- the band layers ---------------------------------------------------------


def strata_of(opaque, colour):
    """The layer's colours in vertical order, with the row its paint starts on.

    Vertical order, not frequency order: a stratum is where it is, and the sky's
    least-used colour is its topmost one.
    """
    first = next(y for y in range(SRC_H) if any(opaque[y]))
    strata = []
    for y in range(first, SRC_H):
        c = colour[y][0]
        if opaque[y][0] and c not in strata:
            strata.append(c)
    return first, strata


def boundaries_of(opaque, colour, first, strata):
    """For each interior boundary between two strata, the per-column row where
    the upper stratum ends, and the rows that boundary occupied."""
    out = []
    for i in range(len(strata) - 1):
        rows = []
        for x in range(SRC_W):
            y = first
            while y < SRC_H and (not opaque[y][x] or colour[y][x] in strata[: i + 1]):
                y += 1
            rows.append(y)
        out.append((rows, min(rows), max(rows)))
    return out


def walk_like(rows, lo, hi, rng):
    """A new boundary of `NATIVE_W` columns with bg1's step distribution.

    Sampling steps from the empirical pool is what reproduces the *texture* of
    these contours - long flat runs broken by a single-row step - which a smooth
    curve or uniform noise both get wrong. The clamp to [lo, hi] is the
    band-boundary guarantee argued at the top of this file, so it is not a
    detail: widening it un-flattens the rows `EXT_GROUND_BANDS` cuts on.
    """
    steps = [rows[i + 1] - rows[i] for i in range(len(rows) - 1)]
    mid = (lo + hi) // 2
    out, y = [], rows[0]
    for _ in range(NATIVE_W):
        out.append(y)
        y += rng.pick(steps)
        # A gentle pull to the middle. Without it an i.i.d. walk spends most of its
        # length pinned against one clamp, which reads as a straight line.
        if y < mid and rng.chance(12):
            y += 1
        elif y > mid and rng.chance(12):
            y -= 1
        y = max(lo, min(hi, y))
    return out


def paint_bands(strata, first_row, edges, dy, keyed):
    """Fill `NATIVE_W` columns of strata, top-anchored or bottom-anchored.

    `keyed` says whether the rows above the paint are transparent (the ground
    plane) or filled with the topmost stratum (the sky, which is opaque and is
    the only layer that reaches row 0).
    """
    grid = [[None] * NATIVE_W for _ in range(NATIVE_H)]
    for x in range(NATIVE_W):
        y = first_row + dy
        for i, edge in enumerate(edges):
            stop = min(NATIVE_H, edge[x] + dy)
            while y < stop:
                grid[y][x] = strata[i]
                y += 1
        while y < NATIVE_H:
            grid[y][x] = strata[-1]
            y += 1
    if not keyed:
        for y in range(first_row + dy):
            for x in range(NATIVE_W):
                grid[y][x] = strata[0]
    return grid


# --- output ------------------------------------------------------------------


def flatten(grid, keyed):
    """Grid of colours-or-None to the flat list `write_bmp` takes.

    The `COLOR_KEY` check is `png_to_bmp.convert`'s nudge stated as a refusal,
    for the one reason that module's own docstring allows a second copy: nothing
    in this path has a PNG or an alpha channel to hand it, so `convert` cannot
    be called at all. An opaque pixel that happens to be exactly the
    transparency colour would vanish at render time; bg1 has none, and this
    asserts rather than assumes it.
    """
    out = []
    for row in grid:
        for c in row:
            if c is None:
                if not keyed:
                    raise SystemExit("error: an opaque layer has an unpainted pixel")
                out.append(COLOR_KEY)
            elif c == COLOR_KEY:
                raise SystemExit("error: bg1 uses the transparency colour as paint")
            else:
                out.append(c)
    return out


def check_flat_cuts(grid, cuts):
    """The property the `bands=` in backdrop.txt places its band boundaries for, checked on
    the pixels this run just produced.

    A band boundary is a step in scroll offset, so it can only be invisible
    where the rows either side of it are one flat colour and the same one. This
    is `boot_test`'s `test_bg1_ground_bands` check, run here as well so that a
    bad roll is caught by the tool that produced it rather than by a suite two
    steps later.
    """
    for cut in cuts:
        above, below = set(grid[cut - 1]), set(grid[cut])
        if len(above) != 1 or len(below) != 1 or above != below:
            raise SystemExit(
                f"error: the ground band cut at row {cut} does not fall on flat "
                f"paint (row {cut - 1}: {len(above)} colours, row {cut}: "
                f"{len(below)} colours) - the shore contour would step and slide"
            )


def build_scene_maps(composite):
    """The material and albedo maps that give `bg1_ext` a floor.

    Same shape as `convert_background_layers.build_scene_maps` and for the same
    reason - a backdrop set is a world the player has to stand in, and a `floor`
    spawn would pin the body to the world's bottom border. The albedo is the
    whole frame rather than the floor band because `src/scene/bmp.cpp` requires
    the two maps to be the same size.
    """
    assert_legend_matches_header()
    material, albedo, solid = [], [], 0
    for y in range(NATIVE_H):
        for x in range(NATIVE_W):
            if y >= FLOOR_ROW:
                material.append(LEGEND_WALL)
                solid += 1
            else:
                material.append(LEGEND_EMPTY)
            albedo.append(composite[y][x])
    write_bmp(MATERIAL_PATH, NATIVE_W, NATIVE_H, material)
    write_bmp(ALBEDO_PATH, NATIVE_W, NATIVE_H, albedo)
    print(
        f"scene maps -> {MATERIAL_PATH}, {ALBEDO_PATH}  "
        f"({NATIVE_W}x{NATIVE_H}, floor at row {FLOOR_ROW}, {solid} solid cells)"
    )


def build_layer(src_name, kind, rng):
    """One layer: measure bg1's, lay out the extension, return the pixel grid
    and the line describing what came out."""
    opaque, colour = read_layer(src_name)

    if kind == "bands":
        first, strata = strata_of(opaque, colour)
        keyed = first > 0  # only the sky reaches row 0
        dy = SHIFT if keyed else 0  # the ground is bottom-anchored
        edges = [
            walk_like(rows, lo, hi, rng)
            for rows, lo, hi in boundaries_of(opaque, colour, first, strata)
        ]
        grid = paint_bands(strata, first, edges, dy, keyed)
        if keyed:
            if first != SRC_HORIZON:
                raise SystemExit(
                    f"error: the ground plane starts at row {first}, "
                    f"not the measured horizon {SRC_HORIZON}"
                )
            check_flat_cuts(grid, [82 + SHIFT, 100 + SHIFT])
        note = f"{len(strata)} strata, horizon at row {first + dy}"
        return grid, keyed, note

    palette = palette_of(opaque, colour)
    base, accent = palette[0], palette[1]
    cols = columns_of(opaque, colour, accent)
    source_cols = sum(1 for c in cols if c is not None)

    if kind == "ridge":
        cols = reconstruct_clipped(cols, slope_span=10)
        base_row = max(c[1] for c in cols if c is not None)
        placed = jitter_ridge(
            scale_about(resample(cols, NATIVE_W), base_row, RIDGE_SCALE), rng, RIDGE_JITTER, 9
        )
        summit = min(c[0] for c in placed if c is not None) + SHIFT
        note = f"one range at {RIDGE_SCALE}% on both axes, summit at row {summit}"
    else:
        placed = assemble_islands(runs_of(cols), rng, 85, 115)
        note = f"{len(runs_of(cols)) * 2} masses"

    grid = paint_masses(placed, base, accent, SHIFT)
    covered = sum(1 for c in placed if c is not None)
    note += (
        f", {100 * covered // NATIVE_W}% of columns covered against bg1's "
        f"{100 * source_cols // SRC_W}%"
    )
    return grid, True, note


def main():
    if not os.path.isdir(SRC_DIR):
        print(f"error: {SRC_DIR} not found - run this from the repo root (code/)")
        return 1

    os.makedirs(DST_DIR, exist_ok=True)
    rng = Rng(SEED)
    grids = {}

    for src_name, dst_name, kind in LAYERS:
        grid, keyed, note = build_layer(src_name, kind, rng)
        path = os.path.join(DST_DIR, dst_name)
        write_bmp(path, NATIVE_W, NATIVE_H, flatten(grid, keyed))
        grids[src_name] = grid
        print(
            f"{src_name} -> {path}  ({NATIVE_W}x{NATIVE_H}, "
            f"{'keyed' if keyed else 'opaque'}; {note})"
        )

    composite = [[(0, 0, 0)] * NATIVE_W for _ in range(NATIVE_H)]
    for name in DRAW_ORDER:
        grid = grids[name]
        for y in range(NATIVE_H):
            for x in range(NATIVE_W):
                if grid[y][x] is not None:
                    composite[y][x] = grid[y][x]
    build_scene_maps(composite)
    return 0


if __name__ == "__main__":
    sys.exit(main())
