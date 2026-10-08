# bg_tarn_wide: the tarn as a long walk

bg_tarn's mountain lake at sunset, redrawn in the style of Cast n Chill /
Wild n Chill and stretched into one scene 2752 cells wide (8 screens at
3440x1440, about 14 at 1920x1080) that the camera follows end to end. Loaded at
scale 10 by the `bg_tarn_wide` row in `assets/scenes.txt`; its level is
`assets/bg_tarn_wide_level.txt`.

**Generated, not drawn.** `python tools/generate_bg_tarn_wide.py` writes every
BMP here, `composite_preview.png`, and `assets/bg_tarn_wide_material.bmp` /
`bg_tarn_wide_albedo.bmp`. Deterministic: a diff here is a change to the script.
This README is the one hand-written file. Every colour is a `wt_*` name in
`tools/pixel_art.py`.

`composite_preview.png` is a map of the walk, not a frame: each column is what
stands behind that world column when the camera is centred on it.

## The style

Backlit sunset: every layer is a silhouette lit along its top edge, and value
climbs toward the sky with distance (indigo-teal near, violet, lavender far).
Sky and lake are flat bands with stepped edges, not dithered ramps. Autumn
maples and birches are the one saturated thing in the middle ground.

## The walk (world columns)

| Columns | Zone | What is there |
| :--- | :--- | :--- |
| 0-520 | Pinewood camp | Dense pines, a tent with its lamp on, a campfire. Player starts at 80. |
| 520-1150 | Autumn shore | Maples and birches, the plank bridge over a pond (Wood over Water), a sand beach, the first dock. |
| 1150-1750 | Open tarn | Low shore and reeds, the islands and a canoe in view, a bench, a stone knoll. |
| 1750-2250 | Cabin cove | Log cabin with lit windows and chimney smoke, woodpile, lantern, second dock. |
| 2250-2752 | Rocky narrows | Boulders and pines, a waterfall across the water, the troll's rise, the objective at 2620. |

## Layers

Same rig as bg_tarn (horizon 200, contact 264, vertical strength 0.75).

| File | Moves | Width | Notes |
| :--- | :---: | :---: | :--- |
| 13_sky | 0.00 | 688 | Opaque. Banded sunset, ringed sun. |
| 12_clouds | 0.01, drift -0.6 | 688 | Flat-bottomed cumulus, lit undersides. |
| 11_plane | per row | 2752 | Lake mirroring the sky's bands, shore, meadow. The docks are painted on it, so they converge to the horizon by themselves. |
| 10_reflection | 0.156 on plane | 1376 | The far treeline upside down, at the far shore's own factor so the two stay aligned. |
| 09_glint | 0.00 on plane | 688 | Sun's reflection. |
| 08_mountains | foot 202 | 688 | |
| 07_ridge | foot 204 | 688 | Forested ridge. |
| 06_hills | foot 207 | 688 | Forested hills. |
| 05_far_shore | foot 210 | 1376 | Far treeline, cliff and waterfall. |
| 04_islets | foot 224 | 1376 | Islands and the canoe. |
| 03_shore | foot 240 | 2752 | Near shore trees, by zone. |
| 02_near | foot 252 | 2752 | Big trees and the set pieces. |
| 01_foreground | 1.30 | 1376 | Grass and ferns in front of the player. |

Why the far layers are narrower: `src/render/backdrop_set.h`, "why a layer may be
narrower than its set". `backdrop_set_test` checks each width against the widest
window.
