# The valley (bg_valley): authored nine-layer source set

Native resolution: 344 x 144 px (Exact 1:1 match to Cast n Chill native scale)

## Layer Order & Depth Breakdown (Front to Back)

| File | Layer Index | Name / Description | Recommended Parallax | Transparency |
| :--- | :---: | :--- | :---: | :--- |
| bg_valley_01_fg_rocks.png | 1 | Foreground Rocks (in front of player) | 1.00x - 1.20x | Alpha / ColorKey |
| bg_valley_02_hills_near.png | 2 | Rock/Hill mass (nearest behind player) | 0.70x | Alpha / ColorKey |
| bg_valley_03_hills_midnear.png | 3 | Rock/Hill mass | 0.55x | Alpha / ColorKey |
| bg_valley_04_hills_mid.png | 4 | Rock/Hill mass | 0.42x | Alpha / ColorKey |
| bg_valley_05_hills_midfar.png | 5 | Rock/Hill mass | 0.30x | Alpha / ColorKey |
| bg_valley_06_hills_far.png | 6 | Rock/Hill mass (farthest hill ridge) | 0.20x | Alpha / ColorKey |
| bg_valley_07_mountains.png | 7 | Distant Mountains / Silhouette Peak | 0.12x | Alpha / ColorKey |
| bg_valley_08_ground.png | 8 | Ground / Water Plane | 0.28x - 0.52x (Ramp) | Receding Plane |
| bg_valley_09_sky.png | 9 | Sky & Atmosphere (farthest back) | 0.04x | Fully Opaque |

## Conversion Command

To convert any layer into a 24-bit engine BMP with magenta transparency:
powershell
python tools/png_to_bmp.py art_src/bg_valley/bg_valley_09_sky.png assets/bg_valley/bg_valley_09_sky.bmp
python tools/png_to_bmp.py art_src/bg_valley/bg_valley_07_mountains.png assets/bg_valley/bg_valley_07_mountains.bmp

