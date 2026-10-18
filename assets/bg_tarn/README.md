# bg_tarn: a mountain lake at golden hour

Native resolution 688 x 288; one art pixel is one world cell, loaded at scale 8
by the `bg_tarn` row in `assets/scenes.txt`.

**Generated, not drawn.** `python tools/generate_bg_tarn.py` writes every BMP
here, plus `bg_tarn_material.bmp` and `bg_tarn_albedo.bmp`. It is
deterministic, so a diff here is a change to the script. This README is the one
hand-written file.

Inspired by `art_src/bg_valley` (bg_valley): its teal range, the sand / grey-green /
red-rock hill ladder with a gold rim line, the still lake and the olive meadow,
re-lit by a low sun. Every colour is a `tarn_*` name in `tools/pixel_art.py`.

## Drawn by the perspective rig, not the bg_valley banding

See `src/render/depth_rig.h`. In short: the horizon is art row 200 and the
terrain surface row 264; a layer's parallax factor is `(foot - 200) / 64`, where
`foot` is the row its lowest pixel stands on. Nothing in the table below is a
hand-picked factor except the sky, clouds, glint and reeds, which do not stand on
the ground.

| File | Foot row | Factor | Notes |
| :--- | :---: | :---: | :--- |
| bg_tarn_11_sky.bmp | - | 0.00 | Opaque. Sun, halo, banded haze. |
| bg_tarn_10_clouds.bmp | - | 0.01 | Drifts -0.6 cells/s with time. |
| bg_tarn_09_plane.bmp | - | per row | The ground plane, line-scrolled: every row at its own factor. Lake rows ripple. |
| bg_tarn_08_glint.bmp | - | 0.00 | The sun's reflection. Follows the plane's rows vertically, stays under the sun horizontally. |
| bg_tarn_07_mountains.bmp | 202 | 0.031 | |
| bg_tarn_06_ridge.bmp | 204 | 0.063 | |
| bg_tarn_05_mesas.bmp | 207 | 0.109 | |
| bg_tarn_04_dunes.bmp | 210 | 0.156 | The far shore. |
| bg_tarn_03_pines_far.bmp | 240 | 0.625 | The near shore. |
| bg_tarn_02_pines_near.bmp | 252 | 0.813 | |
| bg_tarn_01_reeds.bmp | - | 1.30 | Foreground, in front of the player. |

## Rules the art keeps (rig_test checks each)

- A standing layer's lowest painted row is exactly its table foot row.
- The plane is transparent above row 200 and has no holes from it down.
- The plane has no ordered dither: the rig stretches it vertically when the
  camera climbs, and stretched dither turns into stripes. Bands have wavy edges
  instead.
- Every layer tiles left to right (all horizontal structure is periodic in 688).
- The glint's paint lies inside its ripple rows, 212..238.

## The scene

Flat terrain at row 264 (the contact row), with a pond of real Water cells in a
dip at columns 96..152 and a Sand bank at 520..596.

## Mood

`bg_tarn_mood.txt` is the scene's light: a low warm sun, cool sky-fill shade, a thin warm haze on the far layers. It names `bg_tarn_grade.bmp`, the
colour-grade LUT strip written by `python tools/make_lut.py` (golden_hour grade),
and the material tiles in `assets/materials/`. Both are read by the lit
renderer; until `scenes.txt` gives the scene `mood=`, the scene keeps the
legacy look.

`bg_tarn_noir_mood.txt` is a second mood for the same world and backdrop: the
lake at night in the rain, Backbone-style -- cold teal ambient and moon, fog,
rain, warm sodium lamps along the shore and two neon accents, wet reflective
rock. It names `bg_tarn_noir_grade.bmp` (noir grade). It is read by the
`bg_tarn_noir` scene, which is this folder's world and layers under that mood;
it lives here rather than in a `bg_tarn_noir/` folder because nothing but the
light differs, and copying eleven layers and two maps to change the light would
be two sets drifting apart.
