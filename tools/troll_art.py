"""Draws the troll's body and prints it as the ASCII grid src/physics/troll_art.h holds.

Why a generator, when the ghoul in enemy_art.h was typed by hand: the ghoul is
14x26 and the troll is 52x70. At that size the shading is a function of the
shapes -- light from the upper left, falling off across each limb -- and a
hand-typed grid of 3,640 characters is one where moving an arm means re-shading
it by hand. The shapes below are the source; the grid is their output.

The grid is still what gets reviewed and what the game reads. troll_art.h is
the checked-in copy, written whole by --header from HEADER below, and every
structural rule the simulation depends on (row widths, a palette entry per
character, everything connected to the heart, the gaps that let a limb come off)
is a static_assert there, not a promise here.

Usage:
    python tools/troll_art.py --header        # writes src/physics/troll_art.h
    python tools/troll_art.py                 # prints the ROWS block
    python tools/troll_art.py --png out.png   # also writes an 8x preview

The roles (see troll_art.h):
    'E'          eyes        -- HEAD: lose both and it dies
    'h' 'j' 'k'  chest       -- HEART: the root a limb has to stay joined to
Arms and feet are geometry, not letters, exactly as for the ghoul.
"""

import argparse
import os
import string

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

W, H = 52, 70
BOX_W = 24
BOX_LEFT = (W - BOX_W) // 2  # 14
BOX_RIGHT = BOX_LEFT + BOX_W  # 38, one past the last box column
ARM_TOP = 24
EMPTY = None

# Tone ramps, darkest first. One letter per tone; the outline is 'K'.
RAMPS = {
    "skin": "SPQR",  # grey-brown hide, dark to highlight
    "hair": "ZY",
    "iron": "IJ",
    "cloth": "LN",
    "wood": "WX",
    "bone": "T",
    "eye": "E",
}
# The chest's tones, spelt with their own letters so they can be the HEART.
HEART_FOR = {"S": "k", "P": "h", "Q": "j", "R": "j"}


def ellipse(cx, cy, rx, ry):
    def inside(x, y):
        return ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0

    return inside, (cx - rx, cy - ry, cx + rx, cy + ry)


def rect(x0, y0, x1, y1):
    def inside(x, y):
        return x0 <= x <= x1 and y0 <= y <= y1

    return inside, (x0, y0, x1, y1)


def capsule(ax, ay, bx, by, r0, r1):
    """A limb: a segment from a to b, radius r0 at a tapering to r1 at b."""

    def inside(x, y):
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / L2))
        px, py = ax + t * dx, ay + t * dy
        r = r0 + (r1 - r0) * t
        return (x - px) ** 2 + (y - py) ** 2 <= r * r

    r = max(r0, r1)
    return inside, (min(ax, bx) - r, min(ay, by) - r, max(ax, bx) + r, max(ay, by) + r)


def clip_x(shape, x0, x1):
    inside, bb = shape
    return (lambda x, y: x0 <= x <= x1 and inside(x, y)), bb


