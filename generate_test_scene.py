import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tools'))
from pixel_art import PALETTE, apply_depth_ramp, apply_rim_light, write_bmp  # noqa: E402

# Generates the fixture scene (assets/test_material.bmp, test_albedo.bmp).
#
# It is a test fixture wearing art: each region below exists to exercise one
# named system, not to look good, and anything that exercises no system belongs
# in a drawn layer instead. Sized to GRID_WIDTH/HEIGHT in main.cpp; the two must
# move together.
#
# Albedo comes from the locked palette in tools/pixel_art.py plus a rim-light
# pass, rather than from hand-picked shades close to the legend colours.
#
# Every dimension below is a scene unit put through s(), not a cell count. The
# scene was authored against a smaller player body, and a fixture that had simply
# been re-centred in a bigger world would keep exercising the wrong thing: a
# 3-cell ledge is a jump for a small body and a step for a large one, a 2-cell
# fence post is a plank at one scale and a splinter at the other. So the whole
# fixture scales with the body, and the scene units still mean what they meant.

# The spawn stands on an open run of floor, and every region is placed to leave
# it that way. The body spawns at GRID_WIDTH / 2 -- scene unit 384 -- and falls;
# landing it on a small island high above the floor leaves almost none of the
# world in frame and nearly all of the region below the player's feet showing the
# backdrop plane.
#
# That is worth knowing before tuning anything about the junction between the
# world's surface and the plane's near end: at a spawn like that there is no
# world surface in the frame to be a junction with.
#
# So the constraint this file carries, and the one to check before moving any
# region: scene units 355-430 are the spawn corridor and hold floor and nothing
# else. The regions either side were moved rather than deleted -- the pit, bridge
# and jump ledges west, the channel and the sleeper run east -- because the
# decision on this fixture is dress it and spread it out, never retire an
# exercise to make room.

# Cells per scene unit. The same factor the player's own constants moved by.
S = 2.5


def s(v):
    """A scene unit in cells."""
    return int(round(v * S))


# The world is larger than the scaled fixture, which leaves margin to the right
# and below that the floor and the sleeper run extend into -- the camera needs
# somewhere to pan that is not the fixture.
WIDTH = 1920
HEIGHT = 1080

# Legend colours must match src/scene/legend.h exactly (RGB only -- the loader
# masks off alpha before comparing), or a region silently loads as Empty instead
# of the intended material. Frozen; a palette pass must never touch these.
WALL = (0x88, 0x88, 0x88)
SAND = (0xEE, 0xDD, 0x82)
WOOD = (0x6B, 0x44, 0x23)
WATER = (0x44, 0x44, 0xFF)
EMPTY = (0x00, 0x00, 0x00)

# Albedo: the palette's per-material fill, and the deep end of a ramp rather
# than the material's one tone. The depth-ramp pass at the bottom of this file
# lightens every cell near its own surface toward the matching *_lit, and the
# rim-light pass then puts the bright edge on top of that. A cell deep enough
# below open air is still exactly the fill below.
WALL_ALB = PALETTE['wall_fill']
SAND_ALB = PALETTE['sand_fill']
WOOD_ALB = PALETTE['wood_fill']
WATER_ALB = PALETTE['water_fill']

FLOOR_TOP = s(380)  # first row of the floor slab; everything above is open


mat = [EMPTY] * (WIDTH * HEIGHT)
alb = [EMPTY] * (WIDTH * HEIGHT)


def fill_rect(x0, x1, y0, y1, material, albedo):
    for y in range(y0, y1):
        for x in range(x0, x1):
            if 0 <= x < WIDTH and 0 <= y < HEIGHT:
                idx = y * WIDTH + x
                mat[idx] = material
                alb[idx] = albedo


# --- Floor, with a pit left open for the bridge (see below) ---
PIT_X0, PIT_X1 = s(260), s(291)
fill_rect(0, PIT_X0, FLOOR_TOP, HEIGHT, WALL, WALL_ALB)
fill_rect(PIT_X1, WIDTH, FLOOR_TOP, HEIGHT, WALL, WALL_ALB)

# --- Snowbank: an uneven Sand slope for powder step-up ---
# Blocky stairs of random 1-3 cell tread height, not a smooth ramp -- a smooth
# ramp does not exercise the player's step-up over uneven powder any differently
# than flat ground does. The riser range deliberately straddles MAX_STEP_HEIGHT,
# so the slope contains lips on both sides of the threshold. Scaling the risers
# to sit entirely under the step height would lose the point of the region.
rng = random.Random(1)
slope_top = FLOOR_TOP
x = s(20)
while x < s(145):
    tread = s(rng.randint(6, 10))
    slope_top -= s(rng.randint(1, 3))
    fill_rect(x, min(x + tread, s(145)), max(slope_top, FLOOR_TOP - s(60)), FLOOR_TOP,
              SAND, SAND_ALB)
    x += tread

# --- Fence posts: standalone Wood columns for dig-the-base collapse ---
for post_x, post_top in ((s(170), s(340)), (s(200), s(350)), (s(230), s(330))):
    fill_rect(post_x, post_x + s(2), post_top, FLOOR_TOP, WOOD, WOOD_ALB)

# --- Bridge: two grounded pillars and a beam spanning the pit ---
# The two pillar/beam joints are the L-piece-keeping-its-corner case: dig out one
# pillar and the support flood fill has to carry the beam and the far corner down
# together as one piece, not shed the corner it is attached to. The pit is real
# (no floor under it), so this is the gap-underneath case rather than a beam
# resting on solid ground.
fill_rect(s(258), s(260), s(350), FLOOR_TOP, WOOD, WOOD_ALB)  # left pillar
fill_rect(PIT_X1, PIT_X1 + s(2), s(350), FLOOR_TOP, WOOD, WOOD_ALB)  # right pillar
fill_rect(s(258), PIT_X1 + s(2), s(348), s(350), WOOD, WOOD_ALB)  # spanning beam

