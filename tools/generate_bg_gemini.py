"""Generates the extended 688x288 backdrop layer set for bg_gemini/.

This is a newly generated, authored pixel art landscape extending the visual style
and topology of assets/bg1/ to a 688x288 canvas:
- Native resolution: 688 x 288 px
- 9 parallax layers matching bg1's depth breakdown, factor ladder, and back-to-front drawing order
- Exact palette colors measured from assets/bg1/
- Authentic matched topology:
  * Layer 09 (Sky): 3-tier atmospheric gradient with undulating cloud and shadow strata
  * Layer 08 (Ground): 4 receding surface bands (slate shore, teal lake, olive grass, forest meadow)
    with strictly uniform rows at horizontal scroll split zones (156..172, 196..204, 214..287)
  * Layer 07 (Mountains): Continuous majestic mountain massif on left with serrated aretes and craggy peaks
  * Layer 06 (Far Hills): Sharp angular pyramid peaks with diagonal descending facet contours
  * Layer 05 (Mid-Far Hills): Cool slate-green shark-fin twin summits with sharp aretes
  * Layer 04 (Mid Hills): Warm ochre rolling hummocks across the horizon with crest highlights
  * Layer 03 (Mid-Near Hills): Earthy rolling knolls framing the right midground
  * Layer 02 (Near Hills): Low stepped rocky terraces on the left and right, leaving an open panoramic lake vista
  * Layer 01 (FG Rocks): Faceted rounded boulders resting on the player floor
- Material and albedo maps with terrain floor at row 264

**Standard library only.** Follows the project policy of no third-party dependencies.

Nothing here is faster for it. The numpy slice assignments became explicit
nested loops over 688x288, and that is the intended trade - this is an offline
generator run by hand, and a one-to-one correspondence with the art it produces
is worth more than the seconds.
"""
import os
import shutil
import sys
import math

# Derived from this file's location rather than hardcoded: an absolute path
# into one machine's home directory runs only there, and nothing says so.
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, 'tools'))
from pixel_art import (LEGEND_EMPTY, LEGEND_WALL, assert_legend_matches_header,
                       write_bmp, write_png)

DST_DIR = os.path.join(REPO_ROOT, "assets", "bg_gemini")
BUILD_DST_DIR = os.path.join(REPO_ROOT, "build", "Release", "assets", "bg_gemini")

TARGET_W = 688
TARGET_H = 288

# Floor row for material map: scaled 2x from bg1's row 132
FLOOR_ROW = 264

# LEGEND_EMPTY / LEGEND_WALL come from tools/pixel_art.py, which checks them
# against src/scene/legend.h.
COLOR_KEY = (0xFF, 0x00, 0xFF)

MAGENTA = (255, 0, 255)

# Exact palette colors measured from assets/bg1/
SKY_BASE = (161, 201, 199)
SKY_CLOUD = (125, 161, 149)
SKY_RIM = (124, 178, 187)

GND_SHORE = (69, 72, 81)
GND_WATER = (19, 52, 49)
GND_OLIVE = (31, 42, 12)
GND_FOREST = (12, 39, 20)

MTN_BODY = (11, 77, 92)
MTN_RIM = (0, 69, 84)

H_FAR_BODY = (133, 122, 103)
H_FAR_RIM = (133, 112, 76)

H_MIDFAR_BODY = (107, 127, 120)
H_MIDFAR_RIM = (57, 127, 103)

H_MID_BODY = (164, 152, 128)
H_MID_RIM = (164, 136, 81)

H_MIDNEAR_BODY = (128, 108, 71)
H_MIDNEAR_RIM = (128, 97, 40)

H_NEAR_BODY = (117, 79, 68)
H_NEAR_RIM = (117, 63, 48)

ROCKS_BODY = (69, 72, 81)
ROCKS_RIM = (59, 65, 81)


