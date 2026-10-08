"""Draws the fish's body and prints it as the ASCII grid src/physics/fish_art.h holds.

A generator for the troll's reason (tools/troll_art.py): at 94x84 the shading
is a function of the shapes, and a hand-typed grid of nearly eight thousand
characters is one where moving the arm means re-shading it by hand. The shapes
below are the source; the grid is their output, and fish_art.h is the checked-in
copy whose static_asserts decide whether it is still a body.

The look is the player owl's rather than the troll's: no outline, flat tone
ramps of three or four steps, and the form carried by where one tone meets the
next. A largemouth bass on human legs, head tilted up, with a human arm growing
out of the top of its head and reaching forward over the snout so the hand
hangs in front of the mouth -- the anglerfish's lure, if the lure were a hand.

Usage:
    python tools/fish_art.py                 # prints the ROWS block
    python tools/fish_art.py --png out.png   # also writes an 8x preview

The roles (see fish_art.h):
    'E' 'O'          the eye        -- HEAD: lose it all and it dies
    'a' 'b' ... 'g'  the mid-flank  -- HEART: what a limb has to stay joined to
    'p' 'q' 'r' 's'  the lure arm   -- the ARM, by letter: it grows out of the top
                                       of the head, where no box column can say
                                       "this is an arm" (see body_art.h)
The legs are geometry, as for every other body.
"""

import argparse
import math

W, H = 94, 84
BOX_W = 30
BOX_LEFT = (W - BOX_W) // 2  # 32
BOX_RIGHT = BOX_LEFT + BOX_W  # 62, one past the last box column
HIP_ROW = 53  # inside the box columns, legs from here down
LEG_SPLIT = 47  # rear leg left of it, front leg from it
SHOULDER = (60, 22)  # where the arm turns; the top of the head
EMPTY = None

# The fish lies along its own axis, tilted head-up by TILT about PIVOT, the
# way the reference stands: nose raised, as if scenting the air.
PIVOT = (47.0, 37.0)
TILT = math.radians(10)


def to_local(x, y):
    dx, dy = x - PIVOT[0], y - PIVOT[1]
    c, s = math.cos(TILT), math.sin(TILT)
    return dx * c - dy * s, dx * s + dy * c


def to_world(u, v):
    c, s = math.cos(TILT), math.sin(TILT)
    return PIVOT[0] + u * c + v * s, PIVOT[1] - u * s + v * c


def ellipse(cx, cy, rx, ry):
    return lambda x, y: ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0


def capsule(ax, ay, bx, by, r0, r1):
    """A limb: a segment from a to b, radius r0 at a tapering to r1 at b."""

    def inside(x, y):
        dx, dy = bx - ax, by - ay
        L2 = dx * dx + dy * dy
        t = max(0.0, min(1.0, ((x - ax) * dx + (y - ay) * dy) / L2))
        px, py = ax + t * dx, ay + t * dy
        r = r0 + (r1 - r0) * t
        return (x - px) ** 2 + (y - py) ** 2 <= r * r

    return inside


def polygon(points):
    def inside(x, y):
        n, hit = len(points), False
        j = n - 1
        for i in range(n):
            xi, yi = points[i]
            xj, yj = points[j]
            if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                hit = not hit
            j = i
        return hit

    return inside


def union(*shapes):
    return lambda x, y: any(s(x, y) for s in shapes)


def local(shape):
    """A shape written in the fish's own (u, v) frame, sampled in the world's."""
    return lambda x, y: shape(*to_local(x, y))


# --- the shapes ---------------------------------------------------------------
#
# The fish in its own frame: u along the body from tail (-) to snout (+), v
# across it, down positive. The pixel centre is sampled, so a shape's edge is
# where a cell's centre crosses it.

BODY = local(
    union(
        ellipse(1, 0.5, 27, 14.5),  # the deep, heavy middle a bass carries
        ellipse(17, 0.5, 14, 11.5),  # the head
        ellipse(25, 2.5, 7, 5),  # the jaw, jutting past the upper lip
        capsule(-20, 1, -33, 1, 7.5, 4.6),  # the wrist of the tail
    )
)
TAIL = local(
    polygon(
        [
            (-31, -3),
            (-38, -10),
            (-43.5, -15.5),
            (-46.5, -14.5),
            (-44.5, -6),
            (-42, 1.5),
            (-44.5, 9),
            (-46.5, 17.5),
            (-43.5, 18.5),
            (-38, 12),
            (-31, 5),
        ]
    )
)
# Spiny dorsal: a row of points, each a little lower toward the back.
_spines = [(-7, -12.5)]
for i, u in enumerate(range(-6, 13, 3)):
    _spines += [(u, -21.5 + i * 0.6), (u + 1.5, -16.5 + i * 0.3)]
