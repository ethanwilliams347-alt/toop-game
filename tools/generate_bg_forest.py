"""Generates the extended 688x288 backdrop layer set for bg_forest/.

This is a newly generated, offline pixel-art backdrop generator extending the
visual style of assets/ to a 688x288 canvas:
- Native resolution: 688 x 288 px
- 9 parallax layers matching bg_forest's depth breakdown, factor ladder,
  and back-to-front drawing order.
- Standard-library-only implementation with strictly deterministic, offline
  pixel generation using standard trigonometry and Bayer ordered-dither patterns.
- Proper layer separation utilizing the MAGENTA (0xFF, 0x00, 0xFF) color key.
- Flat uniform runs around parallax scroll-split zones at rows 158..170 and
  196..204 to prevent shearing artifacts across the horizontal scroll cuts
  at row 164 and row 200.
- All colors mapped via `color_of(name)` lookup to prevent out-of-palette drift,
  satisfying strict automated palette validator checks.
"""

import os
import shutil
import sys
import math

# Derived REPO_ROOT from the current file's directory.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, 'tools'))
from pixel_art import (LEGEND_EMPTY, LEGEND_WALL, assert_legend_matches_header,
                       write_bmp, write_png, color_of, dither_mix)

DST_DIR = os.path.join(REPO_ROOT, "assets", "bg_forest")
BUILD_DST_DIR = os.path.join(REPO_ROOT, "build", "Release", "assets", "bg_forest")

TARGET_W = 688
TARGET_H = 288

# Floor row for material map: scaled to row 264
FLOOR_ROW = 264

# Only literal tuple permitted for color keying
MAGENTA = (0xFF, 0x00, 0xFF)


def make_empty():
    """Initializes a transparent layer canvas of target dimensions."""
    return [[MAGENTA for _ in range(TARGET_W)] for _ in range(TARGET_H)]


# -------------------------------------------------------------
# 09_sky (opaque, parallax 0.04)
# -------------------------------------------------------------
def generate_sky():
    img = make_empty()
    color_deep = color_of('sky_deep')
    color_horizon = color_of('sky_horizon')
    color_star = color_of('star')

    # The gradient reaches `sky_horizon` at the ground plane's own top edge, not
    # further down the frame.
    #
    # Anchored lower, the ramp is still passing through the mountain band when it
    # gets there, so the silhouette and the sky behind it are the same value and
    # only the rim crest reads -- which on its own looks like a stray line rather
    # than a mountain. The palette's own note says mountain_rim is authored to sit
    # brighter than the sky at the mountains' height, and that relationship assumes
    # the sky has already arrived at sky_horizon by the time it reaches them.
    #
    # The rows below the horizon are covered by the ground plane at every column, so
    # the flat tail is never seen. It is painted anyway because this is the one
    # opaque layer and a magenta pixel in it would key through to nothing.
    HORIZON_ROW = 150

    for y in range(TARGET_H):
        for x in range(TARGET_W):
            if y < HORIZON_ROW:
                t = y / float(HORIZON_ROW)
                img[y][x] = dither_mix(x, y, color_deep, color_horizon, t)
            else:
                img[y][x] = color_horizon

    # Stars stamped from an explicit counter rather than tested for with a hash
    # over (x, y).
    #
    # A scatter-by-hash makes the count a property of arithmetic nobody checks, and
    # an empty sky looks like a design choice. A condition like
    # `(x * A + y * B + C) % M == K` can be unreachable outright: if A, B and M
    # share a factor that C does not, the expression is confined to one residue
    # class and matches nothing.
    #
    # Counting the stars out makes the count readable at the loop. The column stride
    # is coprime with the width, so the columns are distinct and no two stars land
    # on the same pixel.
    for i in range(40):
        sx = (i * 149 + 37) % TARGET_W
        sy = 6 + (i * 47 + 11) % 78
        img[sy][sx] = color_star

    return img


