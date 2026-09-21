"""The pixel-art framework: one shared palette, one BMP codec, one PNG codec, one
dithering rule, one rim-light helper, one depth ramp. Every generator in tools/
imports this rather than rolling its own, which is what makes a palette
validator possible at all.

Status: this is a working set, not a locked one. Two halves have aged
differently.

  - The backdrop, tree and terrain groups are in force. Every pixel of that art
    is generated from these names by the tools below, so it conforms by
    construction and cannot drift.
  - The char_* group is a placeholder that no shipped art uses. The player
    sheets in assets/ are not on-palette, and the drawn poses are warm where
    char_* is cool blue-grey.

So nothing is enforced against hand-drawn art: the style is open for iteration.

Written against nothing but the standard library, on purpose: this project has
no third-party dependencies by policy, and an authoring tool is a bad reason to
be the first exception.

Every colour below is an original value chosen to match the character a
reference sample measured -- dark, narrow value range, cool sky and warm ground
-- not a colour copied out of that reference. Nothing in this project's asset
pipeline ever reads a pixel from footage that is not ours.

Adding a colour: add it here with a one-line reason, regenerate
assets/palette.gpl (tools/export_palette_gpl.py), and every generator picks it
up. Removing or changing one in the generated groups invalidates every BMP built
from it, the same way changing a scene/legend.h value invalidates every material
map -- treat those with that weight.
"""

import os
import zlib
import struct

# --- where the repo is ------------------------------------------------------
#
# One anchor, derived from this file's location, so every script in tools/
# resolves the same way no matter what directory it is run from. Scripts that
# write to bare literals resolve against the shell's working directory instead,
# so two scripts in one directory, invoked the same way, can write into
# different trees.
#
# asset_path and src_path are for outputs. An input the caller names on the
# command line stays relative to the caller, because that is what a shell user
# means when they type a path -- see art_src/ in convert_background_layers.py,
# which is deliberately not routed through here.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def asset_path(*parts):
    """Path under the repo's assets/, regardless of the working directory."""
    return os.path.join(ROOT, 'assets', *parts)


def src_path(*parts):
    """Path under the repo's src/. Used by the two generators that write a
    header (`player_sheet.py`, `generate_backdrop.py`); those headers are
    overwritten wholesale, so writing one to the wrong tree is real damage."""
    return os.path.join(ROOT, 'src', *parts)