_spines += [(13, -11)]
SPINY = local(polygon(_spines))
SOFT = local(polygon([(-22, -10), (-19, -18.5), (-13, -20), (-8, -18), (-6, -13)]))
ANAL = local(polygon([(-23, 8), (-21, 16.5), (-16, 17), (-12, 12), (-14, 9)]))
PECTORAL = local(polygon([(10, 4), (2, 1), (-2, 4), (1, 7.5), (9, 7)]))
PELVIC = local(polygon([(16, 10), (17, 18), (20, 17.5), (21, 10)]))
EYE_CENTRE = to_world(20.5, -3.5)


def leg(hx, near):
    """A human leg from the hip at (hx, 46) down to a foot pointing forward."""
    kx = hx + 3  # the knee, a little forward: a body standing with knees soft
    return union(
        capsule(hx, 45, kx, 64, 5.4 if near else 5.0, 3.6),  # thigh
        ellipse(hx + 1.6, 54, 4.4, 7.5),  # the quadriceps' swell, at the front
        capsule(kx, 64, kx - 2, 79, 3.2, 1.9),  # shin
        ellipse(kx - 2.6, 69.5, 3.0, 5.0),  # the calf, behind the shin
        ellipse(kx - 2.4, 81.2, 2.4, 2.3),  # heel
        capsule(kx - 2.2, 81.6, kx + 5.5, 82.6, 2.1, 1.3),  # foot, toes forward
    )


REAR_HIP_X, FRONT_HIP_X = 37, 52
REAR_LEG = leg(REAR_HIP_X, False)
FRONT_LEG = leg(FRONT_HIP_X, True)

# The arm: up out of the head and forward, elbow high, then down to a hand that
# hangs limp at the wrist with its fingers dangling, in front of the mouth.
SX, SY = SHOULDER
ELBOW = (70.5, 7.5)
WRIST = (84.5, 18.5)
ARM = union(
    capsule(SX, SY + 1, *ELBOW, 3.5, 2.6),  # upper arm
    ellipse(SX + 1.2, SY - 2.6, 3.8, 3.4),  # deltoid, where it leaves the head
    capsule(*ELBOW, *WRIST, 2.8, 1.7),  # forearm
    capsule(WRIST[0] + 0.5, WRIST[1] + 1, WRIST[0] + 2, WRIST[1] + 7, 2.0, 2.4),  # palm
    capsule(WRIST[0] + 3.4, WRIST[1] + 3, WRIST[0] + 5.6, WRIST[1] + 7.5, 0.8, 0.7),  # thumb
    capsule(WRIST[0] + 3.6, WRIST[1] + 8, WRIST[0] + 4.4, WRIST[1] + 13.5, 0.6, 0.55),  # fingers
    capsule(WRIST[0] + 2.2, WRIST[1] + 9, WRIST[0] + 2.6, WRIST[1] + 14.5, 0.6, 0.55),
    capsule(WRIST[0] + 0.8, WRIST[1] + 9, WRIST[0] + 0.9, WRIST[1] + 13.5, 0.6, 0.55),
    capsule(WRIST[0] - 0.5, WRIST[1] + 8, WRIST[0] - 0.8, WRIST[1] + 11.5, 0.6, 0.55),
)


# --- shading ------------------------------------------------------------------


def mask_of(shape):
    return [[bool(shape(x, y)) for x in range(W)] for y in range(H)]


def lit(m, x, y, R=4):
    """-1..1: how much of the mask lies away from the light (upper left) versus
    toward it. On the lit edge there is nothing of the shape between the pixel and
    the light; on the far edge, everything is."""

    def inside(xx, yy):
        return 0 <= xx < W and 0 <= yy < H and m[yy][xx]

    toward = sum(inside(x - i, y - i) * 0.6 + inside(x - i, y) * 0.4 for i in range(1, R + 1))
    away = sum(inside(x + i, y + i) * 0.6 + inside(x + i, y) * 0.4 for i in range(1, R + 1))
    return (away - toward) / R


