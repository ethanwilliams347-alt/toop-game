# bg_valley_wide: the valley, wide

Native resolution 688 x 288, exactly twice `bg_valley` on both axes; one art
pixel is one world cell, loaded at scale 10 by the `bg_valley_wide` row in
`assets/scenes.txt`.

**Generated, not drawn.** `python tools/generate_bg_valley_wide.py` writes every
BMP here, including `bg_valley_wide_material.bmp` and `bg_valley_wide_albedo.bmp`,
out of `art_src/bg_valley/`'s own pixels: every colour, silhouette and band
boundary is read off the source and re-laid-out at the larger size. It is
deterministic, so a diff here is a change to the script or to the source art.
The script's header has the argument for how the extra columns and rows are
filled. This README is the one hand-written file.

## Layers

The factors are `bg_valley`'s, unchanged: the same nine depths of the same
place. The ground's bands are `bg_valley`'s moved down by 144 rows, the frame's
growth. `backdrop_set_test` checks the two sets against each other.

| File | Factor |
| :--- | :---: |
| bg_valley_wide_09_sky.bmp | 0.04, opaque |
| bg_valley_wide_08_ground.bmp | banded 0.30 / 0.70 / 1.00 (cuts at rows 226, 244) |
| bg_valley_wide_07_mountains.bmp | 0.12 |
| bg_valley_wide_06_hills_far.bmp | 0.20 |
| bg_valley_wide_05_hills_midfar.bmp | 0.30 |
| bg_valley_wide_04_hills_mid.bmp | 0.42 |
| bg_valley_wide_03_hills_midnear.bmp | 0.55 |
| bg_valley_wide_02_hills_near.bmp | 0.70 |
| bg_valley_wide_01_fg_rocks.bmp | 1.00, foreground |
