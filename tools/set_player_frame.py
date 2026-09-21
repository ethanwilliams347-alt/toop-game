"""Pastes one finished 14x26 frame into the sheet named by SHEET_PATH at the slot a
named animation (and, for multi-frame ones, an index within it) declares -
the last step before the sprite is actually the thing main.cpp draws. Reuses
tools/player_sheet.py's own ANIMATIONS table and layout constants rather than
asking for a raw (row, col), so a slot can't be aimed at a column an
animation doesn't own.

    python tools/set_player_frame.py assets/candidate_a.bmp idle
    python tools/set_player_frame.py assets/candidate_a.bmp walk 3

Run tools/player_sheet.py --validate afterward - this script only places
pixels, it does not re-check baselines or off-palette colours, both of which
--validate already exists to catch.
"""
import sys
import os
import argparse

from pixel_art import read_bmp, write_bmp
from player_sheet import ANIMATIONS, FRAME_W, FRAME_H, SHEET_PATH, SHEET_W, SHEET_H


def find_anim(name):
    for a in ANIMATIONS:
        if a.name == name:
            return a
    choices = ', '.join(a.name for a in ANIMATIONS)
    raise SystemExit(f"unknown animation '{name}' - choices: {choices}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('frame_bmp')
    p.add_argument('anim')
    p.add_argument('index', type=int, nargs='?', default=0,
                    help='frame index within the animation (default 0)')
    args = p.parse_args()

    anim = find_anim(args.anim)
    if not (0 <= args.index < anim.frames):
        raise SystemExit(f"'{args.anim}' has {anim.frames} frame(s) (index 0-{anim.frames - 1}), "
                          f"got {args.index}")

    fw, fh, frame_px = read_bmp(args.frame_bmp)
    if (fw, fh) != (FRAME_W, FRAME_H):
        raise SystemExit(f"{args.frame_bmp} is {fw}x{fh}, expected {FRAME_W}x{FRAME_H} - "
                          f"run it through tools/gemini_to_player_frame.py first")

    if not os.path.exists(SHEET_PATH):
        raise SystemExit(f"{SHEET_PATH} does not exist - run: python tools/player_sheet.py --starter")

    sw, sh, sheet_px = read_bmp(SHEET_PATH)
    if (sw, sh) != (SHEET_W, SHEET_H):
        raise SystemExit(f"{SHEET_PATH} is {sw}x{sh}, expected {SHEET_W}x{SHEET_H} - "
                          f"its layout has drifted from tools/player_sheet.py's table")

    col = anim.col + args.index
    ox, oy = col * FRAME_W, anim.row * FRAME_H
    for y in range(FRAME_H):
        for x in range(FRAME_W):
            sheet_px[(oy + y) * sw + (ox + x)] = frame_px[y * FRAME_W + x]

    write_bmp(SHEET_PATH, sw, sh, sheet_px)
    print(f'wrote {args.frame_bmp} into {SHEET_PATH} at {args.anim}[{args.index}] '
          f'(row {anim.row}, col {col})')
    print('run: python tools/player_sheet.py --validate')


if __name__ == '__main__':
    sys.exit(main())