class NumPyRandom:
    """Reproduces numpy's legacy MT19937 stream exactly, bit for bit.

    **This exists to preserve generated art, not because a PRNG was wanted.**
    The mountain layer's noise was drawn from `np.random`, so any substitute
    that produces a different sequence produces different mountains - and the
    BMPs it feeds are committed. Python's own `random` is also MT19937 but
    seeds and draws differently, so `random.seed(101)` does not reproduce it.
    What does: numpy's scalar seeding is `init_genrand` (the 1812433253
    recurrence below), and its doubles are built from two 32-bit words as
    `(a >> 5) * 2**26 + (b >> 6)` over 2**53. Both are reproduced here, and
    `generate_mountains` comes out byte-identical against the numpy original.

    **Freezing the draws as constants was tried and does not work.** The four
    phase draws could be inlined, but the arete loop below calls this a
    data-dependent number of times - it draws once per row walked, and how many
    rows that is depends on the ridge the earlier draws just produced. There is
    no fixed list to freeze.

    Only seed 101 survives. `np.random.seed(202)`, `(303)`, `(404)` and `(505)`
    were also in the numpy version and every one of them was dead: each was
    followed by pure `sin`/`cos` noise that never drew a value. They are gone
    rather than carried over, which is why four of the five seeds a reader may
    remember are not here.
    """
    def __init__(self, seed):
        self.state = [0] * 624
        self.state[0] = seed & 0xffffffff
        for i in range(1, 624):
            self.state[i] = (1812433253 * (self.state[i-1] ^ (self.state[i-1] >> 30)) + i) & 0xffffffff
        self.index = 624

    def _twist(self):
        for i in range(624):
            y = (self.state[i] & 0x80000000) | (self.state[(i + 1) % 624] & 0x7fffffff)
            self.state[i] = self.state[(i + 397) % 624] ^ (y >> 1)
            if y & 1:
                self.state[i] ^= 0x9908b0df
        self.index = 0

    def next_u32(self):
        if self.index >= 624:
            self._twist()
        y = self.state[self.index]
        y ^= (y >> 11)
        y ^= (y << 7) & 0x9d2c5680
        y ^= (y << 15) & 0xefc60000
        y ^= (y >> 18)
        self.index += 1
        return y

    def random(self):
        a = self.next_u32() >> 5
        b = self.next_u32() >> 6
        return (a * 67108864.0 + b) / 9007199254740992.0


def make_empty():
    return [[MAGENTA for _ in range(TARGET_W)] for _ in range(TARGET_H)]


# -------------------------------------------------------------
# 09_sky
# -------------------------------------------------------------
def generate_sky():
    img = [[SKY_BASE for _ in range(TARGET_W)] for _ in range(TARGET_H)]
    for x in range(TARGET_W):
        y_cloud = int(24 + 5.0 * math.sin(2 * math.pi * x / 340.0) + 
                          3.0 * math.cos(2 * math.pi * x / 180.0 + 1.2) + 
                          1.2 * math.sin(2 * math.pi * x / 75.0))
        for y in range(0, y_cloud):
            img[y][x] = SKY_CLOUD
        for y in range(y_cloud, y_cloud + 3):
            img[y][x] = SKY_RIM
    return img


# -------------------------------------------------------------
# 08_ground
# -------------------------------------------------------------
def generate_ground():
    img = make_empty()
    y_shore = [0] * TARGET_W
    y_water = [0] * TARGET_W
    y_olive = [0] * TARGET_W
    y_forest = [0] * TARGET_W
    
    for x in range(TARGET_W):
        y_shore[x] = int(128 + 3.0 * math.sin(2 * math.pi * x / 240.0) + 1.5 * math.cos(2 * math.pi * x / 105.0))
        y_water[x] = int(142 + 2.5 * math.sin(2 * math.pi * x / 280.0 + 0.8) + 1.2 * math.cos(2 * math.pi * x / 120.0))
        y_olive[x] = int(188 + 3.0 * math.sin(2 * math.pi * x / 310.0 + 1.5) + 1.5 * math.cos(2 * math.pi * x / 135.0))
        y_forest[x] = int(210 + 2.0 * math.sin(2 * math.pi * x / 290.0 + 2.2) + 1.0 * math.cos(2 * math.pi * x / 145.0))
        
        for y in range(y_shore[x], y_water[x]):
            img[y][x] = GND_SHORE
        for y in range(y_water[x], y_olive[x]):
            img[y][x] = GND_WATER
        for y in range(y_olive[x], y_forest[x]):
            img[y][x] = GND_OLIVE
        for y in range(y_forest[x], TARGET_H):
            img[y][x] = GND_FOREST

    # Strict uniform rows for parallax cut safety:
    for y in range(156, 173):
        for x in range(TARGET_W):
            img[y][x] = GND_WATER   # Band 0 / 1 split at row 164
    for y in range(196, 205):
        for x in range(TARGET_W):
            img[y][x] = GND_OLIVE   # Band 1 / 2 split at row 200
    for y in range(214, TARGET_H):
        for x in range(TARGET_W):
            img[y][x] = GND_FOREST
    return img


