# bg_valley: the valley

Native resolution 344 x 144; one art pixel is one world cell, loaded at scale 10
by the `bg_valley` row in `assets/scenes.txt`.

**Converted, not generated.** Ethan's hand-painted source is
`art_src/bg_valley/`, one PNG per layer under the same names as the BMPs here.
`python tools/convert_bg_valley.py` turns them into the nine layer BMPs and
writes `bg_valley_material.bmp` and `bg_valley_albedo.bmp`, the floor the player
stands on. Edit the PNGs and re-run it rather than editing the BMPs. This README
is the one hand-written file.

`bg_valley_wide` is the same place at 688 x 288.

## Layers

Corner-anchored and locked vertically; the factors are the art's own, and
`backdrop.txt` has the argument for each.

| File | Factor | Notes |
| :--- | :---: | :--- |
| bg_valley_09_sky.bmp | 0.04 | Opaque. |
| bg_valley_08_ground.bmp | banded 0.30 / 0.70 / 1.00 | The one land plane every hill stands on. |
| bg_valley_07_mountains.bmp | 0.12 | |
| bg_valley_06_hills_far.bmp | 0.20 | |
| bg_valley_05_hills_midfar.bmp | 0.30 | Foot at row 73. |
| bg_valley_04_hills_mid.bmp | 0.42 | |
| bg_valley_03_hills_midnear.bmp | 0.55 | |
| bg_valley_02_hills_near.bmp | 0.70 | Foot at row 89. |
| bg_valley_01_fg_rocks.bmp | 1.00 | Foreground, in front of the player. |