# Back to front. Each entry: (material, shape, brightness bias). A shape's own
# extent is what it is shaded against, so an arm in front of the torso is lit
# as an arm and not as a stripe across the chest.
def layers():
    out = []
    # The club, in the right fist -- the leading hand when it faces right -- its
    # head resting on the ground beside the right foot.
    out.append(("wood", capsule(47.5, 54, 47.5, 60, 1.6, 2.4), 0.1))
    out.append(("wood", capsule(47.5, 60, 47.5, 66.5, 3.0, 3.6), 0.1))

    # Legs: short and thick, apart from the hip down so either can be cut off on
    # its own.
    out.append(("skin", capsule(19.5, 52, 19, 63, 4.8, 4.3), -0.05))
    out.append(("skin", capsule(32.0, 52, 32.5, 63, 4.8, 4.3), -0.1))
    out.append(("skin", ellipse(18.5, 67, 4.6, 2.8), -0.1))
    out.append(("skin", ellipse(32.5, 67, 4.6, 2.8), -0.15))

    # Arms, from the shoulder down, kept off the torso by one empty column each
    # side (BOX_LEFT-1 and BOX_RIGHT). Long: the fists hang below the knees.
    L, R = (0, BOX_LEFT - 2), (BOX_RIGHT + 1, W)
    out.append(("skin", clip_x(capsule(8.5, 20, 8, 38, 4.2, 3.4), *L), 0.05))
    out.append(("skin", clip_x(capsule(8, 38, 8.5, 51, 3.6, 4.4), *L), 0.0))
    out.append(("skin", clip_x(ellipse(8.5, 56, 4.4, 4.6), *L), 0.1))
    out.append(("skin", clip_x(capsule(43.5, 20, 44, 38, 4.2, 3.4), *R), 0.0))
    out.append(("skin", clip_x(capsule(44, 38, 43.5, 51, 3.6, 4.4), *R), -0.05))
    out.append(("skin", clip_x(ellipse(43.5, 56, 4.4, 4.6), *R), 0.05))
    # Iron cuffs on the wrists -- the shackles Elden Ring's trolls still wear.
    out.append(("iron", clip_x(rect(4, 45, 12, 48), *L), 0.0))
    out.append(("iron", clip_x(rect(39, 45, 48, 48), *R), 0.0))

    # The hunch: back and shoulders above ARM_TOP, wider than the box and higher
    # than the head, which hangs forward off the front of it.
    out.append(("skin", ellipse(25.5, 14, 17, 11), 0.05))
    out.append(("skin", ellipse(10, 19, 6.5, 6), 0.1))
    out.append(("skin", ellipse(41, 19, 6.5, 6), 0.0))

    # Chest and belly, inside the box from ARM_TOP down. The belly is its own
    # shape so it rounds out under the chest instead of continuing it.
    out.append(("skin", clip_x(ellipse(25.5, 31, 11.5, 10), BOX_LEFT, BOX_RIGHT - 1), 0.05))
    out.append(("skin", clip_x(ellipse(26, 41, 11, 8.5), BOX_LEFT, BOX_RIGHT - 1), 0.0))

    # Loincloth and belt; the flap bridges the legs only above the knee.
    out.append(("cloth", rect(15, 46, 36, 52), 0.0))
    out.append(("cloth", rect(21, 53, 30, 56), -0.05))
    out.append(("iron", rect(15, 45, 36, 46), 0.1))

    # Head: small for the body, low and pushed forward off the hunch.
    out.append(("skin", ellipse(27.5, 13, 6.5, 7.5), 0.2))
    out.append(("skin", ellipse(27.5, 18, 6.0, 3.2), 0.1))  # the jaw
    # Iron collar.
    out.append(("iron", ellipse(27, 22, 9, 2.2), 0.0))
    # Hair: lank, off the crown and down both sides of the face to the collar.
    out.append(("hair", ellipse(27.5, 7, 7.5, 3.4), 0.0))
    out.append(("hair", capsule(21, 7, 19.5, 21, 2.2, 1.5), 0.1))
    out.append(("hair", capsule(34, 7, 35.5, 21, 2.2, 1.5), -0.1))
    return out


def shade(masks, owner, k, x, y, bias):
    """0..1 lightness for pixel (x, y) of layer k, light from the upper left.

    Counts how much of the layer's own shape lies toward the light and away
    from it. Deep inside a shape the two balance and the pixel is the mid tone;
    on the edge facing the light there is nothing of the shape between it and
    the light, so it is lit, and the far edge goes dark. A ball shades like a
    ball and a limb like a limb, whatever its outline.
    """
    m = masks[k]
    R = 4

    def inside(xx, yy):
        return 0 <= xx < W and 0 <= yy < H and m[yy][xx]

    toward = (
        sum(inside(x - i, y - i) for i in range(1, R + 1)) * 0.6
        + sum(inside(x - i, y) for i in range(1, R + 1)) * 0.4
    )
    away = (
        sum(inside(x + i, y + i) for i in range(1, R + 1)) * 0.6
        + sum(inside(x + i, y) for i in range(1, R + 1)) * 0.4
    )
    tone = 0.5 + 0.5 * (away - toward) / R + bias
    # Shadow cast by a shape in front onto this one: a pixel whose upper-left
    # neighbour belongs to a later layer sits in that layer's shadow.
    for dx, dy in ((-1, 0), (0, -1), (-1, -1)):
        nx, ny = x + dx, y + dy
        if 0 <= nx < W and 0 <= ny < H and owner[ny][nx] is not None and owner[ny][nx] > k:
            tone -= 0.3
            break
    return tone