# -------------------------------------------------------------
# 08_ground (colour-keyed, banded ground floor)
# -------------------------------------------------------------
def generate_ground():
    img = make_empty()
    c_shadow = color_of('tree_shadow')
    c_gfar = color_of('ground_far')
    c_dfill = color_of('dirt_fill')
    c_dmid = color_of('dirt_mid')

    for y in range(TARGET_H):
        for x in range(TARGET_W):
            if y < 150:
                img[y][x] = MAGENTA
            elif y < 158:
                # rows 150..157: understory entry dither
                t = (y - 150) / 7.0
                img[y][x] = dither_mix(x, y, c_shadow, c_gfar, t)
            elif y < 171:
                # rows 158..170: FLAT zone (scroll split 164)
                img[y][x] = c_gfar
            elif y < 196:
                # rows 171..195: transition dither
                t = (y - 171) / 24.0
                img[y][x] = dither_mix(x, y, c_gfar, c_dfill, t)
            elif y < 205:
                # rows 196..204: FLAT zone (scroll split 200)
                img[y][x] = c_dmid
            else:
                # rows 205..287: trail bed
                # Warming and brightening downward ramp
                if y < 230:
                    t = (y - 205) / 25.0
                    base_color = dither_mix(x, y, c_dmid, color_of('sand_shade'), t)
                elif y < 260:
                    t = (y - 230) / 30.0
                    base_color = dither_mix(x, y, color_of('sand_shade'), color_of('sand_mid'), t)
                else:
                    t = (y - 260) / 27.0
                    base_color = dither_mix(x, y, color_of('sand_mid'), color_of('sand_lit'), t)

                # Wandering verge contours
                verge_top = 210 + 4.0 * math.sin(2 * math.pi * x / 160.0) + 2.0 * math.cos(2 * math.pi * x / 90.0)
                verge_top_i = int(verge_top)

                verge_bottom = 280 + 5.0 * math.sin(2 * math.pi * x / 200.0) + 1.5 * math.cos(2 * math.pi * x / 80.0)
                verge_bottom_i = int(verge_bottom)

                if y < verge_top_i:
                    img[y][x] = c_shadow
                elif y == verge_top_i:
                    # Broken rim grass edge
                    if (x * 13) % 7 > 1:
                        img[y][x] = color_of('rim_grass')
                    else:
                        img[y][x] = c_shadow
                elif y < verge_top_i + 6:
                    img[y][x] = dither_mix(x, y, c_shadow, base_color, (y - verge_top_i) / 6.0)
                elif y > verge_bottom_i:
                    img[y][x] = c_shadow
                elif y > verge_bottom_i - 8:
                    img[y][x] = dither_mix(x, y, base_color, c_shadow, (y - (verge_bottom_i - 8)) / 8.0)
                else:
                    img[y][x] = base_color
    return img


# -------------------------------------------------------------
# 07_mountains (colour-keyed, parallax 0.12)
# -------------------------------------------------------------
def generate_mountains():
    img = make_empty()
    base_y = 156
    peaks = [
        (120, 95, 0.25, 0.20),
        (310, 105, 0.18, 0.22),
        (480, 90, 0.22, 0.18),
        (610, 115, 0.20, 0.25),
    ]

    ridge = [float(base_y)] * TARGET_W
    for px, py, ls, rs in peaks:
        for x in range(TARGET_W):
            dist = abs(x - px)
            slope = ls if x < px else rs
            curve = py + slope * dist + 0.0004 * (dist ** 2)
            if curve < ridge[x]:
                ridge[x] = curve

    for x in range(TARGET_W):
        noise = 1.5 * math.sin(0.12 * x) + 0.8 * math.cos(0.25 * x)
        ridge_y = int(round(ridge[x] + noise))
        ridge_y = max(0, min(base_y, ridge_y))

        for y in range(ridge_y, base_y):
            if y < ridge_y + 2:
                img[y][x] = color_of('mountain_rim')
            else:
                img[y][x] = color_of('mountain')
    return img


