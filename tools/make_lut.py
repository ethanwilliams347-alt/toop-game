"""Generates the colour-grade LUT strips the mood files name with `grade`.

    python tools/make_lut.py                       # every scene's strip
    python tools/make_lut.py --grade noir --out x.bmp   # one named grade, anywhere

With no arguments it writes the strip each shipped mood names, into that scene's
own folder (assets/README.md: everything a scene loads lives in its folder under
its name) and stages it into any configured build. Deterministic and
standard-library-only, like every generator in tools/: a diff in a *_grade.bmp is
a change to this script.

## The strip, for whoever writes the shader that reads it (R5)

A 32x32x32 colour cube unwrapped into one 1024 x 32 24-bit BMP: 32 slices of
32 x 32 side by side, blue choosing the slice.

    image column x, image row y (row 0 = the TOP row of the picture)
        input red   = (x % 32) / 31
        input green =  y       / 31
        input blue  = (x // 32) / 31
    the texel holds the graded colour for that input.

So the identity strip is a red ramp left to right inside every slice, green
increasing DOWN the image, and slices brightening in blue. If the loader uploads
the BMP top row first (as SDL_LoadBMP's surface is), texture row 0 is green 0;
an upload that flips rows flips green, which the identity strip shows at once.

Sample it at texel centres -- `(c * 31 + 0.5) / 32` within a slice -- and blend
the two blue slices by hand; bilinear filtering across the slice seam is the
classic bug. The input is the colour after tonemapping, display-encoded (sRGB)
in 0..1, which is the space the grades below are written in.

## The grades

Each is a function on display-encoded RGB in 0..1. Written gently on purpose:
the albedo art already carries its palette, and the grade's job is the last few
percent -- Rain World's per-scene palette shift, Cast n Chill's radiance, or
Backbone's split tone -- not a repaint.

  identity        no change. For parity tests: a mood using it must match legacy.
  daylight        Cast n Chill daylight: a mild S-curve, a touch more saturation,
                  warm highlights and very slightly cool shadows.
  golden_hour     daylight leaned warmer for bg_tarn's low sun: amber highlights,
                  a little more contrast.
  noir            Backbone: muted greys and browns, blacks lifted to a cold teal,
                  highlights pushed toward sodium amber, and saturation pulled
                  down everywhere EXCEPT colours already strongly saturated, so
                  neon and sodium light survive the grade while the city does not.
"""

import argparse
import math
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO_ROOT, 'tools'))
from pixel_art import asset_path, stage, write_bmp

N = 32


# --- small colour helpers -------------------------------------------------------


def clamp01(v):
    return 0.0 if v < 0.0 else 1.0 if v > 1.0 else v


def luma(c):
    """Rec. 709 weights on the display-encoded value. Not a true luminance -- that
    wants linear light -- but it is the grading-tool convention, and what a
    colourist means by "the shadows" is this number."""
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def mix(a, b, t):
    return tuple(x + (y - x) * t for x, y in zip(a, b, strict=True))


def saturation(c):
    hi, lo = max(c), min(c)
    return 0.0 if hi <= 0.0 else (hi - lo) / hi


def scale_saturation(c, k):
    """Moves `c` away from (k > 1) or toward (k < 1) its own grey."""
    y = luma(c)
    return tuple(y + (x - y) * k for x in c)


def s_curve(v, amount, pivot=0.5):
    """A contrast curve that keeps 0, `pivot` and 1 where they are: a smoothstep
    blended in by `amount`, applied in a space bent by a power so that `pivot`
    sits at the smoothstep's own fixed point, 0.5."""
    v = clamp01(v)
    g = math.log(0.5) / math.log(pivot)
    u = v**g
    u += (u * u * (3 - 2 * u) - u) * amount
    return u ** (1 / g)


def split_tone(c, shadow, highlight, amount=1.0):
    """Adds `shadow` weighted toward the dark end and `highlight` toward the light
    end, the way a colourist's lift/gain wheels do."""
    y = clamp01(luma(c))
    ws, wh = (1 - y) ** 2 * amount, y * y * amount
    return tuple(x + s * ws + h * wh for x, s, h in zip(c, shadow, highlight, strict=True))


