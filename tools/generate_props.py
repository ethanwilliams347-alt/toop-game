"""Generates tree sprites for the props layer: non-simulated, colour-keyed,
drawn at roughly terrain depth.

Sized in world cells (1 BMP pixel = 1 world cell).

Run from the repo root:
    python tools/generate_props.py
"""
import random
from pixel_art import PALETTE, COLOR_KEY, asset_path, dither_mix, write_bmp


def generate_tree(path, w, h, seed, tiers=4):
    # The seed is what makes three trees three trees rather than one shape at three
    # scales.
    #
    # What it varies is deliberately small -- a per-tier width wobble and a slight
    # vertical stagger -- because the silhouette is doing the work at this size, and
    # a tree that varies too much stops reading as the same species as the one
    # beside it.
    rng = random.Random(seed)
    tier_jitter = [rng.uniform(0.82, 1.06) for _ in range(tiers)]
    # Stagger as a fraction of the tree rather than a cell count. Everything else in
    # this function is already proportional to w/h, and an absolute number here
    # becomes an invisible wobble instead of a tier separation as soon as the sprite
    # is rescaled.
    tier_lift = [rng.randint(0, max(2, h // 16)) for _ in range(tiers)]
    pixels = [COLOR_KEY] * (w * h)
    cx = w // 2

    trunk_h = max(3, h // 6)
    trunk_w = max(2, w // 10)
    for y in range(h - trunk_h, h):
        for x in range(cx - trunk_w // 2, cx + trunk_w // 2 + 1):
            if 0 <= x < w:
                pixels[y * w + x] = PALETTE['trunk']

    canopy_bottom = h - trunk_h + 2  # slight overlap so the trunk isn't detached
    tier_height = canopy_bottom // tiers
    max_half = w / 2.0 - 1

    for t in range(tiers):
        tier_bottom_y = canopy_bottom - t * tier_height - tier_lift[t]
        tier_top_y = tier_bottom_y - tier_height - (2 if t == 0 else 0)
        bottom_half = max_half * (tiers - t) / tiers * tier_jitter[t]
        top_half = max(1.0, max_half * (tiers - t - 1) / tiers * 0.4 + 1)

        for y in range(max(tier_top_y, 0), min(tier_bottom_y, h)):
            span = max(1, tier_bottom_y - tier_top_y)
            frac = (y - tier_top_y) / span
            half = top_half + (bottom_half - top_half) * frac
            half = max(1.0, half)

            for dx in range(-int(half), int(half) + 1):
                x = cx + dx
                if not (0 <= x < w):
                    continue
                # Silhouette-first shading: dark on the left, lit on the right, one
                # dithered hand-off between each flat band -- the same rule the terrain's
                # rim uses, applied to a sprite instead of a filled region.
                edge = dx / half if half > 0 else 0.0
                if edge < -0.5:
                    color = PALETTE['tree_shadow']
                elif edge > 0.55:
                    color = dither_mix(x, y, PALETTE['tree_mid'],
                                        PALETTE['tree_lit'], (edge - 0.55) / 0.45)
                else:
                    color = dither_mix(x, y, PALETTE['tree_shadow'],
                                        PALETTE['tree_mid'], (edge + 0.5) / 1.05)
                pixels[y * w + x] = color

    write_bmp(path, w, h, pixels)
    print(f'wrote {path} ({w}x{h})')


if __name__ == '__main__':
    # Sized with the player body and the fixture scene. One BMP pixel is one world
    # cell, so a tree that was not rescaled with the body goes from standing several
    # times the player's height to barely one and a half -- shrubs, not the canopy
    # the reference has the character walking under. Against the current body these
    # are 3x to 4.75x its height, which is where the reference sits.
    generate_tree(asset_path('tree_a.bmp'), 45, 80, seed=11, tiers=4)
    generate_tree(asset_path('tree_b.bmp'), 35, 60, seed=17, tiers=3)
    generate_tree(asset_path('tree_c.bmp'), 55, 95, seed=23, tiers=5)
