"""Draws the angler's body and prints it as the ASCII grid src/physics/angler_art.h holds.

The fish of tools/fish_art.py -- the same bass, painted by the same functions --
on different limbs: two octopus tentacles where the man's legs were, and a thin
tentacle out of the top of its head where the man's arm was, ending in a pale
glowing bulb that hangs in front of the mouth. The real anglerfish's lure.

What makes these tentacles rather than limbs is that they bend along their
length. Each one is drawn along a chain of joints -- whole cells, written into
the header as REAR_CHAIN, FRONT_CHAIN and LURE_CHAIN -- and the game turns each
link of the chain about its own joint (src/physics/tentacle.h), so the pixels
drawn here between two joints are the pixels that link carries. That is why the
chain is drawn first and the shapes are capsules along it, rather than the
chain being fitted to a shape afterwards: a pixel between joints 3 and 4 has to
be where links 3 and 4 put it.

Usage:
    python tools/angler_art.py --header        # writes src/physics/angler_art.h
    python tools/angler_art.py                 # prints the ROWS block
    python tools/angler_art.py --png out.png   # also writes an 8x preview

The roles (see angler_art.h):
    'E' 'O'               the eye        -- HEAD, as the fish's
    'a' 'b' ... 'g'       the mid-flank  -- HEART, as the fish's
    'j' 'k' 'l' 'n'       the lure stalk -- the ARM, by letter
    'u' 'v' 'w'           the lure bulb  -- the ARM, by letter
    'J' 'K' 'L' 'N' 'Y'   the tentacle legs and their suckers -- legs by geometry
"""

import argparse
import itertools
import math
import os
import string
import subprocess

import fish_art
from fish_art import (
    BOX_LEFT,
    EMPTY,
    HIP_ROW,
    LEG_SPLIT,
    SHOULDER,
    H,
    W,
    capsule,
    ellipse,
    union,
)
from troll_art import ROOT, write_header

# Links per tentacle. The lure is nearly twice as long as a leg and hooks back
# on itself, so it gets twelve, which keeps every joint of its rest hook under
# the game's bend limit (Motion::max_bend). The legs get twelve too, about three
# cells each: they were eight of four or five, and a leg bent along eight long
# links read as a polyline with corners, however softly the game bent each
# joint. The same bend spread over more, shorter links is a curve. The game's
# tentacle::MAX_JOINTS is the most plus one.
#
# The legs are still *drawn* along the eight-link chain they were first drawn
# on (LEG_DRAW_LINKS), so finer links changed which link carries a pixel and
# not a single pixel of the art. Twelve links rounded to whole cells zigzag a
# little at rest -- a cell either way every few joints -- which a pixel never
# shows but a capsule drawn along it would.
LEG_LINKS = 12
LEG_DRAW_LINKS = 8
LURE_LINKS = 12


def catmull_rom(points, samples=400):
    """A smooth curve through every point, sampled densely, ends included."""
    pts = [points[0]] + list(points) + [points[-1]]
    out = []
    for i in range(1, len(pts) - 2):
        p0, p1, p2, p3 = pts[i - 1], pts[i], pts[i + 1], pts[i + 2]
        n = samples // (len(points) - 1)
        for k in range(n):
            t = k / n
            t2, t3 = t * t, t * t * t
            out.append(
                tuple(
                    0.5
                    * (
                        2 * p1[j]
                        + (-p0[j] + p2[j]) * t
                        + (2 * p0[j] - 5 * p1[j] + 4 * p2[j] - p3[j]) * t2
                        + (-p0[j] + 3 * p1[j] - 3 * p2[j] + p3[j]) * t3
                    )
                    for j in (0, 1)
                )
            )
    out.append(points[-1])
    return out


def chain_of(points, links):
    """links + 1 joints, evenly spaced along the curve through `points` and
    rounded to whole cells -- the joints sit on pixel centres, as every rig
    joint does, so the rest pose maps every pixel to itself."""
    curve = catmull_rom(points)
    lengths = [0.0]
    for a, b in itertools.pairwise(curve):
        lengths.append(lengths[-1] + math.dist(a, b))
    total = lengths[-1]
    joints, k = [], 0
    for i in range(links + 1):
        want = total * i / links
        while k < len(lengths) - 2 and lengths[k + 1] < want:
            k += 1
        span = max(1e-9, lengths[k + 1] - lengths[k])
        f = min(1.0, max(0.0, (want - lengths[k]) / span))
        x = curve[k][0] + (curve[k + 1][0] - curve[k][0]) * f
        y = curve[k][1] + (curve[k + 1][1] - curve[k][1]) * f
        joints.append((round(x), round(y)))
    return joints