# -------------------------------------------------------------
# 07_mountains
# -------------------------------------------------------------
def generate_mountains():
    img = make_empty()
    base_y = 126
    
    peaks = [
        (30, 8, 0.40, 0.35),
        (75, 2, 0.35, 0.42),
        (130, 22, 0.42, 0.32),
        (185, 28, 0.32, 0.38),
        (240, 40, 0.36, 0.30),
        (295, 48, 0.28, 0.34),
        (370, 34, 0.44, 0.40),
        (425, 48, 0.36, 0.42),
        (490, 42, 0.40, 0.36),
        (545, 36, 0.42, 0.44),
        (610, 46, 0.38, 0.36),
        (660, 40, 0.36, 0.38),
    ]
    
    ridge = [float(base_y)] * TARGET_W
    for px, py, ls, rs in peaks:
        for x in range(TARGET_W):
            curve = py + (ls * (px - x) if x < px else rs * (x - px))
            if curve < ridge[x]:
                ridge[x] = curve

    rng = NumPyRandom(101)
    noise = [0.0] * TARGET_W
    for f, a in [(0.08, 3.2), (0.18, 1.8), (0.35, 1.0), (0.7, 0.6)]:
        phase = rng.random() * 2.0 * math.pi
        for x in range(TARGET_W):
            noise[x] += a * math.sin(f * x + phase)
    
    ridge_int = [0] * TARGET_W
    for x in range(TARGET_W):
        val = int(round(ridge[x] + noise[x]))
        if val < 0:
            val = 0
        elif val > base_y:
            val = base_y
        ridge_int[x] = val
    
    for x in range(TARGET_W):
        top = ridge_int[x]
        if top < base_y:
            for y in range(top, base_y):
                img[y][x] = MTN_BODY
            for y in range(top, min(top + 2, base_y)):
                img[y][x] = MTN_RIM
            
    arete_origins = [(75, 2), (185, 28), (370, 34), (490, 42), (545, 36), (660, 40)]
    for ox, oy in arete_origins:
        cx, cy = ox, ridge_int[ox]
        while cy < base_y - 4 and 0 <= cx < TARGET_W - 1:
            img[cy][cx] = MTN_RIM
            img[cy+1][cx] = MTN_RIM
            cx += 1 if rng.random() > 0.35 else 0
            cy += 1
            
    return img