# --- the palette ------------------------------------------------------------
#
# Grouped by layer (four-layer visual depth model), not
# alphabetically -- the groups are the thing worth reading, the names inside
# them are arbitrary. Backdrop stays cool with zero warm colour in it, which is
# what keeps it reading as behind everything drawn over it. The terrain fill is
# warm near-black rather than cool near-black so unlit dirt still reads as earth
# instead of void; the rim is the one bright, warm accent in the terrain layer.
#
# The backdrop group's numbers are stated as post-grade luminance targets, since
# that is what reaches the screen, and the RGB is whatever hits the target on the
# hue the group already had:
#
#     sky top        76      mountain rim     57    (grade 0.60)
#     sky horizon    50      mountain body    35    (grade 0.60)
#     ground far     24      ground near      85    (grade 0.53)
#
# Building that ladder downward from a floor does not work. A per-layer Grade is
# a multiply and can therefore only darken, so each band added below an already
# dark sky is pushed further toward zero -- a whole composition inside about nine
# levels of luminance, against a reference that spans well over a hundred.
#
# Scaling the whole group by one factor is a ceiling move: every ratio between
# bands is preserved exactly and no grade is touched, which is the one kind of
# adjustment that cannot separate or merge two bands by accident. It has limited
# downward room, though, because it scales every absolute separation too -- and
# absolute separation is the quantity the ladder exists to buy. The tightest join
# is the mountain/ground horizon, and the reference's own signature there is 14
# levels. Below about a dozen the frame is back to reading flat, so if it still
# reads too bright the next move is hue and saturation (this group is a strongly
# saturated violet, and saturation reads as brightness) or the grades.
#
# `star` is deliberately not scaled. It is a point accent rather than a band, so
# it carries no ratio to preserve, and the reference's night frame keeps its
# stars and moon at full value while everything around them descends.
#
# Two relationships in that table are the point of it rather than side effects.
# ground_far is the darkest value in the frame -- the horizon as the frame's dark
# pinch -- and it sits about 14 below the mountains above it. And the sky darkens
# downward, top to horizon: brightening downward puts the frame's brightest row
# immediately above the row that is supposed to be its darkest.
PALETTE = {
    # sky - cool, deep, no warm colour anywhere in this group. sky_deep is the top
    # of the frame and sky_horizon the bottom of the sky; deep is the brighter of
    # the two.
    'sky_deep':     (0x53, 0x43, 0x8E),
    'sky_horizon':  (0x38, 0x2C, 0x57),
    'star':         (0xE8, 0xE4, 0xFF),

    # mountains - one flat silhouette tone plus a peak rim. The rim is authored to
    # land brighter than the sky behind it, because it is the one lit edge in the
    # band and a rim at or below the sky is a rim nobody can see.
    'mountain':     (0x42, 0x34, 0x63),
    'mountain_rim': (0x69, 0x55, 0x9E),

    # the ground plane (drawn behind the world) - the one band in the frame that
    # recedes within itself, so it is authored as a ramp rather than as a tone.
    # ground_far is its horizon edge and ground_near its near edge; the tile dithers
    # between them top to bottom and the strip loop in render/frame.cpp turns that
    # into distance.
    #
    # The ramp is in the art and the level is in the grade, and that split is
    # deliberate: a per-layer Grade multiplies uniformly, so it cannot make a surface
    # brighten toward the viewer -- only the tile can.
    #
    # The pair is stated as a difference in post-grade luminance rather than as a
    # ratio, and that distinction is the whole of an earlier error here. The
    # reference plane runs 77.5 -> 138.2, which is a ratio of 1.78 and a difference
    # of 61 levels; matched as a ratio down at this end of the scale it buys under
    # ten levels after the grade, and reads as no recession at all. When a mechanism
    # is absolute contrast, matching its ratio is not matching it.
    #
    # The near end is also where the ramp has to be measured, not merely authored.
    # The plane is drawn from the horizon to a near edge well below the window, the
    # world's surface sits about two thirds down that band, and plane_src_at's
    # inverse-depth mapping puts that contact around tile row 198 of 256 -- so the
    # row the eye actually reads against the terrain carries about three quarters of
    # the ramp, not all of it. Tuning aimed at the tile's last row is tuning aimed at
    # a row that is never on screen.
    #
    # What must not move with it: ground_far stays put, so the horizon stays the
    # frame's dark pinch and stays under the graded mountains -- a uniform grade
    # could not do that, which is why it is here and not in the layer table. And
    # ground_mark scales by the same factor as ground_near, so its contrast against
    # the ramp is unchanged as a ratio and grows as a difference toward the near
    # edge, which is the detail-energy ramp the reference carries.
    'ground_far':   (0x32, 0x29, 0x4E),
    'ground_near':  (0xB2, 0x97, 0xCD),
    'ground_mark':  (0xC8, 0xA9, 0xDB),

    # trees (props) - desaturated green, warmer than the sky, cooler than the
    # terrain rim, so it sits legibly between the two
    'tree_shadow':  (0x18, 0x20, 0x16),
    'tree_mid':     (0x2A, 0x38, 0x24),
    'tree_lit':     (0x47, 0x59, 0x39),
    'trunk':        (0x22, 0x1A, 0x13),

    # terrain (simulated grid) - warm near-black fill per material, one shared rim
    # colour, one dithered hand-off band
    'dirt_fill':    (0x1B, 0x16, 0x11),
    'dirt_mid':     (0x29, 0x20, 0x16),
    'wall_fill':    (0x1B, 0x16, 0x11),  # same family as dirt: reads as rock
    'sand_fill':    (0x24, 0x1C, 0x10),
    'wood_fill':    (0x17, 0x12, 0x0D),
    'rim_grass':    (0x69, 0x78, 0x3A),  # the one bright accent in the layer

    # The lit end of the same four, so the *_fill values above are the deep end of a
    # ramp rather than the material's one tone. A cell near its own surface no
    # longer gets the deep tone.
    #
    # The defect these answer, measured: the receding plane's near end reads at
    # luminance 65.7 and the world standing in front of it at 23.3 -- the frame's
    # brightest band directly behind its darkest one, at the one junction the eye is
    # asked to read as continuous. Every reference frame does the opposite: the
    # surface the character stands on is the brightest band in the frame (72.9-79.9
    # across four frames) and the frame darkens both upward and downward from it.
    # That inversion is what reads as a separate shelf in front of a painted
    # backdrop, and it is why grading a layer cannot fix it -- a Grade is uniform, so
    # a ramp inside one band is always the art's job.
    #
    # So the terrain layer is authored the way the ground plane already is: the ramp
    # is in the art and the level is in the grade. These are the near/lit end of it,
    # and they are targets rather than tastes --
    #
    #     wall lit        67      the plane's near end is 65.7; the world is
    #                             nearer than the plane, so it is not darker
    #     sand lit        78      the top of the reference's lit band (64-81)
    #     wood lit        53      a beam is an object on the surface, not the
    #                             surface; it stays the darker thing on it
    #
    # Hue follows the daylight finding: lit surfaces are warm and get warmer as they
    # brighten (+3 red-over-blue at luminance 0-20, rising to +33 at 90-130). At
    # luminance ~68 that is about +20, and these carry +17 (rock, greyer) to +41
    # (sand, the warmest thing in the layer). The *_fill end keeps its own +10, so
    # the ramp warms as it brightens rather than merely lightening.
    #
    # Every rung is named, and that is the constraint that shaped the set.
    # Interpolating between the lit tone and the fill and dithering between the
    # results puts colours into the authored BMP that appear nowhere in this file,
    # which tools/validate_palette.py correctly reports as off-palette. Every other
    # generated pass here dithers between two named colours and therefore cannot
    # leave the set. So the ramp is a ladder of palette entries and apply_depth_ramp
    # only ever picks between two adjacent rungs.
    #
    # Even steps in luminance, four rungs per material counting the fill:
    #
    #     wall   66.9 -> 52.1 -> 37.7 -> 22.9 (wall_fill)
    #     sand   77.9 -> 61.4 -> 45.4 -> 28.8 (sand_fill)
    #     wood   52.8 -> 41.6 -> 30.3 -> 18.9 (wood_fill)
    'wall_lit':     (0x49, 0x42, 0x38),  # greyer than dirt at the same level: rock
    'wall_mid':     (0x3A, 0x33, 0x2B),
    'wall_shade':   (0x2A, 0x25, 0x1E),
    'sand_lit':     (0x5C, 0x4C, 0x33),
    'sand_mid':     (0x49, 0x3C, 0x27),
    'sand_shade':   (0x37, 0x2C, 0x1C),
    'wood_lit':     (0x3E, 0x33, 0x27),
    'wood_mid':     (0x31, 0x28, 0x1E),
    'wood_shade':   (0x24, 0x1D, 0x16),

    # player (drawn between the props and the terrain's own layer - it is not a cell
    # and not a prop, it is the one sprite that moves under input).
    #
    # The separation problem this group solves: every other group above is either
    # cool-and-dark (sky, mountains) or warm-and-dark (terrain), and the trees sit
    # between them in green. A character painted in any of those families disappears
    # into whichever one it happens to be standing against. So the robe is cool
    # blue-grey -- the one hue family the terrain layer never uses -- which reads
    # against warm dirt and against green foliage without being brighter than
    # either. Value range stays inside the locked set's.
    'char_base':    (0x1C, 0x20, 0x29),  # robe shadow; the darkest character tone
    'char_mid':     (0x2A, 0x32, 0x40),  # main robe tone, cool enough to clear the foliage green
    'char_light':   (0x3F, 0x4A, 0x5E),  # shoulder and hood highlight - the silhouette's edge
    'char_belt':    (0x4A, 0x3B, 0x2A),  # rope belt and boots; warm brown, ties the figure to the ground layer
    'char_mask':    (0x11, 0x11, 0x11),  # the mask's interior void, darker than any terrain fill
    'char_accent':  (0x94, 0x51, 0x28),  # dull copper on the mask only - see the note below

    # water is the deliberate exception: it keeps more saturation than anything else
    # in the terrain layer because it has to read as water up close, not just in
    # silhouette
    'water_fill':   (0x1A, 0x29, 0x32),
    'water_rim':    (0x2E, 0x49, 0x55),
}

