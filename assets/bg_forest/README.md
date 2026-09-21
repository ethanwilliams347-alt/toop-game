# Forest trail (bg_forest) multi-layer set

Native resolution: 688 x 288 px. One art pixel is one world cell, so this is a
world 688 cells wide and 288 tall, loaded at scale 10 by the `bg_forest` row in
`assets/scenes.txt`.

**Generated, not drawn.** `python tools/generate_bg_forest.py` writes every file
here; edits made to the BMPs by hand are overwritten on the next run. It is
deterministic - two runs produce byte-identical output - so a diff in this
directory is a real change to the generator and never a re-roll.

**This is the first authored set that is not `bg1`'s landscape.** `bg1_ext` and
`bg_gemini` are the same place as `bg1` at 688x288; this one is a different
place at the same camera. The parallax ladder and the ground band table are
`bg_gemini`'s unchanged, because a factor is one over a depth and the depths did
not move - only what stands at them did.

## Parallax Layer Table (Back-to-Front)

| Filename | Parallax Factor | Transparency | Description |
| :--- | :--- | :--- | :--- |
| bg_forest_09_sky.bmp | 0.04 | Opaque | Sky gradient with scattered stars |
| bg_forest_08_ground.bmp | Banded | Color-keyed (Magenta) | Forest floor with trail bed |
| bg_forest_07_mountains.bmp | 0.12 | Color-keyed (Magenta) | Distant mountain ridge |
| bg_forest_06_hills_far.bmp | 0.20 | Color-keyed (Magenta) | Far hills with rounded treeline |
| bg_forest_05_hills_midfar.bmp | 0.30 | Color-keyed (Magenta) | Mid-far hills with lit canopy edges |
| bg_forest_04_hills_mid.bmp | 0.42 | Color-keyed (Magenta) | Mid-ground hills and trees |
| bg_forest_03_hills_midnear.bmp | 0.55 | Color-keyed (Magenta) | Mid-near forest mass with trunks |
| bg_forest_02_hills_near.bmp | 0.70 | Color-keyed (Magenta) | Near forest canopy and detailed trunks |
| bg_forest_01_fg_rocks.bmp | 1.00 | Color-keyed (Magenta) | Foreground rocks, roots, and overhead canopy |

### Ground Banding Factors:
- Rows 0..164: Factor 0.30
- Rows 164..200: Factor 0.70
- Rows 200..288: Factor 1.00

## Colour palette & visual rules

- **Palette**: every colour is named through `pixel_art.color_of()`. Nothing
  here is a hardcoded RGB tuple, which is what lets
  `python tools/validate_palette.py assets/bg_forest/<layer>.bmp --colorkey`
  come back clean. Gradients go through `dither_mix`, which only ever picks
  between two *named* colours and so cannot leave the set.
- **Flat uniform runs, and they are load-bearing.** A ground band boundary is a
  discontinuity in horizontal scroll offset, so it is invisible only where the
  rows either side of it are uniform across every column *and* the same colour.
  - Rows 158..170: flat `ground_far` - carries the cut at row 164.
  - Rows 196..204: flat `dirt_mid` - carries the cut at row 200.
  `boot_test` reads this directory's `08_ground` BMP and asserts exactly that.
- **The two layers that stand on the plane have their feet inside those runs**:
  `05_hills_midfar` (0.30) at art row 165, `02_hills_near` (0.70) at art row
  200. That is the generator's half of the contract with the band table, and
  nothing checks it - if a foot moves, move it to another flat run.
- **The sky reaches `sky_horizon` at row 150, the ground plane's own top edge.**
  It was authored to bottom out at row 200 and layer 07 disappeared: `mountain`
  is luminance 62 and the ramp passes through 55..63 across rows 90..156, which
  is exactly where the ridge sits. Anchoring the ramp to the scene's horizon is
  what makes the mountain body read against the sky behind it.
- **Depth grade**, body luminance, far to near: 62, 56, 50, 40, 29, 28, 25.
  Nearer is darker and more contrasty; further is closer to the sky.
- **Floor row**: the terrain floor is at art row 264, 16512 solid cells. The
  scene finds it with `spawn terrain`'s own scan, not from a constant.
- **Albedo map**: the finished composite, exactly as `composite_preview.png`
  shows it.