def draw():
    shapes = layers()
    masks = []
    owner = [[None] * W for _ in range(H)]
    for k, (_material, (inside, _bb), _bias) in enumerate(shapes):
        m = [[inside(x, y) for x in range(W)] for y in range(H)]
        masks.append(m)
        for y in range(H):
            for x in range(W):
                if m[y][x]:
                    owner[y][x] = k

    grid = [[EMPTY] * W for _ in range(H)]
    for y in range(H):
        for x in range(W):
            k = owner[y][x]
            if k is None:
                continue
            material, _, bias = shapes[k]
            ramp = RAMPS[material]
            tone = shade(masks, owner, k, x, y, bias)
            # Ordered dither on the hide only, and only near a tone boundary, so
            # a broad surface reads as skin rather than as banded plastic.
            if material == "skin":
                f = tone * len(ramp) - int(tone * len(ramp))
                if abs(f - 0.5) > 0.42:
                    tone += 0.07 if (x + y) % 2 == 0 else -0.07
            i = int(tone * len(ramp))
            grid[y][x] = ramp[max(0, min(len(ramp) - 1, i))]

    # The gap columns. Whatever a shape spilled into them is cleared, from
    # ARM_TOP down, so the arms hang from the shoulder alone.
    for y in range(ARM_TOP, H):
        grid[y][BOX_LEFT - 1] = EMPTY
        grid[y][BOX_RIGHT] = EMPTY
    # And between the legs below the loincloth's flap.
    for y in range(57, H):
        for x in range(24, 28):
            grid[y][x] = EMPTY

    def put(points, c):
        for x, y in points:
            grid[y][x] = c

    # Features painted last, onto the head. Brow, deep-set eyes, a flat nose,
    # a wide mouth with the lower tusks over the lip.
    put([(x, 11) for x in range(23, 33)], "S")
    put([(24, 12), (25, 12), (30, 12), (31, 12)], "E")
    put([(23, 12), (26, 12), (29, 12), (32, 12), (27, 12), (28, 12)], "S")
    put([(27, 14), (28, 14)], "R")
    put([(27, 15), (28, 15)], "S")
    put([(x, 18) for x in range(23, 33)], "Z")
    put([(24, 17), (31, 17), (24, 16), (31, 16)], "T")
    # Muscle: the line under the pecs, the navel, the knees and the knuckles.
    put(
        [(x, 36) for x in (18, 19, 20, 21, 22, 23)] + [(x, 36) for x in (28, 29, 30, 31, 32, 33)],
        "S",
    )
    put([(25, 34), (26, 34)], "S")
    put([(26, 42)], "S")
    put([(17, 59), (18, 59), (19, 59)], "P")
    put([(31, 59), (32, 59), (33, 59)], "P")
    put([(6, 58), (8, 58), (10, 58)], "S")
    put([(41, 58), (43, 58), (45, 58)], "S")
    # Studs on the club's head.
    put([(46, 61), (49, 63), (46, 65), (49, 67)], "J")

    # Outline: every body pixel with an empty 4-neighbour.
    out = [row[:] for row in grid]
    for y in range(H):
        for x in range(W):
            if grid[y][x] is EMPTY:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if not (0 <= nx < W and 0 <= ny < H) or grid[ny][nx] is EMPTY:
                    out[y][x] = "K"
                    break

    # The heart: the middle of the chest, in its own letters.
    for y in range(H):
        for x in range(W):
            c = out[y][x]
            if c in HEART_FOR and ((x - 25.5) / 7.5) ** 2 + ((y - 32) / 6.5) ** 2 <= 1.0:
                out[y][x] = HEART_FOR[c]
    return ["".join(c if c is not EMPTY else "." for c in row) for row in out]