# char_accent is the brightest value in this entire palette, above what is
# otherwise the one bright accent in the terrain layer. That is deliberate and it
# is also the entry here most likely to need revisiting:
#
#   - It is six pixels. The whole justification is that it is the only saturated
#     thing on a figure otherwise painted in the locked dark range, so it
#     functions as a fixation point rather than as a light source. At a larger
#     area this value would be wrong.
#   - It sits in the warm-orange family reserved for Fire. Six pixels on a moving
#     figure will not be mistaken for a flame, but if the character ever gains
#     more copper -- or if the emissive pass makes warm pixels glow -- this is
#     where that reads as fire first. Check it against a burning scene before
#     adding any more.
#
# Reserved marker for sprite transparency (props, mountains) -- SDL_SetColorKey
# treats this exact colour as "no pixel here". Magenta, for the same reason
# scene/legend.h picked it for its own unused slots: nobody paints real art in
# pure magenta by accident, so a leak is obvious on sight.
COLOR_KEY = (0xFF, 0x00, 0xFF)

# --- scene legend colours ---------------------------------------------------
#
# The two entries from src/scene/legend.h's SCENE_LEGEND that the backdrop
# generators need when they write a material map: Empty and Wall. One copy here
# rather than one per generator with nothing asserting they agree.
#
# Still a copy, not a derivation, and the honest reason is scope. Parsing
# legend.h from Python would remove the drift entirely; the reason not to is
# that the legend is frozen -- legend.h says so, and LEGEND_SIZE is
# static_assert'd against ElementType::Count -- so the values cannot move without
# a deliberate edit to a file that announces itself.
# assert_legend_matches_header() below is the cheap middle: it reads the header
# when it is there and fails loudly if these two rows have moved. Call it from
# anything that writes a material map.
LEGEND_EMPTY = (0x00, 0x00, 0x00)
LEGEND_WALL = (0x88, 0x88, 0x88)