# -------------------------------------------------------------
# 06_hills_far (colour-keyed, parallax 0.20)
# -------------------------------------------------------------
def generate_hills_far():
    img = make_empty()
    base_y = 160
    c_mountain = color_of('mountain')
    c_tmid = color_of('tree_mid')
    c_mrim = color_of('mountain_rim')

    for x in range(TARGET_W):
        rolling = 135 + 10.0 * math.sin(2 * math.pi * x / 300.0) + 5.0 * math.cos(2 * math.pi * x / 130.0)
        bump = 3.0 * abs(math.sin(2 * math.pi * x / 16.0)) + 1.5 * math.cos(2 * math.pi * x / 7.0)
        ridge_y = int(round(rolling - bump))
        ridge_y = max(0, min(base_y, ridge_y))

        for y in range(ridge_y, base_y):
            if y == ridge_y:
                if (x * 3 + y * 7) % 4 != 0:
                    img[y][x] = c_mrim
                else:
                    img[y][x] = dither_mix(x, y, c_mountain, c_tmid, 0.5)
            else:
                img[y][x] = dither_mix(x, y, c_mountain, c_tmid, 0.5)
    return img


# -------------------------------------------------------------
# 05_hills_midfar (colour-keyed, parallax 0.30)
# -------------------------------------------------------------
def generate_hills_midfar():
    img = make_empty()
    base_y = 165  # Ends inside flat run 158..170
    c_tmid = color_of('tree_mid')
    c_tlit = color_of('tree_lit')

    for x in range(TARGET_W):
        rolling = 140 + 10.0 * math.sin(2 * math.pi * x / 250.0 + 1.5) + 4.0 * math.cos(2 * math.pi * x / 110.0)
        bump = 4.0 * abs(math.sin(2 * math.pi * x / 18.0)) + 2.0 * math.sin(2 * math.pi * x / 9.0)
        ridge_y = int(round(rolling - bump))
        ridge_y = max(0, min(base_y, ridge_y))

        for y in range(ridge_y, base_y):
            if y < ridge_y + 2:
                img[y][x] = c_tlit
            else:
                img[y][x] = c_tmid
    return img


# -------------------------------------------------------------
# 04_hills_mid (colour-keyed, parallax 0.42)
# -------------------------------------------------------------
def generate_hills_mid():
    img = make_empty()
    base_y = 178
    c_tmid = color_of('tree_mid')
    c_tshadow = color_of('tree_shadow')

    for x in range(TARGET_W):
        rolling = 152 + 12.0 * math.sin(2 * math.pi * x / 200.0 - 0.5) + 6.0 * math.cos(2 * math.pi * x / 95.0)
        bump = 6.0 * abs(math.sin(2 * math.pi * x / 22.0)) + 3.0 * math.cos(2 * math.pi * x / 11.0)
        ridge_y = int(round(rolling - bump))
        ridge_y = max(0, min(base_y, ridge_y))

        for y in range(ridge_y, base_y):
            if y == ridge_y:
                img[y][x] = c_tmid
            else:
                img[y][x] = dither_mix(x, y, c_tmid, c_tshadow, 0.5)
    return img