# -------------------------------------------------------------
# 06_hills_far
# -------------------------------------------------------------
def generate_hills_far():
    img = make_empty()
    base_y = 134
    
    peaks = [
        (75, 74, 0.35, 0.32),
        (280, 42, 0.38, 0.44),
        (495, 56, 0.40, 0.34),
    ]
    
    ridge = [float(base_y)] * TARGET_W
    for px, py, ls, rs in peaks:
        for x in range(TARGET_W):
            dist = abs(x - px)
            slope = ls if x < px else rs
            curve = py + slope * dist + 0.0004 * (dist ** 2)
            if curve < ridge[x]:
                ridge[x] = curve
                
    noise = [1.0 * math.sin(0.14 * x) + 0.6 * math.cos(0.32 * x) for x in range(TARGET_W)]
    ridge_int = [0] * TARGET_W
    for x in range(TARGET_W):
        val = int(round(ridge[x] + noise[x]))
        if val < 0:
            val = 0
        elif val > base_y:
            val = base_y
        ridge_int[x] = val
    
    for x in range(TARGET_W):
        top = ridge_int[x]
        if top < base_y:
            for y in range(top, base_y):
                img[y][x] = H_FAR_BODY
            for y in range(top, min(top + 2, base_y)):
                img[y][x] = H_FAR_RIM
            
    for px, py in [(75, 74), (280, 42), (495, 56)]:
        cx, cy = px, ridge_int[px]
        while cy < base_y - 4 and cx < TARGET_W - 2:
            img[cy][cx] = H_FAR_RIM
            img[cy][cx+1] = H_FAR_RIM
            cy += 1
            cx += 1 if (cy % 2 == 0) else 2
            
    return img


# -------------------------------------------------------------
# 05_hills_midfar
# -------------------------------------------------------------
def generate_hills_midfar():
    img = make_empty()
    base_y = 146
    
    peaks = [
        (440, 66, 0.42, 0.52),
        (472, 68, 0.54, 0.38),
        (565, 84, 0.36, 0.34),
    ]
    
    ridge = [float(base_y)] * TARGET_W
    for px, py, ls, rs in peaks:
        for x in range(320, 650):
            dist = abs(x - px)
            slope = ls if x < px else rs
            curve = py + slope * dist + 0.0006 * (dist ** 2)
            if curve < ridge[x]:
                ridge[x] = curve
                
    noise = [0.8 * math.sin(0.16 * x) for x in range(TARGET_W)]
    ridge_int = [0] * TARGET_W
    for x in range(TARGET_W):
        val = int(round(ridge[x] + noise[x]))
        if val < 0:
            val = 0
        elif val > base_y:
            val = base_y
        ridge_int[x] = val
    
    for x in range(TARGET_W):
        top = ridge_int[x]
        if top < base_y:
            for y in range(top, base_y):
                img[y][x] = H_MIDFAR_BODY
            for y in range(top, min(top + 2, base_y)):
                img[y][x] = H_MIDFAR_RIM
            
    cx, cy = 440, ridge_int[440]
    while cy < base_y - 4 and cx < 472:
        img[cy][cx] = H_MIDFAR_RIM
        cy += 1
        cx += 1
        
    return img


# -------------------------------------------------------------
# 04_hills_mid
# -------------------------------------------------------------
def generate_hills_mid():
    img = make_empty()
    base_y = 156
    
    peaks = [
        (35, 90, 0.24, 0.28),
        (290, 116, 0.26, 0.30),
        (510, 108, 0.28, 0.26),
    ]
    
    ridge = [float(base_y)] * TARGET_W
    for px, py, ls, rs in peaks:
        for x in range(TARGET_W):
            dist = abs(x - px)
            slope = ls if x < px else rs
            curve = py + slope * dist + 0.0005 * (dist ** 2)
            if curve < ridge[x]:
                ridge[x] = curve
                
    noise = [0.8 * math.sin(0.18 * x) for x in range(TARGET_W)]
    ridge_int = [0] * TARGET_W
    for x in range(TARGET_W):
        val = int(round(ridge[x] + noise[x]))
        if val < 0:
            val = 0
        elif val > base_y:
            val = base_y
        ridge_int[x] = val
    
    for x in range(TARGET_W):
        top = ridge_int[x]
        if top < base_y:
            for y in range(top, base_y):
                img[y][x] = H_MID_BODY
            for y in range(top, min(top + 2, base_y)):
                img[y][x] = H_MID_RIM
            
    for sx, sy in [(35, 94), (290, 120), (510, 112)]:
        cx, cy = sx, sy
        while cy < base_y - 4 and cx < TARGET_W - 2:
            img[cy][cx] = H_MID_RIM
            cy += 1
            cx += 2 if (cy % 2 == 0) else 1

    return img