def draw():
    grid = [[EMPTY] * W for _ in range(H)]

    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H:
            grid[y][x] = c

    # --- the legs, the far one behind the fish and the near one in front.
    # Four skin tones, light from the upper left; a soft dither only at a tone
    # boundary, as the owl's feathers have it, so muscle reads as rounded.
    skin = "SPQR"

    def paint_leg(shape, bias, from_row=0):
        m = mask_of(shape)
        for y in range(from_row, H):
            for x in range(W):
                if not m[y][x]:
                    continue
                t = 0.5 + 0.55 * lit(m, x, y) + bias
                f = t * 4 - int(t * 4)
                if abs(f - 0.5) > 0.46:
                    t += 0.06 if (x + y) % 2 == 0 else -0.06
                grid[y][x] = skin[max(0, min(3, int(t * 4)))]

    paint_leg(REAR_LEG, -0.22)

    # --- the arm, before the fish, so the head covers its root and it grows
    # out of the head rather than being stuck on top of it.
    arm_mask = mask_of(ARM)
    for y in range(H):
        for x in range(W):
            if arm_mask[y][x]:
                t = 0.5 + 0.55 * lit(arm_mask, x, y, 3) + 0.05
                grid[y][x] = "spqr"[max(0, min(3, int(t * 4)))]

    # --- the fins, behind the body.
    fins = {}
    for name, shape in (("tail", TAIL), ("spiny", SPINY), ("soft", SOFT), ("anal", ANAL)):
        m = mask_of(shape)
        for y in range(H):
            for x in range(W):
                if m[y][x]:
                    fins[(x, y)] = name

    for (x, y), name in fins.items():
        u, v = to_local(x, y)
        if name == "tail":
            # Rays fanning from the wrist of the tail, the trailing edge darker.
            ang = math.atan2(v - 1, -(u + 30))
            ray = int(ang * 9) % 2 == 0
            c = "H" if ray else "I"
            if u < -44.2:
                c = "B"
            grid[y][x] = c
        elif name == "spiny":
            # The spines are the dark ribs; the membrane between them pale.
            c = "B" if round(u) % 3 == 0 else "H"
            if v > -14.5:
                c = "B"
            grid[y][x] = c
        else:
            c = "H" if (round(u) + (0 if name == "soft" else 1)) % 2 == 0 else "I"
            grid[y][x] = c

    # --- the body: countershaded, dark olive back to cream belly. Each column's
    # tone is its height between that column's top and bottom of the body, so
    # the bands follow the fish's own profile, then nudged by the light.
    body_mask = mask_of(BODY)
    tops, bots = {}, {}
    for x in range(W):
        ys = [y for y in range(H) if body_mask[y][x]]
        if ys:
            tops[x], bots[x] = ys[0], ys[-1]
    for y in range(H):
        for x in range(W):
            if not body_mask[y][x]:
                continue
            span = max(1, bots[x] - tops[x])
            vn = (y - tops[x]) / span + 0.12 * lit(body_mask, x, y, 3)
            u, v = to_local(x, y)
            if vn < 0.14:
                c = "A"
            elif vn < 0.34:
                c = "B"
            elif vn < 0.52:
                c = "C"
            elif vn < 0.64:
                c = "D"
            elif vn < 0.9:
                c = "G" if vn < 0.82 else "F"
            else:
                c = "F"
            # Scales: a sparse diamond lattice one tone down across the olive
            # flank, so it reads as fish skin rather than a painted band.
            if c in "CD" and (x + 2 * y) % 5 == 0 and (x - 2 * y) % 5 == 0:
                c = "B" if c == "C" else "C"
            grid[y][x] = c

    # The lateral band: a jagged chain of dark blotches from the gill to the
    # tail, the mark that makes a largemouth a largemouth.
    for i, u in enumerate(range(-33, 13, 3)):
        v = 0.5 + (0.7 if i % 2 else -0.6)
        rx, ry = (2.2, 2.0) if i % 3 else (1.7, 1.5)
        for y in range(H):
            for x in range(W):
                if not body_mask[y][x]:
                    continue
                lu, lv = to_local(x, y)
                if ((lu - u) / rx) ** 2 + ((lv - v) / ry) ** 2 <= 1.0:
                    grid[y][x] = "A" if grid[y][x] in "BC" else "B"

    # The gill cover: a curved seam behind the eye, and the head in front of it
    # a shade lighter -- the bone of the cheek under the skin.
    for y in range(H):
        for x in range(W):
            if not body_mask[y][x]:
                continue
            u, v = to_local(x, y)
            seam = 11.5 + 0.035 * v * v
            if abs(u - seam) < 0.55 and -9 < v < 10.5:
                grid[y][x] = "B"
            elif u > seam + 0.6 and grid[y][x] == "C" and v > -5:
                grid[y][x] = "D"

    # The pectoral and pelvic fins, in front of the flank.
    for shape, light in ((PECTORAL, "I"), (PELVIC, "I")):
        m = mask_of(shape)
        for y in range(H):
            for x in range(W):
                if m[y][x]:
                    u, _ = to_local(x, y)
                    grid[y][x] = light if round(u) % 2 else "H"

    # The mouth: the jaw line runs back past the eye -- the "largemouth" -- with
    # the lip under it a pale edge and the corner tucked down.
    for x in range(W):
        for y in range(H):
            if not body_mask[y][x]:
                continue
            u, v = to_local(x, y)
            line = 2.6 - 0.06 * (u - 30)
            if 13 < u < 32 and abs(v - line) < 0.5:
                grid[y][x] = "M"
            elif 15 < u < 32 and 0.5 < v - line < 1.5:
                grid[y][x] = "F"
    # The far leg again from the hips down, where it is leg and nothing else:
    # the belly overhangs it above the hips, never below.
    paint_leg(REAR_LEG, -0.22, HIP_ROW)
    # The near leg, in front of the belly.
    paint_leg(FRONT_LEG, 0.0)

    # The eye, last: a ring of red-orange round a black pupil, four across.
    ex, ey = round(EYE_CENTRE[0] - 1.5), round(EYE_CENTRE[1] - 1.5)
    for dy, row in enumerate([".EE.", "EOOE", "EOOE", ".EE."]):
        for dx, c in enumerate(row):
            if c != ".":
                put(ex + dx, ey + dy, c)
    # A pale socket rim on the upper side, so the eye sits in the head.
    for dx, dy in ((0, -1), (1, -1), (2, -1), (3, -1), (-1, 0), (4, 0)):
        x, y = ex + dx, ey + dy
        if grid[y][x] in ("A", "B", "C"):
            grid[y][x] = "D"

    # --- the cuts that make the limbs come off -------------------------------
    #
    # Between the legs, from the hips down: an empty column each side of the
    # split, so either leg hangs from its hip alone.
    for y in range(HIP_ROW, H):
        for x in (LEG_SPLIT - 1, LEG_SPLIT):
            grid[y][x] = EMPTY
    # Nothing but leg inside the box from the hips down: a fin there would be
    # read as a third leg and swing with the stride.
    for y in range(HIP_ROW, H):
        for x in range(BOX_LEFT, BOX_RIGHT):
            if grid[y][x] is not EMPTY and grid[y][x] not in skin:
                grid[y][x] = EMPTY
    # The arm joins the head only at its root: any other body pixel touching it
    # further than ROOT cells from the shoulder is cleared, so a shot through
    # the shoulder takes the whole arm off and the hand never fuses to the lip.
    ROOT = 3.0
    for y in range(H):
        for x in range(W):
            c = grid[y][x]
            if c is EMPTY or c in "pqrs":
                continue
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    nx, ny = x + dx, y + dy
                    if not (0 <= nx < W and 0 <= ny < H) or grid[ny][nx] is EMPTY:
                        continue
                    if grid[ny][nx] in "pqrs" and math.hypot(nx - SX, ny - SY) > ROOT:
                        grid[y][x] = EMPTY

    # --- the heart: the middle of the flank, in its own letters.
    heart = {"A": "a", "B": "b", "C": "c", "D": "d", "F": "f", "G": "g"}
    for y in range(H):
        for x in range(W):
            u, v = to_local(x, y)
            if grid[y][x] in heart and (u / 9) ** 2 + ((v - 1) / 6) ** 2 <= 1.0:
                grid[y][x] = heart[grid[y][x]]

    return ["".join(c if c is not EMPTY else "." for c in row) for row in grid]