# -------------------------------------------------------------
# 03_hills_midnear (colour-keyed, parallax 0.55)
# -------------------------------------------------------------
def generate_hills_midnear():
    img = make_empty()
    base_y = 190
    c_tmid = color_of('tree_mid')
    c_tshadow = color_of('tree_shadow')
    c_trunk = color_of('trunk')

    # 1. Base rolling hill mass
    for x in range(TARGET_W):
        rolling = 150 + 20.0 * math.sin(2 * math.pi * x / 180.0 + 0.8) + 8.0 * math.cos(2 * math.pi * x / 80.0)
        bump = 5.0 * abs(math.sin(2 * math.pi * x / 16.0)) + 2.0 * math.cos(2 * math.pi * x / 8.0)
        ridge_y = int(round(rolling - bump))
        ridge_y = max(0, min(base_y, ridge_y))

        for y in range(ridge_y, base_y):
            img[y][x] = c_tshadow

    # 2. Add trunks & canopy lumps
    trunks = [
        # (tx, ty_top, rx, ry)
        (120, 55, 24, 16),
        (280, 65, 28, 18),
        (450, 50, 22, 15),
        (590, 70, 30, 20),
    ]

    # Paint trunks
    for tx, ty_top, rx, ry in trunks:
        for y in range(ty_top, base_y):
            offset = int(1.5 * math.sin(y / 15.0))
            for dx in [-1, 0, 1]:
                cx = tx + offset + dx
                if 0 <= cx < TARGET_W:
                    img[y][cx] = c_trunk

    # Paint canopy lumps over trunks
    for tx, ty_top, rx, ry in trunks:
        tx_center = tx + int(1.5 * math.sin(ty_top / 15.0))
        ty_center = ty_top
        for cy in range(max(0, ty_center - ry - 5), min(TARGET_H, ty_center + ry + 5)):
            for cx in range(max(0, tx_center - rx - 5), min(TARGET_W, tx_center + rx + 5)):
                dx = (cx - tx_center) / rx
                dy = (cy - ty_center) / ry
                dist = math.sqrt(dx*dx + dy*dy)
                angle = math.atan2(dy, dx)
                noise = 0.12 * math.sin(6.0 * angle) + 0.06 * math.cos(11.0 * angle)
                if dist + noise <= 1.0:
                    img[cy][cx] = c_tshadow

    # 3. Post-process to map a continuous crest edge
    for x in range(TARGET_W):
        for y in range(TARGET_H):
            if img[y][x] != MAGENTA and img[y][x] != c_trunk:
                is_top_edge = (y == 0 or img[y-1][x] == MAGENTA)
                if is_top_edge:
                    img[y][x] = c_tmid

    return img


# -------------------------------------------------------------
# 02_hills_near (colour-keyed, parallax 0.70)
# -------------------------------------------------------------
def generate_hills_near():
    img = make_empty()
    base_y = 200  # Ends inside flat run 196..204
    c_tshadow = color_of('tree_shadow')
    c_tmid = color_of('tree_mid')
    c_trunk = color_of('trunk')
    c_rim = color_of('rim_grass')

    # 1. Base rolling hill mass
    for x in range(TARGET_W):
        rolling = 160 + 25.0 * math.sin(2 * math.pi * x / 150.0 + 1.2) + 10.0 * math.cos(2 * math.pi * x / 70.0)
        bump = 6.0 * abs(math.sin(2 * math.pi * x / 14.0)) + 3.0 * math.cos(2 * math.pi * x / 7.0)
        ridge_y = int(round(rolling - bump))
        ridge_y = max(0, min(base_y, ridge_y))

        for y in range(ridge_y, base_y):
            img[y][x] = dither_mix(x, y, c_tshadow, c_trunk, 0.3)

        # 2. broken one-pixel rim_grass edge on the undergrowth only
        if ridge_y < base_y:
            if (x * 13) % 7 > 1:
                img[ridge_y][x] = c_rim

    # 3. Large foreground trunks & wide canopy lumps
    trunks = [
        # (tx, ty_top, rx, ry)
        (60, 30, 40, 24),
        (200, 45, 45, 26),
        (340, 25, 38, 22),
        (480, 50, 42, 25),
        (620, 35, 48, 28),
    ]

    # Paint trunks
    for tx, ty_top, rx, ry in trunks:
        for y in range(ty_top, base_y):
            offset = int(2.5 * math.sin(y / 20.0))
            for dx in [-2, -1, 0, 1]:
                cx = tx + offset + dx
                if 0 <= cx < TARGET_W:
                    img[y][cx] = c_trunk

    # Paint canopy masses over trunks
    for tx, ty_top, rx, ry in trunks:
        tx_center = tx + int(2.5 * math.sin(ty_top / 20.0))
        ty_center = ty_top
        for cy in range(max(0, ty_center - ry - 5), min(TARGET_H, ty_center + ry + 5)):
            for cx in range(max(0, tx_center - rx - 5), min(TARGET_W, tx_center + rx + 5)):
                dx = (cx - tx_center) / rx
                dy = (cy - ty_center) / ry
                dist = math.sqrt(dx*dx + dy*dy)
                angle = math.atan2(dy, dx)
                noise = 0.15 * math.sin(7.0 * angle) + 0.05 * math.cos(13.0 * angle)
                if dist + noise <= 1.0:
                    t = (cy - (ty_center - ry)) / (2.0 * ry)
                    t = min(1.0, max(0.0, 1.0 - t))
                    img[cy][cx] = dither_mix(cx, cy, c_tshadow, c_tmid, t)

    return img