def assert_legend_matches_header():
    """Checks LEGEND_EMPTY/LEGEND_WALL against src/scene/legend.h.

    Silently does nothing if the header is missing - these tools are also run
    against a source tree without a build, and a missing header is not a
    drifted one. When the header *is* present and disagrees, this raises: a
    material map written from a stale legend loads as the wrong element, and
    the failure shows up as a scene that is subtly wrong rather than one that
    refuses to load."""
    header = src_path('scene', 'legend.h')
    if not os.path.exists(header):
        return
    with open(header, encoding='utf-8') as f:
        text = f.read()
    for name, rgb in (('Empty', LEGEND_EMPTY), ('Wall', LEGEND_WALL)):
        want = '0x{:02X}{:02X}{:02X}'.format(*rgb)
        if f'{{ {want}, ElementType::{name}' not in text.replace('  ', ' '):
            raise ValueError(
                f'tools/pixel_art.py has {name} = {want}, which is not the row '
                f'src/scene/legend.h carries. The legend is frozen; if it moved '
                f'on purpose, update this file and regenerate every material map.')


def color_of(name):
    """Palette lookup that fails loudly. Catches typos and hardcoded colors
    early."""
    if name not in PALETTE:
        raise KeyError(f"'{name}' is not in PALETTE (tools/pixel_art.py) - "
                        f"add it there first - a generator names colours, "
                        f"it does not hardcode them")
    return PALETTE[name]


