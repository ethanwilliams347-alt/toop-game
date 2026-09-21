"""Wrap a raw RGB dump from preview_light into a PNG.

    build/Release/preview_light frame.raw 90
    python tools/rawpng.py frame.raw frame.png 804 604

Uses pixel_art.py's PNG codec (standard library only).
"""
import sys

from pixel_art import write_png

raw_path, png_path, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
data = open(raw_path, 'rb').read()
assert len(data) == w * h * 3, (len(data), w * h * 3)

write_png(png_path, w, h, data)
print('wrote', png_path)