# -------------------------------------------------------------
# 03_hills_midnear
# -------------------------------------------------------------
def generate_hills_midnear():
    img = make_empty()
    base_y = 164
    
    peaks = [
        (480, 128, 0.26, 0.24),
        (615, 106, 0.24, 0.20),
    ]
    
    ridge = [float(base_y)] * TARGET_W
    for px, py, ls, rs in peaks:
        for x in range(370, TARGET_W):
            dist = abs(x - px)
            slope = ls if x < px else rs
            curve = py + slope * dist + 0.0005 * (dist ** 2)
            if curve < ridge[x]:
                ridge[x] = curve
                
    noise = [0.8 * math.sin(0.20 * x) for x in range(TARGET_W)]
    ridge_int = [0] * TARGET_W
    for x in range(370, TARGET_W):
        val = int(round(ridge[x] + noise[x]))
        if val < 0:
            val = 0
        elif val > base_y:
            val = base_y
        ridge_int[x] = val
    
    for x in range(370, TARGET_W):
        top = ridge_int[x]
        if top < base_y:
            for y in range(top, base_y):
                img[y][x] = H_MIDNEAR_BODY
            for y in range(top, min(top + 2, base_y)):
                img[y][x] = H_MIDNEAR_RIM
            
    for sx, sy in [(480, 132), (615, 110)]:
        cx, cy = sx, sy
        while cy < base_y - 4 and cx < TARGET_W - 1:
            img[cy][cx] = H_MIDNEAR_RIM
            cy += 1
            cx += 2
            
    return img


# -------------------------------------------------------------
# 02_hills_near
# -------------------------------------------------------------
def generate_hills_near():
    img = make_empty()
    base_y = 178
    
    # Left terraces: continuous natural stepped profile
    for x in range(0, 210):
        if x < 45:
            local_top = int(132 + 0.10 * x)
        elif x < 75:
            t = (x - 45) / 30.0
            local_top = int(136.5 + 11.5 * (3*t*t - 2*t*t*t))
        elif x < 125:
            local_top = int(148 + 0.08 * (x - 75))
        elif x < 155:
            t = (x - 125) / 30.0
            local_top = int(152.0 + 10.0 * (3*t*t - 2*t*t*t))
        elif x < 195:
            local_top = int(162 + 0.12 * (x - 155))
        else:
            t = (x - 195) / 15.0
            local_top = int(167 + 10.0 * t)
            
        if local_top < base_y:
            for y in range(local_top, base_y):
                img[y][x] = H_NEAR_BODY
            for y in range(local_top, min(local_top + 2, base_y)):
                img[y][x] = H_NEAR_RIM

    # Right terraces: continuous natural stepped profile
    for x in range(545, TARGET_W):
        if x < 575:
            t = (x - 545) / 30.0
            local_top = int(176 - 10.0 * (3*t*t - 2*t*t*t))
        elif x < 615:
            local_top = int(166 - 0.10 * (x - 575))
        elif x < 645:
            t = (x - 615) / 30.0
            local_top = int(162 - 8.0 * (3*t*t - 2*t*t*t))
        else:
            local_top = int(154 - 0.12 * (x - 645))
            
        if local_top < base_y:
            for y in range(local_top, base_y):
                img[y][x] = H_NEAR_BODY
            for y in range(local_top, min(local_top + 2, base_y)):
                img[y][x] = H_NEAR_RIM
                
    for y_step in [146, 158, 168]:
        for x in range(0, 185):
            if img[y_step][x] != MAGENTA and y_step + 1 < base_y:
                img[y_step][x] = H_NEAR_RIM
    for y_step in [160, 170]:
        for x in range(570, TARGET_W):
            if img[y_step][x] != MAGENTA and y_step + 1 < base_y:
                img[y_step][x] = H_NEAR_RIM

    return img