# --- ordered dithering --------------------------------------------------
#
# Random per-cell jitter fights hand-placed dither instead of adding to it,
# which is why colour_jitter stays out of everything this pipeline authors. This
# is the positive case: a transition between two flat tones is stepped through a
# Bayer matrix threshold, never smoothly interpolated and never a hard edge with
# nothing between.
_BAYER_4X4 = (
    (0,  8,  2, 10),
    (12, 4, 14,  6),
    (3, 11,  1,  9),
    (15, 7, 13,  5),
)


def bayer_threshold(x, y):
    """0..1, deterministic in (x, y). Used as a per-pixel coin flip whose
    outcomes fall into the classic 4x4 ordered-dither grid instead of static."""
    return (_BAYER_4X4[y % 4][x % 4] + 0.5) / 16.0


def dither_mix(x, y, color_a, color_b, t):
    """Ordered-dithered pick between two flat colours, not a blend. t is the
    fraction of color_b: t=0 is all color_a, t=1 is all color_b, and values
    between land a proportional share of color_b's pixels in the Bayer
    pattern rather than averaging the two into a third colour no editor's
    palette would contain."""
    return color_b if bayer_threshold(x, y) < t else color_a


# --- rim light -----------------------------------------------------------
#
# A filled region reads as shadowed mass with almost no internal texture, and
# the one to two cells facing open air carry a bright, warm highlight. That is
# authored per scene, not computed by the engine -- MATERIALS' colours are
# untouched -- so this is a pre-process over the (material, albedo) buffers a
# scene generator already builds, not new engine code.
def apply_rim_light(mat, alb, width, height, empty_marker, rim_color,
                     rim_depth=2):
    """Returns a new albedo buffer. For every filled cell (mat != empty_marker)
    whose cell directly above is empty or off-grid, paints it a rim colour.
    The next (rim_depth - 1) cells downward are ordered-dithered from the rim
    colour toward whatever albedo already held there, so the hand-off is a
    dithered band rather than a hard line.

    `rim_color` is either a flat (r, g, b) applied everywhere, or a callable
    `(x, y) -> (r, g, b) | None` so a caller with more than one material in
    the buffer can give water a different rim than dirt - returning None
    skips that cell (e.g. a material that should stay unlit at its surface).

    Deliberately shape-agnostic otherwise: it only asks "is the cell above me
    filled", so it rims a slope, a ceiling-less pit wall or a standalone
    platform identically, with no per-region special-casing in the caller.
    """
    color_fn = rim_color if callable(rim_color) else (lambda x, y: rim_color)

    def filled(x, y):
        if not (0 <= x < width and 0 <= y < height):
            return False
        return mat[y * width + x] != empty_marker

    out = list(alb)
    for y in range(height):
        for x in range(width):
            if not filled(x, y) or filled(x, y - 1):
                continue
            top_color = color_fn(x, y)
            if top_color is None:
                continue
            for d in range(rim_depth):
                yy = y + d
                if not filled(x, yy):
                    break
                idx = yy * width + x
                if d == 0:
                    out[idx] = top_color
                else:
                    out[idx] = dither_mix(x, yy, alb[idx], top_color,
                                           1.0 - d / rim_depth)
    return out