# -------------------------------------------------------------
# 01_fg_rocks (colour-keyed, parallax 1.00)
# -------------------------------------------------------------
def generate_fg_rocks():
    img = make_empty()
    c_tshadow = color_of('tree_shadow')
    c_dfill = color_of('dirt_fill')
    c_trunk = color_of('trunk')
    c_wshade = color_of('wall_shade')
    c_wmid = color_of('wall_mid')
    c_wlit = color_of('wall_lit')

    # 1. Overhead hanging canopy
    for x in range(TARGET_W):
        fringe_y = 35 + 15.0 * math.sin(2 * math.pi * x / 140.0) + \
                   8.0 * math.cos(2 * math.pi * x / 65.0) + \
                   3.0 * math.sin(2 * math.pi * x / 15.0)
        fringe_y = min(58, max(0, fringe_y))
        for y in range(0, int(fringe_y)):
            t = min(1.0, max(0.0, y / 55.0))
            img[y][x] = dither_mix(x, y, c_dfill, c_tshadow, t)

    # 2. Full-height frame trunks (far left / far right)
    for y in range(TARGET_H):
        left_offset = int(2.0 * math.sin(y / 25.0))
        right_offset = int(1.5 * math.cos(y / 20.0))

        # Flare into base roots near the trail edge
        w_left = 2
        w_right = 2
        if y >= 240:
            w_left = 2 + int((y - 240) / 10)
            w_right = 2 + int((y - 240) / 12)

        # Left trunk
        for dx in range(-w_left, w_left + 1):
            cx = 25 + left_offset + dx
            if 0 <= cx < TARGET_W:
                img[y][cx] = c_trunk

        # Right trunk
        for dx in range(-w_right, w_right + 1):
            cx = 660 + right_offset + dx
            if 0 <= cx < TARGET_W:
                img[y][cx] = c_trunk

    # 3. Winding ground root details
    for rx in range(115, 145):
        ry = int(280 + 3.0 * math.sin((rx - 115) / 5.0))
        for dy in [-1, 0]:
            if 0 <= ry + dy < TARGET_H:
                img[ry + dy][rx] = c_trunk

    for rx in range(380, 410):
        ry = int(280 + 4.0 * math.cos((rx - 380) / 6.0))
        for dy in [-1, 0]:
            if 0 <= ry + dy < TARGET_H:
                img[ry + dy][rx] = c_trunk

    # 4. Rounded faceted trailside boulders
    boulders = [
        (100, 275, 15),
        (115, 280, 12),
        (250, 278, 14),
        (420, 276, 16),
        (405, 282, 10),
        (550, 277, 15),
    ]

    for bcx, bcy, r in boulders:
        for cy in range(max(0, bcy - r - 2), min(TARGET_H, bcy + r + 2)):
            for cx in range(max(0, bcx - r - 2), min(TARGET_W, bcx + r + 2)):
                dx = cx - bcx
                dy = cy - bcy
                dist = math.sqrt(dx*dx + dy*dy)
                if dist <= r:
                    is_rim = False
                    if dx <= 0 and dy <= 0:
                        for nx, ny in [(cx - 1, cy), (cx, cy - 1), (cx - 1, cy - 1)]:
                            ndist = math.sqrt((nx - bcx)**2 + (ny - bcy)**2)
                            if ndist > r:
                                is_rim = True
                                break
                    if is_rim:
                        img[cy][cx] = c_wlit
                    else:
                        if dx + dy < -2:
                            img[cy][cx] = c_wmid
                        else:
                            img[cy][cx] = c_wshade

    return img