# (letter, ARGB, what it is): the header's PALETTE, comments included, and
# what --png paints the preview with.
PALETTE = [
    ("K", 0xFF15110F, "outline"),
    ("S", 0xFF3E3430, "hide, shadow"),
    ("P", 0xFF5E5049, "hide, mid"),
    ("Q", 0xFF7E6C5F, "hide, lit"),
    ("R", 0xFF9C8875, "hide, highlight"),
    ("k", 0xFF3E3430, "chest, shadow -- HEART"),
    ("h", 0xFF5E5049, "chest, mid -- HEART"),
    ("j", 0xFF7E6C5F, "chest, lit -- HEART"),
    ("Z", 0xFF1B1716, "hair"),
    ("Y", 0xFF332C28, "hair, lit"),
    ("I", 0xFF3B3E44, "iron"),
    ("J", 0xFF6E747C, "iron, lit; also the club's studs"),
    ("L", 0xFF4A3322, "loincloth"),
    ("N", 0xFF6B4A2F, "loincloth, lit"),
    ("W", 0xFF4E3720, "club"),
    ("X", 0xFF74552F, "club, lit"),
    ("T", 0xFFDDD2B4, "tusks"),
    ("E", 0xFFFF7A2E, "eyes -- HEAD"),
]


HEADER_PATH = os.path.join(ROOT, "src", "physics", "troll_art.h")


def header_rows(rows):
    """ROWS' initialiser lines, broken where clang-format breaks them.

    A row too wide for the 100-column limit becomes adjacent string literals,
    which C++ joins back into one: each piece as long as fits, with at least one
    character left for the last. That is the split clang-format makes, so a
    regenerated header is already formatted and `just code_cleanup` never turns
    it into a diff of rewrapped rows. The fish's 94-wide rows need it; the
    troll's 52 do not.
    """
    out = []
    for r in rows:
        while len(f'    "{r}",') > 100:
            cut = min(100 - len('    ""'), len(r) - 1)
            out.append(f'    "{r[:cut]}"')
            r = r[cut:]
        out.append(f'    "{r}",')
    return out


def write_header(path, template, rows, palette, **fields):
    """Fills a body's header template and writes it. Shared with fish_art.py."""
    text = template.substitute(
        ROWS="\n".join(header_rows(rows)),
        PALETTE="\n".join(f"    {{'{c}', 0x{argb:08X}}},  // {what}" for c, argb, what in palette),
        **fields,
    )
    # LF, as player_sheet.py writes its header: the form git stores text in.
    with open(path, "w", newline="\n") as f:
        f.write(text)
    print(f"wrote {os.path.relpath(path, ROOT)}")


