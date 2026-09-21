# Background Gemini (bg_gemini) Multi-Layer Extended Set

Native resolution: 688 x 288 px (New generative pixel art extension of Background_1 / bg1)

## Layer Order & Depth Breakdown (Back to Front)

| File | Layer Index | Name / Description | Parallax X | Parallax Y | Transparency |
| :--- | :---: | :--- | :---: | :---: | :--- |
| bg_gemini_09_sky.bmp | 9 | Sky & Atmosphere | 0.04x | 1.00x | Fully Opaque |
| bg_gemini_08_ground.bmp | 8 | Ground / Water Plane | Banded | 1.00x | Magenta Key |
| bg_gemini_07_mountains.bmp | 7 | Distant Mountains | 0.12x | 1.00x | Magenta Key |
| bg_gemini_06_hills_far.bmp | 6 | Far Hills | 0.20x | 1.00x | Magenta Key |
| bg_gemini_05_hills_midfar.bmp | 5 | Mid-Far Hills | 0.30x | 1.00x | Magenta Key |
| bg_gemini_04_hills_mid.bmp | 4 | Mid Hills | 0.42x | 1.00x | Magenta Key |
| bg_gemini_03_hills_midnear.bmp | 3 | Mid-Near Hills | 0.55x | 1.00x | Magenta Key |
| bg_gemini_02_hills_near.bmp | 2 | Near Hills | 0.70x | 1.00x | Magenta Key |
| bg_gemini_01_fg_rocks.bmp | 1 | Foreground Rocks (in front of player) | 1.00x | 1.00x | Magenta Key (Foreground) |

## Color Palette & Visual Rules

- **Palette**: Exact RGB colors measured from `assets/bg1/`.
- **Composition**: Authoring of extended geological features (towering mountain massif on left with serrated aretes, crisp pyramid summits on far hills, shark-fin twin summits on midfar hills, stepped rock terraces framing open panoramic lake).
- **Ground Banding**:
  * Band 0 (rows 0..164): 0.30x parallax factor.
  * Band 1 (rows 164..200): 0.70x parallax factor.
  * Band 2 (rows 200..288): 1.00x parallax factor.
  * Rows 156..172: 100% uniform water color `(19, 52, 49)`.
  * Rows 196..204: 100% uniform mid-ground color `(31, 42, 12)`.
  * Rows 214..287: 100% uniform near-ground color `(12, 39, 20)`.
- **Material Map**: Terrain floor at row 264 with Wall material `0x888888`, air above as Empty `0x000000`.
- **Albedo Map**: Fully composed scene matching game presentation.