PALETTE = {
    # The bass: olive back, a dark band, cream belly.
    "A": 0xFF263019,  # back, darkest; the lateral blotches
    "B": 0xFF3B4A26,  # back; fin spines and seams
    "C": 0xFF5A6A36,  # flank
    "D": 0xFF828C4C,  # flank, lit; the cheek
    "G": 0xFFC8C196,  # belly
    "F": 0xFF9E9A72,  # belly, in its own shadow; the lip
    "H": 0xFF6A6A3C,  # fin
    "I": 0xFF9A955E,  # fin, between the rays
    "M": 0xFF1C1810,  # the mouth's line
    "a": 0xFF263019,  # -- HEART, the same tones in the middle of the flank
    "b": 0xFF3B4A26,
    "c": 0xFF5A6A36,
    "d": 0xFF828C4C,
    "g": 0xFFC8C196,
    "f": 0xFF9E9A72,
    "E": 0xFFC4502A,  # the eye's red ring -- HEAD
    "O": 0xFF120E0A,  # the pupil -- HEAD
    # The man: one skin ramp for the legs and, under its own letters, the arm.
    "S": 0xFF6E5444,
    "P": 0xFF9A7B62,
    "Q": 0xFFC2A384,
    "R": 0xFFDEC6A4,
    "s": 0xFF6E5444,
    "p": 0xFF9A7B62,
    "q": 0xFFC2A384,
    "r": 0xFFDEC6A4,
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
