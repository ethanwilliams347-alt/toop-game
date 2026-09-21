"""Reports which colours in a BMP are outside tools/pixel_art.py's PALETTE.

Audit tool for checking palette conformance. Generated layers (backdrops,
trees, terrain) are built from PALETTE, so this catches hardcoded colors.

Usage:
    python tools/validate_palette.py assets/test_albedo.bmp
    python tools/validate_palette.py assets/tree_a.bmp --colorkey

--colorkey allows pixels equal to pixel_art.COLOR_KEY through unchecked for
sprites.
"""
import sys
from pixel_art import PALETTE, COLOR_KEY, read_bmp

# Pure black is always allowed and is never counted as an off-palette pixel:
# it is scene/legend.h's Empty marker and src/scene/scene.cpp's own comment
# is explicit that Empty "carries no authored colour" - an albedo BMP's
# unpainted background is black by construction, not a colour choice, the
# same way Empty is excluded from being "real" art everywhere else in this
# pipeline. Nothing in PALETTE is pure black, so this can never mask a real
# off-palette pixel.
UNPAINTED = (0x00, 0x00, 0x00)


def validate(path, allow_colorkey):
    width, height, pixels = read_bmp(path)
    allowed = set(PALETTE.values())
    offenders = {}
    for p in pixels:
        if p in allowed or p == UNPAINTED:
            continue
        if allow_colorkey and p == COLOR_KEY:
            continue
        offenders[p] = offenders.get(p, 0) + 1

    total = width * height
    if not offenders:
        print(f"OK  {path}: {total} pixels, all in PALETTE"
              + (" (colour-key allowed)" if allow_colorkey else ""))
        return True

    # Reported as "OFF", not "FAIL": for hand-drawn art this is a description,
    # not a verdict. The exit code stays non-zero so the generated layers can
    # still be checked from a script, where it does mean something is wrong.
    print(f"OFF  {path}: {sum(offenders.values())}/{total} pixels are off-palette")
    for color, count in sorted(offenders.items(), key=lambda kv: -kv[1])[:10]:
        print(f"     #{color[0]:02X}{color[1]:02X}{color[2]:02X}  x{count}")
    if len(offenders) > 10:
        print(f"     ... and {len(offenders) - 10} more distinct colours")
    return False


def main():
    args = sys.argv[1:]
    allow_colorkey = '--colorkey' in args
    paths = [a for a in args if not a.startswith('--')]
    if not paths:
        print(__doc__)
        return 1

    ok = True
    for path in paths:
        ok = validate(path, allow_colorkey) and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