# The whole of troll_art.h, prose and static_asserts included, with $-fields for
# what the drawing above decides. string.Template rather than str.format,
# because C++ is braces all the way down.
HEADER = string.Template(
    r'''// GENERATED by tools/troll_art.py - do not edit.
//
// Regenerate with:  python tools/troll_art.py --header
//
#pragma once
#include <cstdint>
#include "body_art.h"

// The troll's body, one character per pixel and one pixel per world cell, read
// by the same rules as the ghoul's (body_art.h) for the reasons enemy_art.h
// gives for keeping a body's art in src/physics/ at all.
//
// After the trolls of Elden Ring: a hunched giant with a small head sunk
// forward off a mass of back and shoulder, arms long enough that the fists hang
// below the knees, iron shackles still on its wrists and neck, and a club in the
// leading hand. 52x70 against the player's 14x26 frame -- a body nearly three
// times the player's height, which is the whole of "imposing" at four screen
// pixels per cell.
//
// Generated by tools/troll_art.py from shapes, because at this size the shading
// is a function of the shapes and hand-typing 3,640 characters makes every edit
// a re-shade. The generator writes this whole file, comments included, so
// change them there; the static_asserts at the bottom are what decide whether
// the result is still a body.
//
// The roles, as for the ghoul:
//  - 'E', the eyes, are the HEAD. Their inner pixels are five cells apart, so
//    one arrow's bite (radius 2) never reaches both: a head kill is one
//    well-placed shot per eye, not one shot between them as for the ghoul. A
//    body this size should not drop to a single lucky arrow.
//  - 'h', 'j' and 'k', the middle of the chest, are the HEART and the root of
//    the severing fill.
//  - Arms are everything outside the box's columns from ARM_TOP down; feet are
//    what is inside them in the bottom FOOT_ROWS rows.
//
// The empty column each side of the torso from ARM_TOP down, and the four empty
// columns between the legs below the loincloth, are load-bearing exactly as the
// ghoul's gaps are: an arm hangs from the shoulder alone and a leg from the hip
// alone, so cutting through the shoulder brings the whole arm down. The
// difference is thickness. A troll's arm is eight cells across against an
// arrow's five-cell bite, so a limb does not come off to one arrow -- it is
// chipped, and comes off to the second or third in the same place. That is the
// local part of "comes apart where it is hit" at this size.
namespace troll_art {

inline constexpr int W = $W;
inline constexpr int H = $H;

inline constexpr const char* ROWS[H] = {
$ROWS
};

// Its own palette rather than the game's: nothing in assets/palette.gpl is a
// hide. Grey-brown, warm in the light and cold in the shade, so it reads as
// skin against both the forest's greens and the cave's greys, and the sand it
// leaves behind is visibly its flesh, not the Sand row's yellow.
using body_art::Ink;
inline constexpr Ink PALETTE[] = {
$PALETTE
};
inline constexpr int PALETTE_COUNT = static_cast<int>(sizeof(PALETTE) / sizeof(PALETTE[0]));

inline constexpr int FOOT_ROWS = 3;
inline constexpr int ARM_TOP = $ARM_TOP;
// The box is the torso and legs: 24 columns, centred. species::TROLL checks it
// against its own box width.
inline constexpr int BOX_LEFT = $BOX_LEFT;
inline constexpr int BOX_RIGHT = W - BOX_LEFT;

inline constexpr body_art::Art ART{ROWS, W,     H,        PALETTE, PALETTE_COUNT,
                                   "E",  "hjk", BOX_LEFT, ARM_TOP, FOOT_ROWS};

static_assert(body_art::well_formed(ART),
              "troll_art::ROWS has a row of the wrong width or a character with no "
              "PALETTE entry");
static_assert(body_art::count_where(ART, &body_art::Art::is_head) >= 2 &&
                  body_art::count_where(ART, &body_art::Art::is_heart) > 0,
              "the troll needs its eyes ('E') and a heart ('h'/'j'/'k'), or it is "
              "born dead");
static_assert(body_art::count_where(ART, &body_art::Art::is_foot) > 0,
              "the troll has nothing in its FOOT_ROWS inside the box, so it is born "
              "footless and collapses on its first hit");
static_assert(body_art::count_where(ART, &body_art::Art::is_arm) > 0,
              "the troll has no pixels outside the box below ARM_TOP, so it is born "
              "armless and can never slam");
static_assert(body_art::connected<W * H>(ART),
              "some troll pixel is not 8-connected to the heart; it would fall off "
              "as sand the first time the troll is hit anywhere");
static_assert(body_art::stands_on_bottom_row(ART), "the troll art floats: its bottom row is empty");

// The gaps that let a limb come off, checked rather than trusted: a pixel in
// either gap column below ARM_TOP joins an arm to the ribs down its length, and
// a shot through the shoulder would take a bite and leave the arm on.
constexpr bool arms_hang_free() {
    for (int y = ARM_TOP; y < H; ++y)
        if (ART.is_body(BOX_LEFT - 1, y) || ART.is_body(BOX_RIGHT, y)) return false;
    return true;
}
static_assert(arms_hang_free(),
              "a troll pixel fills the gap column between an arm and the torso, so "
              "that arm can no longer be severed at the shoulder");

inline constexpr int PIXEL_COUNT = body_art::count_where(ART, &body_art::Art::is_body);

}  // namespace troll_art
'''
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png")
    ap.add_argument("--header", action="store_true", help="write the header rather than print ROWS")
    args = ap.parse_args()
    rows = draw()
    if args.header:
        write_header(
            HEADER_PATH, HEADER, rows, PALETTE, W=W, H=H, ARM_TOP=ARM_TOP, BOX_LEFT=BOX_LEFT
        )
    else:
        for r in rows:
            print(f'    "{r}",')
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
