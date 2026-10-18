"""Turns angler_probe's raw frames into a GIF and a contact sheet.

    python tools/angler_frames.py walk.raw walk [columns] [picks]

writes walk.gif (every frame, 3x) and walk_sheet.png (`picks` frames spread
evenly over the run, `columns` across). Needs Pillow.
"""

import struct
import sys

from PIL import Image

SCALE = 3


def read_frames(path):
    frames = []
    with open(path, "rb") as f:
        while True:
            hdr = f.read(8)
            if len(hdr) < 8:
                return frames
            w, h = struct.unpack("<ii", hdr)
            data = f.read(w * h * 4)
            img = Image.frombuffer("RGBA", (w, h), data, "raw", "BGRA", 0, 1)
            frames.append(img.convert("RGB"))


def main():
    raw, stem = sys.argv[1], sys.argv[2]
    cols = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    picks = int(sys.argv[4]) if len(sys.argv) > 4 else 18
    frames = read_frames(raw)
    if not frames:
        sys.exit(f"{raw}: no frames")
    big = [f.resize((f.width * SCALE, f.height * SCALE), Image.NEAREST) for f in frames]
    big[0].save(stem + ".gif", save_all=True, append_images=big[1:], duration=33, loop=0)
    chosen = frames[:: max(1, len(frames) // picks)][:picks]
    w, h = chosen[0].size
    rows = (len(chosen) + cols - 1) // cols
    sheet = Image.new("RGB", (w * cols, h * rows))
    for i, fr in enumerate(chosen):
        sheet.paste(fr, ((i % cols) * w, (i // cols) * h))
    sheet = sheet.resize((sheet.width * 2, sheet.height * 2), Image.NEAREST)
    sheet.save(stem + "_sheet.png")
    print(len(frames), "frames")


if __name__ == "__main__":
    main()