def tentacle(joints, r0, r1):
    """Capsules along the chain, tapering from r0 at the root to r1 at the tip."""

    links = len(joints) - 1

    def r(i):
        return r0 + (r1 - r0) * (i / links) ** 0.8

    return union(*(capsule(*joints[i], *joints[i + 1], r(i), r(i + 1)) for i in range(links)))


# --- the tentacles ------------------------------------------------------------
#
# The legs hang from under the belly to the ground, each bowed outward -- the
# rear one back, the front one forward -- so that standing they read as
# tentacles holding the body up, not as poles. Each stays inside its half of
# the box's columns, either side of the split, the whole way down: below the
# hips a pixel is leg because of where it is (rig::part_of), so a tentacle that
# wandered out of the box would leave its pixels behind as body.
REAR_CURVE = [(39, 49), (36.5, 60), (35, 71), (39, 83)]
FRONT_CURVE = [(55, 49), (58, 60), (58.5, 71), (55, 83)]
REAR_CHAIN = chain_of(REAR_CURVE, LEG_LINKS)
FRONT_CHAIN = chain_of(FRONT_CURVE, LEG_LINKS)
REAR_DRAWN = chain_of(REAR_CURVE, LEG_DRAW_LINKS)
FRONT_DRAWN = chain_of(FRONT_CURVE, LEG_DRAW_LINKS)
REAR_LEG = tentacle(REAR_DRAWN, 4.4, 1.1)
FRONT_LEG = tentacle(FRONT_DRAWN, 4.6, 1.1)

# The lure: up out of the top of the head, arching forward over the snout, and
# hanging straight down in front of the mouth to the bulb. Longer than the
# fish's arm -- about sixty cells -- because a tentacle strikes by reaching, not
# by swinging a forearm: the bulb has to get to the ground in front of it.
LURE_CHAIN = chain_of(
    [SHOULDER, (62, 10), (69, 3.5), (79, 4), (86, 10), (88.5, 19), (89, 28)], LURE_LINKS
)
BULB = LURE_CHAIN[-1]
BULB_R = 3.0
LURE = union(
    tentacle(LURE_CHAIN, 2.8, 1.1),
    ellipse(SHOULDER[0] + 0.5, SHOULDER[1] - 1.5, 3.2, 3.0),  # the knob it grows from
)


def distance_to_chain(joints, x, y):
    best = 1e9
    for (ax, ay), (bx, by) in itertools.pairwise(joints):
        dx, dy = bx - ax, by - ay
        t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / (dx * dx + dy * dy)))
        best = min(best, math.hypot(x - ax - t * dx, y - ay - t * dy))
    return best


def draw():
    grid = [[EMPTY] * W for _ in range(H)]
    legs = "JKLN"

    # The far tentacle behind the fish, the lure before it so the head covers
    # its root -- the fish's order, with the fish's own painter.
    fish_art.shade(grid, REAR_LEG, legs, -0.22)
    fish_art.shade(grid, LURE, "jkln", 0.05, R=3, dither=False)

    fish_art.paint_fish(grid)
    fish_art.shade(grid, REAR_LEG, legs, -0.22, HIP_ROW)
    # The near tentacle from just above the hips, so its root is over the
    # belly's edge and it grows out of the fish instead of being stood under it.
    fish_art.shade(grid, FRONT_LEG, legs, 0.0, HIP_ROW - 3)

    # Suckers: a pale dot every few cells down the inside of each tentacle, the
    # side facing the other one, from a little below the hip to near the tip.
    for joints, inward in ((REAR_DRAWN, 1), (FRONT_DRAWN, -1)):
        for i in range(2, LEG_DRAW_LINKS):
            (ax, ay), (bx, by) = joints[i], joints[i + 1]
            for f in (0.0, 0.5):
                cx, cy = ax + (bx - ax) * f, ay + (by - ay) * f
                x, y = round(cx + inward * (3.0 - 1.8 * i / LEG_DRAW_LINKS)), round(cy)
                if y >= HIP_ROW and grid[y][x] is not EMPTY and grid[y][x] in legs:
                    grid[y][x] = "Y"

    # The bulb: a teal ring round a pale core, lit at its upper left -- the one
    # cold colour on the body, so it is the first thing the eye finds.
    bulb = ellipse(BULB[0], BULB[1], BULB_R, BULB_R)
    core = ellipse(BULB[0] - 0.3, BULB[1] - 0.3, 1.6, 1.6)
    for y in range(H):
        for x in range(W):
            if bulb(x, y):
                grid[y][x] = "u"
            if core(x, y):
                grid[y][x] = "v"
    grid[BULB[1] - 1][BULB[0] - 1] = "w"

    # Below the hips a pixel outside the box's columns is body, not leg
    # (rig::part_of): a tentacle bowed out past the box would leave a strip of
    # itself standing still beside it as it walked.
    for y in range(HIP_ROW, H):
        for x in list(range(BOX_LEFT)) + list(range(W - BOX_LEFT, W)):
            assert grid[y][x] is EMPTY or grid[y][x] not in legs + "Y", (
                f"tentacle pixel ({x}, {y}) is outside the box below the hips"
            )

    fish_art.paint_eye(grid)
    fish_art.cut_limbs(grid, legs + "Y", "jklnuvw")
    fish_art.mark_heart(grid)
    return fish_art.rows_of(grid)