# --- the depth ramp ------------------------------------------------------
#
# The terrain layer's own far-to-near ramp, and the sibling of apply_rim_light
# above: same shape of pass, same place in a generator, one question different.
# The rim asks whether this cell is at a surface; this asks how far below one it
# is.
#
# A pass over the albedo and not a Grade in frame.cpp, because a Grade is a
# uniform multiply over a whole layer: it can place a band on the value ladder
# but can never make one end of a band brighter than the other. A ramp within a
# band is always the art's job.
#
# What it costs, stated because it is the same cost the rim light already pays
# and neither is free: the tone is baked per cell, so a cell carries the depth it
# was authored at, not the depth it currently sits at. Dig a lit surface cell out
# and drop it down a shaft and it stays lit, and the freshly exposed face of a
# dig gets no lit band. If either ever reads wrongly in play, the fix is a
# renderer pass with the grid as its input.
#
# The steps are quantised and then dithered between, rather than interpolated
# smoothly: a transition is stepped through the Bayer matrix, never blended into
# tones no editor's palette would contain. `steps` is therefore how many distinct
# tones the ramp adds, and it is a small number on purpose.
def apply_depth_ramp(mat, alb, width, height, empty_marker, ramp,
                     lit_depth=4, fade_depth=24):
    """Returns a new albedo buffer, brightened toward each cell's own surface.

    For every filled cell, the depth to the nearest empty cell **straight up**
    is measured, and the albedo is set from a ladder of colours: `ramp[0]` for
    the first `lit_depth` cells, then the rest of the ladder in order, reaching
    whatever the buffer already held at `lit_depth + fade_depth`. Deeper than
    that the buffer is untouched, so a material's `*_fill` keeps meaning exactly
    what it meant - the deep tone, and the ladder's last rung.

    `ramp` is a sequence of (r, g, b) from the surface downward, or a callable
    `(x, y) -> sequence | None` - the same contract `apply_rim_light` uses, so a
    caller with several materials in one buffer gives each its own ladder and
    returns None for any that should stay flat.

    **Only ever picks between two adjacent rungs, and never averages them.** The
    ladder is palette entries, so the output is too; see the note at the ramp
    colours in PALETTE for the audit that made this the contract.

    Straight up, and not a distance field: the light in this scene comes from
    the sky, so a vertical measure is the one that matches the mechanism. It
    also makes an overhang's underside deep, which is correct, where a distance
    field would light it like a floor.

    Run it **before** `apply_rim_light`, so the rim's dithered hand-off lands on
    the lit tone rather than on the deep one - which is the join the whole pass
    exists to make continuous.
    """
    ramp_fn = ramp if callable(ramp) else (lambda x, y: ramp)

    def filled(x, y):
        if not (0 <= x < width and 0 <= y < height):
            return False
        return mat[y * width + x] != empty_marker

    out = list(alb)
    total = lit_depth + fade_depth
    for x in range(width):
        depth = 0
        for y in range(height):
            if not filled(x, y):
                depth = 0
                continue
            if depth >= total:
                depth += 1
                continue
            rungs = ramp_fn(x, y)
            if not rungs:
                depth += 1
                continue
            idx = y * width + x
            if depth < lit_depth:
                out[idx] = rungs[0]
            else:
                # The fill already in the buffer is the ladder's last rung, so there are
                # len(rungs) segments to cross over fade_depth cells.
                t = (depth - lit_depth) / float(fade_depth)
                level = t * len(rungs)
                k = min(int(level), len(rungs) - 1)
                lower = rungs[k + 1] if k + 1 < len(rungs) else alb[idx]
                out[idx] = dither_mix(x, y, rungs[k], lower, level - k)
            depth += 1
    return out


