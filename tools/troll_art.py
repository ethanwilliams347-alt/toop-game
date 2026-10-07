"""Draws the troll's body and prints it as the ASCII grid src/physics/troll_art.h holds.

Why a generator, when the ghoul in enemy_art.h was typed by hand: the ghoul is
14x26 and the troll is 52x70. At that size the shading is a function of the
shapes -- light from the upper left, falling off across each limb -- and a
hand-typed grid of 3,640 characters is one where moving an arm means re-shading
it by hand. The shapes below are the source; the grid is their output.

The grid is still what gets reviewed and what the game reads. troll_art.h is
the checked-in copy, and every structural rule the simulation depends on (row
widths, a palette entry per character, everything connected to the heart, the
gaps that let a limb come off) is a static_assert there, not a promise here.

Usage:
    python tools/troll_art.py                 # prints the ROWS block
    python tools/troll_art.py --png out.png   # also writes an 8x preview

The roles (see troll_art.h):
    'E'          eyes        -- HEAD: lose both and it dies
    'h' 'j' 'k'  chest       -- HEART: the root a limb has to stay joined to
Arms and feet are geometry, not letters, exactly as for the ghoul.
"""
import argparse
import math

W, H = 52, 70
BOX_W = 24
BOX_LEFT = (W - BOX_W) // 2          # 14
BOX_RIGHT = BOX_LEFT + BOX_W         # 38, one past the last box column
ARM_TOP = 24
EMPTY = None

# Tone ramps, darkest first. One letter per tone; the outline is 'K'.
RAMPS = {
    "skin":  "SPQR",   # grey-brown hide, dark to highlight
    "hair":  "ZY",
    "iron":  "IJ",
    "cloth": "LN",
    "wood":  "WX",
    "bone":  "T",
    "eye":   "E",
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
    out.append(("skin", ellipse(27.5, 18, 6.0, 3.2), 0.1))   # the jaw
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

    toward = sum(inside(x - i, y - i) for i in range(1, R + 1)) * 0.6 + \
        sum(inside(x - i, y) for i in range(1, R + 1)) * 0.4
    away = sum(inside(x + i, y + i) for i in range(1, R + 1)) * 0.6 + \
        sum(inside(x + i, y) for i in range(1, R + 1)) * 0.4
    l = 0.5 + 0.5 * (away - toward) / R + bias
    # Shadow cast by a shape in front onto this one: a pixel whose upper-left
    # neighbour belongs to a later layer sits in that layer's shadow.
    for dx, dy in ((-1, 0), (0, -1), (-1, -1)):
        nx, ny = x + dx, y + dy
        if 0 <= nx < W and 0 <= ny < H and owner[ny][nx] is not None and owner[ny][nx] > k:
            l -= 0.3
            break
    return l


def draw():
    shapes = layers()
    masks = []
    owner = [[None] * W for _ in range(H)]
    for k, (material, (inside, bb), bias) in enumerate(shapes):
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
            l = shade(masks, owner, k, x, y, bias)
            # Ordered dither on the hide only, and only near a tone boundary, so
            # a broad surface reads as skin rather than as banded plastic.
            if material == "skin":
                f = l * len(ramp) - int(l * len(ramp))
                if abs(f - 0.5) > 0.42:
                    l += 0.07 if (x + y) % 2 == 0 else -0.07
            i = int(l * len(ramp))
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
        for (x, y) in points:
            grid[y][x] = c

    # Features painted last, onto the head. Brow, deep-set eyes, a flat nose,
    # a wide mouth with the lower tusks over the lip.
    put([(x, 11) for x in range(23, 33)], "S")
    put([(24, 12), (25, 12), (30, 12), (31, 12)], "E")
    put([(23, 12), (26, 12), (29, 12), (32, 12), (27, 12), (28, 12)], "S")
    put([(27, 14), (28, 14)], "R"); put([(27, 15), (28, 15)], "S")
    put([(x, 18) for x in range(23, 33)], "Z")
    put([(24, 17), (31, 17), (24, 16), (31, 16)], "T")
    # Muscle: the line under the pecs, the navel, the knees and the knuckles.
    put([(x, 36) for x in (18, 19, 20, 21, 22, 23)] + [(x, 36) for x in (28, 29, 30, 31, 32, 33)], "S")
    put([(25, 34), (26, 34)], "S")
    put([(26, 42)], "S")
    put([(17, 59), (18, 59), (19, 59)], "P"); put([(31, 59), (32, 59), (33, 59)], "P")
    put([(6, 58), (8, 58), (10, 58)], "S"); put([(41, 58), (43, 58), (45, 58)], "S")
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


PALETTE = {
    "K": 0xFF15110F,
    "S": 0xFF3E3430, "P": 0xFF5E5049, "Q": 0xFF7E6C5F, "R": 0xFF9C8875,
    "k": 0xFF3E3430, "h": 0xFF5E5049, "j": 0xFF7E6C5F,
    "Z": 0xFF1B1716, "Y": 0xFF332C28,
    "I": 0xFF3B3E44, "J": 0xFF6E747C,
    "L": 0xFF4A3322, "N": 0xFF6B4A2F,
    "W": 0xFF4E3720, "X": 0xFF74552F,
    "T": 0xFFDDD2B4,
    "E": 0xFFFF7A2E,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--png")
    args = ap.parse_args()
    rows = draw()
    for r in rows:
        print(f'    "{r}",')
    if args.png:
        from PIL import Image
        s = 8
        img = Image.new("RGB", (W * s, H * s), (40, 32, 64))
        px = img.load()
        for y, r in enumerate(rows):
            for x, c in enumerate(r):
                if c == ".":
                    continue
                argb = PALETTE[c]
                col = ((argb >> 16) & 255, (argb >> 8) & 255, argb & 255)
                for yy in range(s):
                    for xx in range(s):
                        px[x * s + xx, y * s + yy] = col
        img.save(args.png)


if __name__ == "__main__":
    main()
