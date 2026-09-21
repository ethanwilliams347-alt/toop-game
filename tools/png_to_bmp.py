"""Converts a PNG back into this project's BMP format (24-bit uncompressed,
COLOR_KEY-transparent - see tools/pixel_art.py's BMP codec comment) after a
round trip through an external sprite editor.

The reason this exists rather than saving straight back to .bmp from the
editor: LibreSprite (like Aseprite, the project it's forked from) exports BMP
as 32-bit for an RGB-mode sprite, not the 24-bit format tools/pixel_art.py's
read_bmp requires - so a file saved directly as .bmp from the editor can fail
tools/validate_palette.py or tools/player_sheet.py --validate for a format
reason that has nothing to do with the art. Exporting PNG from the editor and
converting here sidesteps that: this project already writes PNG for output
(tools/bmp_to_png.py) and reads it for the Gemini pipeline
(tools/gemini_to_player_frame.py, which this reuses), so PNG is the one
format both ends already handle correctly.

Alpha, if the PNG has it, decides transparency directly (below 128 becomes
COLOR_KEY) - unlike gemini_to_player_frame.py's flood-fill, which exists only
because a fresh Gemini render doesn't reliably carry a real alpha channel. A
properly erased sprite-editor export does, so use it instead of re-guessing
the background from colour.

    python tools/png_to_bmp.py edited.png assets/candidate_a.bmp
"""
import sys
import argparse

from pixel_art import ALPHA_THRESHOLD, COLOR_KEY, read_png, write_bmp


def convert(width, height, pixels, alpha):
    if alpha is None:
        return list(pixels)
    out = []
    nudged = 0
    for px, a in zip(pixels, alpha):
        if a < ALPHA_THRESHOLD:
            out.append(COLOR_KEY)
            continue
        if px == COLOR_KEY:
            px = (px[0], px[1], 254)
            nudged += 1
        out.append(px)
    if nudged:
        print(f'note: {nudged} opaque pixel(s) were exactly the transparency '
              f'colour-key and got nudged by 1 so they don\'t vanish at render time')
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('input_png')
    p.add_argument('output_bmp')
    args = p.parse_args()

    width, height, pixels, alpha = read_png(args.input_png)
    out = convert(width, height, pixels, alpha)
    write_bmp(args.output_bmp, width, height, out)

    print(f'wrote {args.output_bmp}: {width}x{height}'
          + (' (alpha -> colour-key)' if alpha is not None else ' (no alpha channel; copied as-is)'))


if __name__ == '__main__':
    sys.exit(main())