# --- BMP codec -------------------------------------------------------------
#
# 24-bit uncompressed only, matching every BMP this project writes and reads. No
# alpha channel -- SDL_LoadBMP does not give one back reliably across platforms,
# which is why transparency here is COLOR_KEY plus SDL_SetColorKey on the C++
# side rather than an alpha byte.
def write_bmp(filename, width, height, pixels_rgb):
    """pixels_rgb: row-major list of (r, g, b) tuples, top row first."""
    row_size = (width * 3 + 3) & ~3
    pixel_data_size = row_size * height
    file_size = 54 + pixel_data_size

    with open(filename, 'wb') as f:
        f.write(b'BM')
        f.write(struct.pack('<I', file_size))
        f.write(struct.pack('<H', 0))
        f.write(struct.pack('<H', 0))
        f.write(struct.pack('<I', 54))

        f.write(struct.pack('<I', 40))
        f.write(struct.pack('<i', width))
        f.write(struct.pack('<i', -height))  # negative: top-down, matches the reader below
        f.write(struct.pack('<H', 1))
        f.write(struct.pack('<H', 24))
        f.write(struct.pack('<I', 0))
        f.write(struct.pack('<I', pixel_data_size))
        f.write(struct.pack('<i', 2835))
        f.write(struct.pack('<i', 2835))
        f.write(struct.pack('<I', 0))
        f.write(struct.pack('<I', 0))

        for y in range(height):
            for x in range(width):
                r, g, b = pixels_rgb[y * width + x]
                f.write(struct.pack('<BBB', b, g, r))
            f.write(b'\x00' * (row_size - width * 3))


def read_bmp(filename):
    """Returns (width, height, pixels_rgb) for a 24-bit uncompressed BMP,
    top row first regardless of whether the file is stored top-down or
    bottom-up. Exists for tools/validate_palette.py - reading a format this
    project already writes with the standard library rather than reaching
    for Pillow, same reasoning as the rest of this file."""
    with open(filename, 'rb') as f:
        data = f.read()

    if data[0:2] != b'BM':
        raise ValueError(f"{filename}: not a BMP")

    pixel_offset = struct.unpack_from('<I', data, 10)[0]
    header_size = struct.unpack_from('<I', data, 14)[0]
    width = struct.unpack_from('<i', data, 18)[0]
    height_raw = struct.unpack_from('<i', data, 22)[0]
    bpp = struct.unpack_from('<H', data, 28)[0]
    compression = struct.unpack_from('<I', data, 30)[0]

    if bpp != 24 or compression != 0:
        raise ValueError(f"{filename}: only 24-bit uncompressed BMP is "
                          f"supported (got {bpp}-bit, compression {compression})")

    top_down = height_raw < 0
    height = abs(height_raw)
    row_size = (width * 3 + 3) & ~3

    pixels = [None] * (width * height)
    for row in range(height):
        file_row = row if top_down else (height - 1 - row)
        offset = pixel_offset + file_row * row_size
        for x in range(width):
            b, g, r = data[offset + x * 3:offset + x * 3 + 3]
            pixels[row * width + x] = (r, g, b)

    return width, height, pixels


# --- PNG codec --------------------------------------------------------------
#
# PNG is a debugging and interchange format here, never an asset format.
# Everything the engine loads is BMP, for the reasons at the codec above; PNG
# exists so a human can look at what a generator produced, and so art can round
# trip through an external editor that will not write this project's 24-bit BMP.
#
# Standard library only, same policy as the BMP codec: a PNG is a signature, an
# IHDR, one deflate stream of filtered scanlines and three CRCs.