LAYER_DEFS = [
    ("09_sky",          "bg_forest_09_sky.bmp",          generate_sky),
    ("08_ground",       "bg_forest_08_ground.bmp",       generate_ground),
    ("07_mountains",    "bg_forest_07_mountains.bmp",    generate_mountains),
    ("06_hills_far",    "bg_forest_06_hills_far.bmp",    generate_hills_far),
    ("05_hills_midfar", "bg_forest_05_hills_midfar.bmp", generate_hills_midfar),
    ("04_hills_mid",    "bg_forest_04_hills_mid.bmp",    generate_hills_mid),
    ("03_hills_midnear","bg_forest_03_hills_midnear.bmp",generate_hills_midnear),
    ("02_hills_near",   "bg_forest_02_hills_near.bmp",   generate_hills_near),
    ("01_fg_rocks",     "bg_forest_01_fg_rocks.bmp",     generate_fg_rocks),
]


def write_scene_maps(composite_rgb):
    """Generates and writes out the material and albedo map pairs."""
    assert_legend_matches_header()
    mat_path = os.path.join(DST_DIR, "bg_forest_material.bmp")
    alb_path = os.path.join(DST_DIR, "bg_forest_albedo.bmp")
    root_mat = os.path.join(REPO_ROOT, "assets", "bg_forest_material.bmp")
    root_alb = os.path.join(REPO_ROOT, "assets", "bg_forest_albedo.bmp")

    mat_pixels = []
    alb_pixels = []
    solid = 0
    for y in range(TARGET_H):
        for x in range(TARGET_W):
            if y >= FLOOR_ROW:
                mat_pixels.append(LEGEND_WALL)
                solid += 1
            else:
                mat_pixels.append(LEGEND_EMPTY)
            alb_pixels.append(composite_rgb[y * TARGET_W + x])

    write_bmp(mat_path, TARGET_W, TARGET_H, mat_pixels)
    write_bmp(alb_path, TARGET_W, TARGET_H, alb_pixels)
    write_bmp(root_mat, TARGET_W, TARGET_H, mat_pixels)
    write_bmp(root_alb, TARGET_W, TARGET_H, alb_pixels)

    if os.path.exists(BUILD_DST_DIR):
        build_mat = os.path.join(BUILD_DST_DIR, "bg_forest_material.bmp")
        build_alb = os.path.join(BUILD_DST_DIR, "bg_forest_albedo.bmp")
        write_bmp(build_mat, TARGET_W, TARGET_H, mat_pixels)
        write_bmp(build_alb, TARGET_W, TARGET_H, alb_pixels)

    print(f"Scene maps written: floor at row {FLOOR_ROW}, {solid} solid cells.")