# -------------------------------------------------------------
# 01_fg_rocks
# -------------------------------------------------------------
def generate_fg_rocks():
    img = make_empty()
    base_y = 288
    
    boulders = [
        (110, 276, 42, 18),
        (165, 270, 58, 22),
        (225, 266, 65, 24),
        (275, 272, 40, 18),
        (505, 268, 55, 22),
        (560, 272, 45, 18),
    ]
    
    for cx, cy, rx, ry in boulders:
        for x in range(max(0, cx - rx), min(TARGET_W, cx + rx)):
            dx = (x - cx) / rx
            term = 1.0 - (dx * dx)
            if term > 0:
                h_curve = ry * (term ** 0.65)
                top_y = int(cy - h_curve)
                if top_y < base_y:
                    for y in range(top_y, base_y):
                        img[y][x] = ROCKS_BODY
                    for y in range(top_y, min(top_y + 2, base_y)):
                        img[y][x] = ROCKS_RIM
                    
    for cx, cy, rx, ry in [(165, 270, 58, 22), (225, 266, 65, 24), (505, 268, 55, 22)]:
        facet_y = cy - int(ry * 0.3)
        for x in range(cx - int(rx * 0.5), cx + int(rx * 0.5)):
            if img[facet_y][x] != MAGENTA and facet_y + 1 < base_y:
                img[facet_y][x] = ROCKS_RIM

    return img


# Draw order back to front (exact engine compositing order):
LAYER_DEFS = [
    ("09_sky",          "bg_gemini_09_sky.bmp",          "bg1_09_sky.bmp",          generate_sky),
    ("08_ground",       "bg_gemini_08_ground.bmp",       "bg1_08_ground.bmp",       generate_ground),
    ("07_mountains",    "bg_gemini_07_mountains.bmp",    "bg1_07_mountains.bmp",    generate_mountains),
    ("06_hills_far",    "bg_gemini_06_hills_far.bmp",    "bg1_06_hills_far.bmp",    generate_hills_far),
    ("05_hills_midfar", "bg_gemini_05_hills_midfar.bmp", "bg1_05_hills_midfar.bmp", generate_hills_midfar),
    ("04_hills_mid",    "bg_gemini_04_hills_mid.bmp",    "bg1_04_hills_mid.bmp",    generate_hills_mid),
    ("03_hills_midnear","bg_gemini_03_hills_midnear.bmp","bg1_03_hills_midnear.bmp",generate_hills_midnear),
    ("02_hills_near",   "bg_gemini_02_hills_near.bmp",   "bg1_02_hills_near.bmp",   generate_hills_near),
    ("01_fg_rocks",     "bg_gemini_01_fg_rocks.bmp",     "bg1_01_fg_rocks.bmp",     generate_fg_rocks),
]


def write_scene_maps(composite_rgb):
    assert_legend_matches_header()
    mat_path = os.path.join(DST_DIR, "bg_gemini_material.bmp")
    alb_path = os.path.join(DST_DIR, "bg_gemini_albedo.bmp")
    root_mat = os.path.join(REPO_ROOT, "assets", "bg_gemini_material.bmp")
    root_alb = os.path.join(REPO_ROOT, "assets", "bg_gemini_albedo.bmp")
    
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
        build_mat = os.path.join(BUILD_DST_DIR, "bg_gemini_material.bmp")
        build_alb = os.path.join(BUILD_DST_DIR, "bg_gemini_albedo.bmp")
        write_bmp(build_mat, TARGET_W, TARGET_H, mat_pixels)
        write_bmp(build_alb, TARGET_W, TARGET_H, alb_pixels)

    print(f"Scene maps written: floor at row {FLOOR_ROW}, {solid} solid cells.")