# --- the named grades -----------------------------------------------------------


def identity(c):
    return c


def daylight(c):
    c = tuple(s_curve(x, 0.12) for x in c)
    c = scale_saturation(c, 1.08)
    return split_tone(c, shadow=(-0.008, 0.004, 0.022), highlight=(0.030, 0.014, -0.020))


def golden_hour(c):
    c = tuple(s_curve(x, 0.16) for x in c)
    c = scale_saturation(c, 1.06)
    return split_tone(c, shadow=(-0.010, 0.010, 0.030), highlight=(0.055, 0.024, -0.035))


def noir(c):
    # Saturation down, except where it is already high: a muted world keeps 45%
    # of its chroma, a neon sign close to all of it.
    s = saturation(c)
    keep = 0.45 + 0.55 * s * s
    c = scale_saturation(c, keep)
    # Contrast with a soft shoulder, so sodium highlights roll off instead of
    # clipping to flat yellow.
    c = tuple(s_curve(x, 0.22) for x in c)
    c = tuple(x if x < 0.8 else 0.8 + (x - 0.8) * 0.75 for x in c)
    # Blacks lifted to a cold teal-black: Backbone's shadows are deep but never
    # empty, and never neutral.
    c = tuple(0.035 + x * (1 - 0.035) for x in c)
    return split_tone(c, shadow=(-0.030, 0.030, 0.050), highlight=(0.060, 0.025, -0.045))


GRADES = {
    'identity': identity,
    'daylight': daylight,
    'golden_hour': golden_hour,
    'noir': noir,
}

# What each shipped mood names, and with which grade. A strip per scene rather
# than one shared file because a mood resolves `grade` inside its own folder.
SCENE_STRIPS = [
    ('bg_valley', 'bg_valley_grade.bmp', 'daylight'),
    ('bg_valley_wide', 'bg_valley_wide_grade.bmp', 'daylight'),
    ('bg_tarn', 'bg_tarn_grade.bmp', 'golden_hour'),
    ('bg_tarn', 'bg_tarn_noir_grade.bmp', 'noir'),
    ('bg_tarn_wide', 'bg_tarn_wide_grade.bmp', 'golden_hour'),
]


def strip(grade):
    """The 1024 x 32 strip for one grade, row-major, top row first."""
    px = []
    for y in range(N):
        for x in range(N * N):
            c = ((x % N) / (N - 1), y / (N - 1), (x // N) / (N - 1))
            px.append(tuple(int(round(clamp01(v) * 255)) for v in grade(c)))
    return px


def lookup(px, rgb):
    """Applies a strip to one 0..255 colour by trilinear interpolation, the same
    sampling the shader does. For previews and for checking a strip by eye."""
    f = [v / 255 * (N - 1) for v in rgb]
    i0 = [min(int(v), N - 2) for v in f]
    t = [v - i for v, i in zip(f, i0, strict=True)]
    out = [0.0, 0.0, 0.0]
    for dr in (0, 1):
        for dg in (0, 1):
            for db in (0, 1):
                w = (
                    (t[0] if dr else 1 - t[0])
                    * (t[1] if dg else 1 - t[1])
                    * (t[2] if db else 1 - t[2])
                )
                x = (i0[2] + db) * N + i0[0] + dr
                c = px[(i0[1] + dg) * N * N + x]
                for k in range(3):
                    out[k] += c[k] * w
    return tuple(int(round(v)) for v in out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--grade', choices=sorted(GRADES), help='write just this grade')
    ap.add_argument('--out', help='where to write it (with --grade)')
    args = ap.parse_args()

    if args.grade:
        out = args.out or f'lut_{args.grade}.bmp'
        write_bmp(out, N * N, N, strip(GRADES[args.grade]))
        print(out)
        return

    for scene, name, grade in SCENE_STRIPS:
        path = asset_path(scene, name)
        write_bmp(path, N * N, N, strip(GRADES[grade]))
        stage(path, scene)
        print(f'{scene}/{name}  ({grade})')


if __name__ == '__main__':
    main()