ALPHA_THRESHOLD = 128
"""Alpha below this becomes COLOR_KEY when a PNG is flattened to BMP.

Lives here rather than in `png_to_bmp.py` because two background generators
import it and neither has anything to do with that script's job. The value is a
midpoint and nothing measured chose it - PNG alpha out of a sprite editor is
effectively binary, so anything away from the extremes behaves the same."""


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(path):
    """Returns (width, height, pixels_rgb, alpha) - pixels_rgb is row-major
    (r, g, b); alpha is a same-length list of 0-255 ints, or None if the PNG
    had no alpha channel.

    8-bit non-interlaced truecolour only (colour type 2 or 6). Palette PNGs,
    16-bit and interlaced files raise rather than being silently mishandled,
    because every one of those is something an editor export setting can
    produce by accident and all three would otherwise land as wrong pixels.

    Callers differ on what they do with the alpha: `png_to_bmp.py` wants the
    real channel back for art that was properly erased in a sprite editor,
    while `gemini_to_player_frame.py` ignores it and flood-fills instead,
    because a fresh Gemini render does not reliably carry one."""
    with open(path, 'rb') as f:
        data = f.read()

    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'{path}: not a PNG')

    width = height = bit_depth = color_type = interlace = None
    idat = bytearray()
    pos = 8
    while pos < len(data):
        length = struct.unpack_from('>I', data, pos)[0]
        ctype = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        if ctype == b'IHDR':
            (width, height, bit_depth, color_type, _comp, _filt, interlace) = \
                struct.unpack('>IIBBBBB', payload)
        elif ctype == b'IDAT':
            idat += payload
        elif ctype == b'PLTE' and color_type == 3:
            raise ValueError(f'{path}: palette PNGs are not supported - '
                              f're-export as a truecolor PNG-24')
        pos += 8 + length + 4  # length + type + data + crc

    if bit_depth != 8:
        raise ValueError(f'{path}: only 8-bit PNGs are supported (got {bit_depth}-bit)')
    if interlace:
        raise ValueError(f'{path}: interlaced PNGs are not supported - re-export non-interlaced')
    if color_type not in (2, 6):
        raise ValueError(f'{path}: only RGB (2) or RGBA (6) PNGs are supported '
                          f'(got color type {color_type})')

    channels = 3 if color_type == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    recon = bytearray(stride * height)

    pos = 0
    for y in range(height):
        filt = raw[pos]
        pos += 1
        row_start = y * stride
        prev_start = row_start - stride
        for x in range(stride):
            byte = raw[pos + x]
            a = recon[row_start + x - channels] if x >= channels else 0
            b = recon[prev_start + x] if y > 0 else 0
            c = recon[prev_start + x - channels] if y > 0 and x >= channels else 0
            if filt == 0:
                val = byte
            elif filt == 1:
                val = (byte + a) & 0xFF
            elif filt == 2:
                val = (byte + b) & 0xFF
            elif filt == 3:
                val = (byte + (a + b) // 2) & 0xFF
            elif filt == 4:
                val = (byte + _paeth(a, b, c)) & 0xFF
            else:
                raise ValueError(f'{path}: unknown PNG filter type {filt} on row {y}')
            recon[row_start + x] = val
        pos += stride

    pixels = [None] * (width * height)
    alpha = [None] * (width * height) if channels == 4 else None
    for i in range(width * height):
        o = i * channels
        pixels[i] = (recon[o], recon[o + 1], recon[o + 2])
        if alpha is not None:
            alpha[i] = recon[o + 3]
    return width, height, pixels, alpha


def _png_chunk(tag, payload):
    return (struct.pack('>I', len(payload)) + tag + payload +
            struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF))


def write_png(filename, width, height, pixels_rgb):
    """Writes an 8-bit RGB PNG (colour type 2).

    `pixels_rgb` is either a row-major sequence of (r, g, b) tuples, as
    `write_bmp` takes, or a flat bytes-like of length width*height*3 - both
    because the three copies this replaces were split between the two shapes
    and converting at every call site would be noise.

    Every scanline gets filter 0. A real encoder picks a filter per row and
    would compress better; nothing here ships a PNG, so the bytes saved would
    buy nothing and the filter search is the only complicated part of the
    format."""
    if isinstance(pixels_rgb, (bytes, bytearray, memoryview)):
        flat = bytes(pixels_rgb)
        if len(flat) != width * height * 3:
            raise ValueError(f'{filename}: expected {width * height * 3} bytes, '
                              f'got {len(flat)}')
    else:
        flat = bytearray()
        for px in pixels_rgb:
            flat += bytes(px[:3])
        flat = bytes(flat)

    scan = bytearray()
    for y in range(height):
        scan.append(0)  # filter type 0 (None)
        scan += flat[y * width * 3:(y + 1) * width * 3]

    png = b'\x89PNG\r\n\x1a\n'
    png += _png_chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
    png += _png_chunk(b'IDAT', zlib.compress(bytes(scan), 6))
    png += _png_chunk(b'IEND', b'')
    with open(filename, 'wb') as f:
        f.write(png)