def radius_of(rows, joints, keep):
    """The furthest any of the tentacle's pixels is from its chain, rounded up:
    the bound the game puts round each posed link to find its pixels."""
    far = 0.0
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            if c != "." and keep(x, y, c):
                far = max(far, distance_to_chain(joints, x, y))
    return math.ceil(far)


PALETTE = [(c, argb, what) for c, argb, what in fish_art.PALETTE if c not in "SPQRspqr"] + [
    # The tentacles: a bruised plum, warm against the bass's greens so the parts
    # that are not fish are the first thing the eye separates out, as the man's
    # skin was; suckers a pale flesh pink.
    ("J", 0xFF3E1A30, "tentacle, shadow"),
    ("K", 0xFF6B2D4A, "tentacle, mid"),
    ("L", 0xFF97476A, "tentacle, lit"),
    ("N", 0xFFC07090, "tentacle, highlight"),
    ("Y", 0xFFE6B8B4, "a sucker"),
    ("j", 0xFF3E1A30, "lure, shadow -- ARM"),
    ("k", 0xFF6B2D4A, "lure, mid -- ARM"),
    ("l", 0xFF97476A, "lure, lit -- ARM"),
    ("n", 0xFFC07090, "lure, highlight -- ARM"),
    ("u", 0xFF4FA898, "the bulb's ring -- ARM"),
    ("v", 0xFFB8F2E0, "the bulb, glowing -- ARM"),
    ("w", 0xFFF2FFF8, "the bulb's highlight -- ARM"),
]

HEADER_PATH = os.path.join(ROOT, "src", "physics", "angler_art.h")


def chain_text(joints):
    """The pairs on their own line, so the header stays inside 100 columns."""
    pairs = [f"{{{x}, {y}}}" for x, y in joints]
    rows = [", ".join(pairs[i : i + 9]) for i in range(0, len(pairs), 9)]
    return ",".join("\n    " + row for row in rows)


