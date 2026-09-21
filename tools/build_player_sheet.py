"""Builds the sheet named by player_sheet.SHEET_PATH from authored frame BMPs,
stamping each one into the slots tools/player_sheet.py's ANIMATIONS table
declares.

The sheet stops being the thing you edit and becomes a derived file, rebuilt
from the frames in assets/. A sheet edited in place has no source, so a frame
that lands in the wrong column, or an animation that half-updates with one slot
redrawn and its siblings left on the old pose, is invisible until it is on
screen. Rebuilding from frames makes both states impossible to reach.

    # one frame stamped into every declared slot
    python tools/build_player_sheet.py assets/copy_candidate_a.bmp

    # a base everywhere, with specific slots overridden as they get drawn
    python tools/build_player_sheet.py assets/copy_candidate_a.bmp \
        --frame walk:0=assets/walk_0.bmp --frame walk:1=assets/walk_1.bmp

Frames are bottom-aligned into the frame box, and that is a correctness fix
rather than a convenience. src/render/player_sprite.h anchors the sprite to its
collision box's bottom-centre, so the frame's last row is the ground line -- art
that stops one row short renders a figure hovering above every floor it stands
on, which reads as a physics bug rather than an art one. Authored art does not
reliably fill its own last row, so the alignment happens here rather than being
a rule every drawing has to remember. Content taller than FRAME_H is an error,
not a crop: silently cutting a head off is worse than refusing.

Horizontal placement is left exactly as authored. A sprite wider than the
collision box is correct by design -- sleeves hanging outside the box are art,
not body -- so there is no centring rule that could be applied without
overriding a deliberate choice.

Off-palette colour is reported, not rejected, and no palette is currently
enforced on hand-drawn art at all. A build step that refused work-in-progress
art would mean the sheet stopped being rebuildable exactly when it is changing
most.
"""
import sys
import argparse

from pixel_art import COLOR_KEY, PALETTE, read_bmp, write_bmp
from player_sheet import ANIMATIONS, FRAME_W, FRAME_H, SHEET_W, SHEET_H, SHEET_PATH


def parse_slot(spec):
    """'walk:3=path.bmp' -> ('walk', 3, 'path.bmp'); index defaults to 0."""
    if '=' not in spec:
        raise SystemExit(f"--frame needs ANIM[:INDEX]=PATH, got '{spec}'")
    slot, path = spec.split('=', 1)
    name, _, idx = slot.partition(':')
    try:
        index = int(idx) if idx else 0
    except ValueError:
        raise SystemExit(f"--frame index must be a number, got '{idx}' in '{spec}'")
    return name, index, path


def find_anim(name):
    for a in ANIMATIONS:
        if a.name == name:
            return a
    choices = ', '.join(a.name for a in ANIMATIONS)
    raise SystemExit(f"unknown animation '{name}' - choices: {choices}")


def load_frame(path):
    """Reads a frame and bottom-aligns it into a FRAME_W x FRAME_H box.
    Returns (pixels, report) where report describes what had to be moved."""
    w, h, px = read_bmp(path)
    if w != FRAME_W:
        raise SystemExit(f'{path}: is {w}px wide, expected {FRAME_W} '
                          f'(the frame width in tools/player_sheet.py)')
    if h > FRAME_H:
        raise SystemExit(f'{path}: is {h}px tall, more than the {FRAME_H}-row frame - '
                          f'a crop would cut authored art off, so this is refused')

    rows = [y for y in range(h) if any(px[y * w + x] != COLOR_KEY for x in range(w))]
    if not rows:
        raise SystemExit(f'{path}: every pixel is the transparency colour-key - '
                          f'there is no art in this file')

    shift = (FRAME_H - 1) - max(rows)
    if min(rows) + shift < 0:
        raise SystemExit(f'{path}: painted rows {min(rows)}..{max(rows)} cannot be '
                          f'bottom-aligned inside {FRAME_H} rows without clipping the top')

    out = [COLOR_KEY] * (FRAME_W * FRAME_H)
    for y in rows:
        for x in range(FRAME_W):
            out[(y + shift) * FRAME_W + x] = px[y * w + x]

    return out, shift


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('base_frame',
                    help='stamped into every declared slot not covered by --frame')
    p.add_argument('--frame', action='append', default=[], metavar='ANIM[:INDEX]=PATH',
                    help='override one slot; repeatable')
    args = p.parse_args()

    base, base_shift = load_frame(args.base_frame)
    if base_shift:
        print(f'{args.base_frame}: bottom-aligned by {base_shift} row(s) so the '
              f'feet sit on the frame\'s ground line')

    overrides = {}
    for spec in args.frame:
        name, index, path = parse_slot(spec)
        anim = find_anim(name)
        if not (0 <= index < anim.frames):
            raise SystemExit(f"'{name}' has {anim.frames} frame(s) (index 0-{anim.frames-1}), "
                              f'got {index}')
        pixels, shift = load_frame(path)
        if shift:
            print(f'{path}: bottom-aligned by {shift} row(s)')
        overrides[(anim.row, anim.col + index)] = (pixels, path)

    sheet = [COLOR_KEY] * (SHEET_W * SHEET_H)
    filled = 0
    for anim in ANIMATIONS:
        for i in range(anim.frames):
            row, col = anim.row, anim.col + i
            pixels, _src = overrides.get((row, col), (base, args.base_frame))
            ox, oy = col * FRAME_W, row * FRAME_H
            for y in range(FRAME_H):
                for x in range(FRAME_W):
                    sheet[(oy + y) * SHEET_W + (ox + x)] = pixels[y * FRAME_W + x]
            filled += 1

    write_bmp(SHEET_PATH, SHEET_W, SHEET_H, sheet)
    print(f'wrote {SHEET_PATH} ({SHEET_W}x{SHEET_H}): {filled} slot(s) filled, '
          f'{len(overrides)} from --frame')

    off = {c for c in sheet if c not in set(PALETTE.values()) and c != COLOR_KEY}
    if off:
        print(f'note: {len(off)} distinct off-palette colour(s) in the sheet - expected '
              f'while the art is still being drawn; tools/snap_to_palette.py is the '
              f'step that fixes it, and tools/validate_palette.py is what checks it')
    print('run: python tools/player_sheet.py --validate')


if __name__ == '__main__':
    sys.exit(main())