def write_readme():
    content = """# Background Gemini (bg_gemini) Multi-Layer Extended Set

Native resolution: 688 x 288 px (New generative pixel art extension of Background_1 / bg1)

## Layer Order & Depth Breakdown (Back to Front)

| File | Layer Index | Name / Description | Parallax X | Parallax Y | Transparency |
| :--- | :---: | :--- | :---: | :---: | :--- |
| bg_gemini_09_sky.bmp | 9 | Sky & Atmosphere | 0.04x | 1.00x | Fully Opaque |
| bg_gemini_08_ground.bmp | 8 | Ground / Water Plane | Banded | 1.00x | Magenta Key |
| bg_gemini_07_mountains.bmp | 7 | Distant Mountains | 0.12x | 1.00x | Magenta Key |
| bg_gemini_06_hills_far.bmp | 6 | Far Hills | 0.20x | 1.00x | Magenta Key |
| bg_gemini_05_hills_midfar.bmp | 5 | Mid-Far Hills | 0.30x | 1.00x | Magenta Key |
| bg_gemini_04_hills_mid.bmp | 4 | Mid Hills | 0.42x | 1.00x | Magenta Key |
| bg_gemini_03_hills_midnear.bmp | 3 | Mid-Near Hills | 0.55x | 1.00x | Magenta Key |
| bg_gemini_02_hills_near.bmp | 2 | Near Hills | 0.70x | 1.00x | Magenta Key |
| bg_gemini_01_fg_rocks.bmp | 1 | Foreground Rocks (in front of player) | 1.00x | 1.00x | Magenta Key (Foreground) |

## Color Palette & Visual Rules

- **Palette**: Exact RGB colors measured from `assets/bg1/`.
- **Composition**: Authoring of extended geological features (towering mountain massif on left with serrated arêtes, crisp pyramid summits on far hills, shark-fin twin summits on midfar hills, stepped rock terraces framing open panoramic lake).
- **Ground Banding**:
  * Band 0 (rows 0..164): 0.30x parallax factor.
  * Band 1 (rows 164..200): 0.70x parallax factor.
  * Band 2 (rows 200..288): 1.00x parallax factor.
  * Rows 156..172: 100% uniform water color `(19, 52, 49)`.
  * Rows 196..204: 100% uniform mid-ground color `(31, 42, 12)`.
  * Rows 214..287: 100% uniform near-ground color `(12, 39, 20)`.
- **Material Map**: Terrain floor at row 264 with Wall material `0x888888`, air above as Empty `0x000000`.
- **Albedo Map**: Fully composed scene matching game presentation.
"""
    with open(os.path.join(DST_DIR, "README.md"), "w") as f:
        f.write(content)


def main():
    os.makedirs(DST_DIR, exist_ok=True)
    os.makedirs(BUILD_DST_DIR, exist_ok=True)
    
    print("Generating newly generated 688x288 backdrop layers for bg_gemini...")
    composite_rgb = [MAGENTA] * (TARGET_H * TARGET_W)
    
    for tag, fname, alias_fname, gen_fn in LAYER_DEFS:
        layer_arr = gen_fn()
        
        # Save primary BMP
        dst_path = os.path.join(DST_DIR, fname)
        pixels = [layer_arr[y][x] for y in range(TARGET_H) for x in range(TARGET_W)]
        write_bmp(dst_path, TARGET_W, TARGET_H, pixels)
        
        # Save alias BMP (e.g. bg1_*.bmp in assets/bg_gemini/)
        alias_path = os.path.join(DST_DIR, alias_fname)
        shutil.copyfile(dst_path, alias_path)
        
        # Copy to build dir if present
        if os.path.exists(BUILD_DST_DIR):
            shutil.copyfile(dst_path, os.path.join(BUILD_DST_DIR, fname))
            shutil.copyfile(dst_path, os.path.join(BUILD_DST_DIR, alias_fname))
            
        # Composite back-to-front
        for y in range(TARGET_H):
            for x in range(TARGET_W):
                color = layer_arr[y][x]
                if color != MAGENTA:
                    composite_rgb[y * TARGET_W + x] = color
            
        print(f"Generated {fname} ({alias_fname})")
        
    preview_path = os.path.join(DST_DIR, "composite_preview.png")
    write_png(preview_path, TARGET_W, TARGET_H, composite_rgb)
    if os.path.exists(BUILD_DST_DIR):
        write_png(os.path.join(BUILD_DST_DIR, "composite_preview.png"), TARGET_W, TARGET_H, composite_rgb)
    print("Composite preview saved.")
    
    write_scene_maps(composite_rgb)
    write_readme()
    print("Done! All assets in assets/bg_gemini/ successfully generated.")


if __name__ == "__main__":
    main()