HEADER = string.Template(
    r'''// GENERATED by tools/angler_art.py - do not edit.
//
// Regenerate with:  python tools/angler_art.py --header
//
#pragma once
#include <cstdint>
#include "body_art.h"

// The angler's body: the fish's bass (fish_art.h), painted by the same code, on
// tentacles instead of a man's limbs -- two octopus arms it walks on, and a thin
// one out of the top of its head that arches over the snout and hangs a pale
// glowing bulb in front of the mouth, as a real anglerfish's lure does. 94x84,
// the fish's frame exactly, so the enemy pool's slots do not grow.
//
// The tentacles are what is new. Each is drawn along a chain of joints -- the
// *_CHAIN tables below -- and the game turns every link of the chain about its
// own joint (tentacle.h), so a tentacle bends along its length instead of
// turning whole about one joint as the fish's legs do. The pixels the generator
// drew between two joints are the pixels that link carries.
//
// The roles:
//  - 'E' and 'O', the eye, are the HEAD; 'a' to 'g', the middle of the flank,
//    the HEART. Both are the fish's.
//  - 'j' 'k' 'l' 'n' (the stalk) and 'u' 'v' 'w' (the bulb) are the ARM by
//    letter, for the fish's reason: no column rule can say which pixels above
//    and in front of the head are the lure. It joins the head only at its root.
//  - The legs are geometry, as for every body: inside the box's columns from
//    HIP_ROW down. Each tentacle keeps to its own side of the split all the way
//    down, and the two columns either side of it are empty.
namespace angler_art {

inline constexpr int W = $W;
inline constexpr int H = $H;

inline constexpr const char* ROWS[H] = {
$ROWS
};

// The fish's palette without the man's skin, and a plum ramp for the
// tentacles in its place -- the lure repeating it under its own letters, as the
// fish's arm repeated the skin -- with pale suckers and a teal bulb.
using body_art::Ink;
inline constexpr Ink PALETTE[] = {
$PALETTE
};
inline constexpr int PALETTE_COUNT = static_cast<int>(sizeof(PALETTE) / sizeof(PALETTE[0]));

inline constexpr int FOOT_ROWS = 3;
// The lure is lettered, so every row may hold it.
inline constexpr int ARM_TOP = 0;
// The fish's box, shoulder and hips: the body is the fish's.
inline constexpr int BOX_LEFT = $BOX_LEFT;
inline constexpr int BOX_RIGHT = W - BOX_LEFT;
inline constexpr int SHOULDER_X = $SHOULDER_X;
inline constexpr int SHOULDER_Y = $SHOULDER_Y;
inline constexpr int HIP_ROW = $HIP_ROW;
inline constexpr int LEG_SPLIT = $LEG_SPLIT;

// The chains, root first, on pixel centres: $LEG_JOINTS joints a leg and
// $LURE_JOINTS for the lure, which is nearly twice as long and hooks back on
// itself. A leg's links are about three cells, rounded to whole cells, so its
// rest chain zigzags a cell here and there; the game bends toward a smoothed
// rest curl (tentacle::rest_curl), never toward that zigzag. The radius is
// the furthest any of the tentacle's pixels is from its chain, rounded up --
// the bound the game draws round each posed link to look for its pixels, and
// which enemy_test checks against the art.
inline constexpr int LEG_JOINTS = $LEG_JOINTS;
inline constexpr int LURE_JOINTS = $LURE_JOINTS;
inline constexpr int REAR_CHAIN[LEG_JOINTS][2] = {$REAR_CHAIN};
inline constexpr int FRONT_CHAIN[LEG_JOINTS][2] = {$FRONT_CHAIN};
inline constexpr int LURE_CHAIN[LURE_JOINTS][2] = {$LURE_CHAIN};
inline constexpr int REAR_RADIUS = $REAR_RADIUS;
inline constexpr int FRONT_RADIUS = $FRONT_RADIUS;
inline constexpr int LURE_RADIUS = $LURE_RADIUS;

inline constexpr body_art::Art ART = body_art::with_arm_box(body_art::Art{
    ROWS, W, H, PALETTE, PALETTE_COUNT, "EO", "abcdfg", BOX_LEFT, ARM_TOP, FOOT_ROWS, "jklnuvw"});

static_assert(body_art::well_formed(ART),
              "angler_art::ROWS has a row of the wrong width or a character with no "
              "PALETTE entry");
static_assert(body_art::count_where(ART, &body_art::Art::is_head) >= 9 &&
                  body_art::count_where(ART, &body_art::Art::is_heart) > 0,
              "the angler needs its whole eye ('E'/'O') and a heart ('a'-'g'), or it "
              "is born dead -- or dies to one arrow in the eye");
static_assert(body_art::count_where(ART, &body_art::Art::is_foot) > 0,
              "the angler has nothing in its FOOT_ROWS inside the box, so it is born "
              "footless and collapses on its first hit");
static_assert(body_art::count_where(ART, &body_art::Art::is_arm) > 0,
              "the angler has no lure letters, so it is born armless and can never "
              "strike");
static_assert(body_art::connected<W * H>(ART),
              "some angler pixel is not 8-connected to the heart; it would fall off "
              "as sand the first time it is hit anywhere");
static_assert(body_art::stands_on_bottom_row(ART),
              "the angler art floats: its bottom row is empty");

// The lure touches the rest of the body only at its root, so a shot through the
// root takes it off whole -- the fish's rule for its arm.
constexpr bool lure_hangs_free() {
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            if (!ART.is_arm(x, y)) continue;
            const int sx = x - SHOULDER_X, sy = y - SHOULDER_Y;
            if (sx * sx + sy * sy <= 9) continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                    if (ART.is_body(nx, ny) && !ART.is_arm(nx, ny)) return false;
                }
        }
    return true;
}
static_assert(lure_hangs_free(),
              "an angler pixel touches the lure away from its root, so the lure can "
              "no longer be shot off whole");

constexpr bool legs_hang_free() {
    for (int y = HIP_ROW; y < H; ++y)
        if (ART.is_body(LEG_SPLIT - 1, y) || ART.is_body(LEG_SPLIT, y)) return false;
    return true;
}
static_assert(legs_hang_free(),
              "an angler pixel fills the gap between the tentacles, so neither can "
              "be severed at the hip on its own");

// The chains start where their tentacles do: the lure at the shoulder, each leg
// above the hips (inside the belly, so the first link turns the tentacle's root
// out from under it) and on its own side of the split.
static_assert(LURE_CHAIN[0][0] == SHOULDER_X && LURE_CHAIN[0][1] == SHOULDER_Y,
              "the lure's chain does not start at the shoulder");
static_assert(REAR_CHAIN[0][1] < HIP_ROW && FRONT_CHAIN[0][1] < HIP_ROW &&
                  REAR_CHAIN[0][0] < LEG_SPLIT && FRONT_CHAIN[0][0] > LEG_SPLIT,
              "a leg chain starts below the hips or on the wrong side of the split");
static_assert(REAR_CHAIN[LEG_JOINTS - 1][1] == H - 1 && FRONT_CHAIN[LEG_JOINTS - 1][1] == H - 1,
              "a leg chain does not end on the bottom row, so its tip is not where the "
              "body stands");

inline constexpr int PIXEL_COUNT = body_art::count_where(ART, &body_art::Art::is_body);

}  // namespace angler_art
'''
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png")
    ap.add_argument("--header", action="store_true", help="write the header rather than print ROWS")
    args = ap.parse_args()
    rows = draw()

    def in_box_below_hips(lo, hi):
        return lambda x, y, c: y >= HIP_ROW and lo <= x < hi

    radii = dict(
        REAR_RADIUS=radius_of(rows, REAR_CHAIN, in_box_below_hips(BOX_LEFT, LEG_SPLIT)),
        FRONT_RADIUS=radius_of(rows, FRONT_CHAIN, in_box_below_hips(LEG_SPLIT, W - BOX_LEFT)),
        LURE_RADIUS=radius_of(rows, LURE_CHAIN, lambda x, y, c: c in "jklnuvw"),
    )
    if args.header:
        write_header(
            HEADER_PATH,
            HEADER,
            rows,
            PALETTE,
            W=W,
            H=H,
            BOX_LEFT=BOX_LEFT,
            SHOULDER_X=SHOULDER[0],
            SHOULDER_Y=SHOULDER[1],
            HIP_ROW=HIP_ROW,
            LEG_SPLIT=LEG_SPLIT,
            LEG_JOINTS=LEG_LINKS + 1,
            LURE_JOINTS=LURE_LINKS + 1,
            REAR_CHAIN=chain_text(REAR_CHAIN),
            FRONT_CHAIN=chain_text(FRONT_CHAIN),
            LURE_CHAIN=chain_text(LURE_CHAIN),
            **radii,
        )
        # The chain tables are the one part of the header whose layout
        # clang-format has an opinion on that is awkward to write by hand, so
        # the header goes through it -- code_cleanup would otherwise reformat a
        # generated file and the next --header would undo it.
        try:
            subprocess.run(["clang-format", "-i", HEADER_PATH], check=True)
        except (OSError, subprocess.CalledProcessError) as e:
            print(f"clang-format not run ({e}); run it on {HEADER_PATH} before committing")
    else:
        for r in rows:
            print(f'    "{r}",')
        print("rear", REAR_CHAIN, "front", FRONT_CHAIN, "lure", LURE_CHAIN, radii)
    if args.png:
        from PIL import Image

        ink = {c: argb for c, argb, _ in PALETTE}
        s = 8
        img = Image.new("RGB", (W * s, H * s), (40, 32, 64))
        px = img.load()
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                if c == ".":
                    continue
                argb = ink[c]
                col = ((argb >> 16) & 255, (argb >> 8) & 255, argb & 255)
                for yy in range(s):
                    for xx in range(s):
                        px[x * s + xx, y * s + yy] = col
        img.save(args.png)


if __name__ == "__main__":
    main()
