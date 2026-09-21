"""Snaps a BMP's colours to the nearest entries in PALETTE
(tools/pixel_art.py) - the step between a hand-touched-up candidate sprite
and palette conformance.

Defaults to only the six `char_*` entries (the player group), not the whole
PALETTE, ensuring character hues stay distinct from environment tones. Pass
--full-palette for a non-character asset where that restriction does not apply.

    python tools/snap_to_palette.py assets/candidate_a.bmp assets/candidate_a.bmp
    python tools/snap_to_palette.py assets/some_prop.bmp out.bmp --full-palette
"""
import sys
import argparse

from pixel_art import PALETTE, COLOR_KEY, read_bmp, write_bmp

# The player group palette entries, not derived programmatically -
# PALETTE's own comment says it is grouped by layer rather than by a naming
# convention every entry follows, so this list is named explicitly rather
# than guessed at from key prefixes.
CHAR_KEYS = ['char_base', 'char_mid', 'char_light', 'char_belt', 'char_mask', 'char_accent']


def _dist2(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2 + (a[2] - b[2]) ** 2


def nearest(color, candidates):
    return min(candidates, key=lambda kv: _dist2(color, kv[1]))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('input_bmp')
    p.add_argument('output_bmp')
    p.add_argument('--full-palette', action='store_true',
                    help='search all of PALETTE instead of just the char_* group')
    args = p.parse_args()

    keys = list(PALETTE.keys()) if args.full_palette else CHAR_KEYS
    candidates = [(k, PALETTE[k]) for k in keys]

    width, height, pixels = read_bmp(args.input_bmp)
    counts = {}
    out = []
    for px in pixels:
        if px == COLOR_KEY:
            out.append(COLOR_KEY)
            continue
        name, color = nearest(px, candidates)
        counts[name] = counts.get(name, 0) + 1
        out.append(color)

    write_bmp(args.output_bmp, width, height, out)

    print(f'wrote {args.output_bmp}: {width}x{height}, snapped against '
          + ('the full palette' if args.full_palette else 'char_* only'))
    for name, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f'  {name}: {n} px')


if __name__ == '__main__':
    sys.exit(main())