def write_readme():
    """Generates the descriptive asset documentation file."""
    readme_path = os.path.join(DST_DIR, "README.md")
    content = """# Forest trail (bg_forest) multi-layer set

Native resolution: 688 x 288 px. One art pixel is one world cell, so this is a
world 688 cells wide and 288 tall, loaded at scale 10 by the `bg_forest` row in
`assets/scenes.txt`.

**Generated, not drawn.** `python tools/generate_bg_forest.py` writes every file
here; edits made to the BMPs by hand are overwritten on the next run. It is
deterministic - two runs produce byte-identical output - so a diff in this
directory is a real change to the generator and never a re-roll.

**This is the first authored set that is not `bg1`'s landscape.** `bg1_ext` and
`bg_gemini` are the same place as `bg1` at 688x288; this one is a different
place at the same camera. The parallax ladder and the ground band table are
`bg_gemini`'s unchanged, because a factor is one over a depth and the depths did
not move - only what stands at them did.

## Parallax Layer Table (Back-to-Front)

| Filename | Parallax Factor | Transparency | Description |
| :--- | :--- | :--- | :--- |
| bg_forest_09_sky.bmp | 0.04 | Opaque | Sky gradient with scattered stars |
| bg_forest_08_ground.bmp | Banded | Color-keyed (Magenta) | Forest floor with trail bed |
| bg_forest_07_mountains.bmp | 0.12 | Color-keyed (Magenta) | Distant mountain ridge |
| bg_forest_06_hills_far.bmp | 0.20 | Color-keyed (Magenta) | Far hills with rounded treeline |
| bg_forest_05_hills_midfar.bmp | 0.30 | Color-keyed (Magenta) | Mid-far hills with lit canopy edges |
| bg_forest_04_hills_mid.bmp | 0.42 | Color-keyed (Magenta) | Mid-ground hills and trees |
| bg_forest_03_hills_midnear.bmp | 0.55 | Color-keyed (Magenta) | Mid-near forest mass with trunks |
| bg_forest_02_hills_near.bmp | 0.70 | Color-keyed (Magenta) | Near forest canopy and detailed trunks |
| bg_forest_01_fg_rocks.bmp | 1.00 | Color-keyed (Magenta) | Foreground rocks, roots, and overhead canopy |

### Ground Banding Factors:
- Rows 0..164: Factor 0.30
- Rows 164..200: Factor 0.70
- Rows 200..288: Factor 1.00

## Colour palette & visual rules

- **Palette**: every colour is named through `pixel_art.color_of()`. Nothing
  here is a hardcoded RGB tuple, which is what lets
  `python tools/validate_palette.py assets/bg_forest/<layer>.bmp --colorkey`
  come back clean. Gradients go through `dither_mix`, which only ever picks
  between two *named* colours and so cannot leave the set.
- **Flat uniform runs, and they are load-bearing.** A ground band boundary is a
  discontinuity in horizontal scroll offset, so it is invisible only where the
  rows either side of it are uniform across every column *and* the same colour.
  - Rows 158..170: flat `ground_far` - carries the cut at row 164.
  - Rows 196..204: flat `dirt_mid` - carries the cut at row 200.
  `boot_test` reads this directory's `08_ground` BMP and asserts exactly that.
- **The two layers that stand on the plane have their feet inside those runs**:
  `05_hills_midfar` (0.30) at art row 165, `02_hills_near` (0.70) at art row
  200. That is the generator's half of the contract with the band table, and
  nothing checks it - if a foot moves, move it to another flat run.
- **The sky reaches `sky_horizon` at row 150, the ground plane's own top edge.**
  It was authored to bottom out at row 200 and layer 07 disappeared: `mountain`
  is luminance 62 and the ramp passes through 55..63 across rows 90..156, which
  is exactly where the ridge sits. Anchoring the ramp to the scene's horizon is
  what makes the mountain body read against the sky behind it.
- **Depth grade**, body luminance, far to near: 62, 56, 50, 40, 29, 28, 25.
  Nearer is darker and more contrasty; further is closer to the sky.
- **Floor row**: the terrain floor is at art row 264, 16512 solid cells. The
  scene finds it with `spawn terrain`'s own scan, not from a constant.
- **Albedo map**: the finished composite, exactly as `composite_preview.png`
  shows it.
"""
    with open(readme_path, "w", encoding="utf-8") as f:
        f.write(content)


def main():
    os.makedirs(DST_DIR, exist_ok=True)

    print("Generating standard 688x288 backdrop layers for bg_forest...")
    composite_rgb = [MAGENTA] * (TARGET_H * TARGET_W)

    for tag, fname, gen_fn in LAYER_DEFS:
        layer_arr = gen_fn()

        dst_path = os.path.join(DST_DIR, fname)
        pixels = [layer_arr[y][x] for y in range(TARGET_H) for x in range(TARGET_W)]
        write_bmp(dst_path, TARGET_W, TARGET_H, pixels)

        if os.path.exists(BUILD_DST_DIR):
            shutil.copyfile(dst_path, os.path.join(BUILD_DST_DIR, fname))

        # Composite back-to-front
        for y in range(TARGET_H):
            for x in range(TARGET_W):
                color = layer_arr[y][x]
                if color != MAGENTA:
                    composite_rgb[y * TARGET_W + x] = color

        print(f"Generated {fname}")

    preview_path = os.path.join(DST_DIR, "composite_preview.png")
    write_png(preview_path, TARGET_W, TARGET_H, composite_rgb)
    print("Composite preview saved.")

    if os.path.exists(BUILD_DST_DIR):
        shutil.copyfile(preview_path, os.path.join(BUILD_DST_DIR, "composite_preview.png"))

    write_scene_maps(composite_rgb)
    write_readme()
    print("Done! All assets in assets/bg_forest/ successfully generated.")


if __name__ == "__main__":
    main()