# --- Jump ledges at mixed heights, for jump arc tuning ---
# Still a jump-arc region and not a staircase: the gaps between these scale with
# the body, so what used to need a jump still needs one against a JUMP_SPEED that
# grew by the same factor.
#
# Placed west of the spawn column and they must not come back to it -- a cluster
# straddling the spawn is one the player lands on every launch. See the
# spawn-corridor note at the top of this file.
for ledge_x, ledge_y in ((s(305), s(330)), (s(320), s(300)), (s(335), s(270)), (s(300), s(240))):
    fill_rect(ledge_x, ledge_x + s(8), ledge_y, ledge_y + s(3), WALL, WALL_ALB)

# --- Water channel: walled trench, open at the top so the player can fall in ---
# Placed far enough east of the spawn column that there is an open run of ground
# in front of the player instead of a trench wall.
CHANNEL_X0, CHANNEL_X1 = s(440), s(601)
fill_rect(CHANNEL_X0 - s(2), CHANNEL_X0, s(250), FLOOR_TOP, WALL, WALL_ALB)  # left wall
fill_rect(CHANNEL_X1, CHANNEL_X1 + s(2), s(250), FLOOR_TOP, WALL, WALL_ALB)  # right wall
fill_rect(CHANNEL_X0, CHANNEL_X1, s(310), FLOOR_TOP, WATER, WATER_ALB)
# Diving ledge above the channel's near edge -- step off it to fall through open
# air and then through the water, exercising density/displacement.
fill_rect(CHANNEL_X0, CHANNEL_X0 + s(20), s(230), s(233), WALL, WALL_ALB)

# --- Sleepers beside the water: ignite with the Fire brush, then breach the
# channel wall so the spilled Water douses them into Steam ---
sleeper_x = CHANNEL_X1 + s(4)
while sleeper_x + s(8) <= WIDTH - s(4):
    fill_rect(sleeper_x, sleeper_x + s(8), FLOOR_TOP - s(3), FLOOR_TOP, WOOD, WOOD_ALB)
    sleeper_x += s(14)

# --- depth ramp ---
#
# Gives the terrain layer the far-to-near ramp the receding ground plane already
# has, so that the world's surface picks the value ladder up where the plane's
# near end leaves it instead of dropping tens of levels across the join. Runs
# before the rim light on purpose -- see apply_depth_ramp's own note, which also
# carries the argument for why this cannot be a Grade.
#
# Water is deliberately not ramped, and that is a decision rather than an
# oversight. The lit band's job is to join the walkable surface to the plane's
# near end, and the channel's water is neither walkable nor at the plane's depth
# -- it sits well below the floor line in a trench, which is the one place in
# this scene where a surface genuinely is in shadow. It also already carries its
# own cooler rim. Returning None is the documented way to say "leave this
# material flat". Reopen it if a scene ever puts water at the surface: a lake at
# the floor line would want the ramp and would look wrong without it.
def ramp_for(x, y):
    m = mat[y * WIDTH + x]
    if m == WALL:
        return (PALETTE['wall_lit'], PALETTE['wall_mid'], PALETTE['wall_shade'])
    if m == SAND:
        return (PALETTE['sand_lit'], PALETTE['sand_mid'], PALETTE['sand_shade'])
    if m == WOOD:
        return (PALETTE['wood_lit'], PALETTE['wood_mid'], PALETTE['wood_shade'])
    return None


# lit_depth and fade_depth are cell counts and they are set from the frame, not
# from the fixture: the viewport is 480x270 cells at 1920x1080 and the player
# sits at Camera::VERTICAL_ANCHOR, so about 54 cells of world are visible below
# the feet and that is the whole budget this ramp has to spend. The reference
# spends the first ~40% of its below-contact region falling from the lit band to
# near-black and holds near-black for the rest.
#
# Cell counts rather than a fraction of the window, the same argument
# PLANE_TEXEL_SCALE makes: a fraction would put the same ramp on screen at two
# depths at 1080 and 1440. A taller window sees further down into the deep tone,
# which is what looking further down should do.
alb = apply_depth_ramp(mat, alb, WIDTH, HEIGHT, EMPTY, ramp_for,
                       lit_depth=4, fade_depth=24)

# --- rim light ---
# Every top-facing surface gets a bright edge; water gets its own cooler rim
# instead of the ground's warm one, and nothing else in the buffer is touched. It
# hands off into the depth ramp above rather than into a flat interior fill,
# which is the join the two passes are made of.
def rim_for(x, y):
    m = mat[y * WIDTH + x]
    if m == WATER:
        return PALETTE['water_rim']
    if m in (WALL, SAND, WOOD):
        return PALETTE['rim_grass']
    return None


# rim_depth scales with everything else: a 2-cell edge on a 20-cell body is the
# thin bright line the reference has, where the same 2 cells against a much
# smaller body was a quarter of its height. Measured off the reference as a
# proportion, not as a pixel count.
alb = apply_rim_light(mat, alb, WIDTH, HEIGHT, EMPTY, rim_for, rim_depth=s(2))

write_bmp('assets/test_material.bmp', WIDTH, HEIGHT, mat)
write_bmp('assets/test_albedo.bmp', WIDTH, HEIGHT, alb)
