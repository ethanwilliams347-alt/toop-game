# Background 1 Multi-Layer Test Set

Native resolution: 344 x 144 px (Exact 1:1 match to Cast n Chill native scale)

## Layer Order & Depth Breakdown (Front to Back)

| File | Layer Index | Name / Description | Recommended Parallax | Transparency |
| :--- | :---: | :--- | :---: | :--- |
| 01_foreground_rocks.png | 1 | Foreground Rocks (in front of player) | 1.00x - 1.20x | Alpha / ColorKey |
| 02_hills_near.png | 2 | Rock/Hill mass (nearest behind player) | 0.70x | Alpha / ColorKey |
| 03_hills_midnear.png | 3 | Rock/Hill mass | 0.55x | Alpha / ColorKey |
| 04_hills_mid.png | 4 | Rock/Hill mass | 0.42x | Alpha / ColorKey |
| 05_hills_midfar.png | 5 | Rock/Hill mass | 0.30x | Alpha / ColorKey |
| 06_hills_far.png | 6 | Rock/Hill mass (farthest hill ridge) | 0.20x | Alpha / ColorKey |
| 07_distant_mountains.png | 7 | Distant Mountains / Silhouette Peak | 0.12x | Alpha / ColorKey |
| 08_ground_plane.png | 8 | Ground / Water Plane | 0.28x - 0.52x (Ramp) | Receding Plane |
| 09_sky.png | 9 | Sky & Atmosphere (farthest back) | 0.04x | Fully Opaque |

## Conversion Command

To convert any layer into a 24-bit engine BMP with magenta transparency:
powershell
python tools/png_to_bmp.py art_src/Background_1/09_sky.png assets/backdrop_sky.bmp
python tools/png_to_bmp.py art_src/Background_1/07_distant_mountains.png assets/backdrop_mountains.bmp

