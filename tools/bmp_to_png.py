"""Wraps an authored BMP into a PNG for preview.

    python tools/bmp_to_png.py assets/backdrop_mountains.bmp out.png
"""
import sys

from pixel_art import read_bmp, write_png

bmp_path, png_path = sys.argv[1], sys.argv[2]
w, h, pixels = read_bmp(bmp_path)
write_png(png_path, w, h, pixels)
print('wrote', png_path)
