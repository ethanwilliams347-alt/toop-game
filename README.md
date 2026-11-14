# Toop / Xoco (working title)

A pixel-art destructible physics engine (cellular automata) built from scratch
in C++20 and SDL2.

## Build and run

Needs [CMake](https://cmake.org/download/) 3.14+ and MSVC. SDL2 is fetched
automatically.

```bash
cmake -B build -S .
cmake --build build --config Release     # also copies assets/ next to the exe
.\build\Release\SlopPhysics.exe
```

## Tests and benchmark

```bash
ctest --test-dir build -C Release --output-on-failure
.\build\Release\grid_bench.exe
```

- There are 28 test suites, one per concern: one per `tests/test_*.cpp`, plus
  `sdl_boundary_test`, which checks that the simulation never sees SDL
  (`ctest -N` lists them). All are headless except `gl_frame_test`, which needs
  an OpenGL 3.3 context and reports itself skipped without one.
  `golden_frame_test` checksums a rendered frame, so a changed checksum means
  the visible output changed.
- The benchmark is run by hand, not in CI. For an A/B comparison use
  `grid_bench --focus`, compare timings taken back to back in one sitting, and
  trust them only when the `placement:` line says `on`: without pinning to the
  fast cores, runs can differ by nearly 2x. Its replayed row needs a
  `session.rec`; without one it prints "not run". Recordings are not actively
  maintained (see below).

## Recordings

`F9` saves the session so far to `session.rec`, which `grid_bench` replays.
Recordings are not actively maintained: no change is checked against
replaying a saved session. Before relying on recordings again, record a session,
replay it, and confirm the replay matches.

## Manual check

The suites test each mechanic alone. After changes to `src/physics/`,
`src/game/` or `main.cpp`, play through this list to check they still work
together.

| # | Check | Expected |
|---|---|---|
| 1 | Launch | The game opens on the `empty` scene. `F7` moves to the next scene, whose terrain is visible at once, and the console's `Scene: …, N cells placed` line shows a large `N`. |
| 2 | Movement | Walking, jumping and landing are flush with the ground, walls block, and one-cell steps need no jump. |
| 3 | Flight | Holding jump in the air beats the wings in countable, laboured strokes, not a hover. |
| 4 | Digging | Left-click carves a hole, and undercutting sand makes the pile above it fall in. |
| 5 | Materials | Sand slopes, water levels out, oil floats, and steam rises. |
| 6 | Fire | Fire runs along wood and burns out, water beside fire makes steam, and steam drips back as water. |
| 7 | Settling | Left alone, the HUD's awake-chunk count returns to 0. |
| 8 | Structures | An unsupported wall or wood shape falls as one piece, and a supported one never drifts. |
| 9 | Depth | Background layers scroll at different rates with no seams. |
| 10 | The run | Falls and fire hurt, death shows `YOU DIED`, `R` restarts, and reaching the marker at the east end of a terrain scene shows `OBJECTIVE REACHED`. |
| 11 | Enemies | Shot limbs fall off as sand, and a head shot collapses the body. |
| 12 | Stability | Minutes of all of the above together run with no crash or slowdown. |
| 13 | Display | Each display mode in the settings menu applies, `Enter` keeps it, and a change left alone goes back after 10 seconds. |

## Controls

**Player**
| Key | Action |
|---|---|
| `A` / `D`, `←` / `→` | Walk left and right. |
| `Space`, `W`, `↑` | Jump; hold it in the air to fly. |
| Left-click | Dig the terrain under the cursor, within reach (the crosshair dims when the target is out of reach). |
| `E` (hold) | Shoot arrows at the cursor. |
| `R` | Start a new run once the current one is over. |
| `ESC` | Open the settings menu (quit is inside it). |

**Settings menu** (see [Settings](#settings) below)
| Key | Action |
|---|---|
| `↑` / `↓`, `W` / `S` | Move between rows. |
| `←` / `→`, `A` / `D` | Change the row's value. Nothing changes until you apply it. |
| `Enter`, `Space` | Apply every changed value. With nothing changed, close the menu. On `QUIT`, quit. |
| `ESC` | Close the menu and resume, dropping changes not yet applied. |
| `Enter` / `ESC` after a display change | Keep the new mode, or go back to the old one at once. |

**Dev tools: world**
| Key | Action |
|---|---|
| Right-click (hold) | Paint the selected material at the cursor. |
| Mouse wheel | Grow or shrink the brush. |
| `1` – `8` | Select Wood, Wall, Sand, Water, Oil, Steam, Fire, Eraser (shown on the hotbar). |
| `T` | Spawn a ghoul standing on the cursor. |
| `Y` | Spawn a troll standing on the cursor. |
| `U` | Spawn a fish standing on the cursor. |
| `I` | Spawn an angler standing on the cursor. |

**Dev tools: debug**
| Key | Action |
|---|---|
| `P` | Pause and resume the simulation. |
| `.` | Advance one step while paused (hold to scrub; the brush and dig act on that step). |
| `F` | Detach the camera and pan it with the movement keys (`Shift` for fast); press again to reattach. |
| `Ctrl`+`R` | Reset the world on the same seed, mid-run included. |
| `F6` | Switch enemy animation between snapped key poses (default) and smooth turning. |
| `F7` | Cycle to the next scene in `assets/scenes.txt`. |
| `F8` | Toggle the cell inspector for the cell under the cursor. |
| `F9` | Save the session so far to `session.rec` (later saves go to `session_N.rec`). |

**Dev tools: OpenGL renderer only** (`renderer=gl`; with the SDL renderer these keys do nothing)
| Key | Action |
|---|---|
| `F3` | Reload the scene's mood file (`assets/<set>/<set>_mood.txt` next to the exe). One that doesn't parse keeps the current mood and prints why. With `F8`'s inspector on, a `GPU MS` line shows what each lit-frame pass costs. |
| `F5` | Recompile the shaders from the `assets/shaders/` copy next to the exe. A shader that doesn't compile keeps the old one running and prints why. |
| `F10` | Switch the cells between the normal look and the material view, where each cell is flat-coloured by what it is. |
| `F11` | Turn the scene's mood lighting off and on, to compare with the plain look. |
| `F12` | Save the current frame next to the exe as `frame_NNN.bmp`. |

Holding a key down doesn't repeat it, except for the keys that are meant to be
held (movement, `E`, the mouse buttons), `.` (which steps once per repeat, to
scrub) and the settings menu's arrow keys. A held `F7` changes scene once, and a
held `F9` writes one recording.

## Settings

`ESC` opens the settings menu over the frozen game. Its rows are:

- **Display:** `WINDOWED`, `BORDERLESS` (a borderless window covering the
  screen at the desktop's own resolution; the usual choice for playing
  fullscreen, including on an ultrawide), or `FULLSCREEN` (exclusive, at the
  chosen resolution and that resolution's highest refresh rate).
- **Resolution:** windowed, a common size that fits on the screen with the
  window's title bar and the taskbar: 1280x720, 1366x768, 1600x900, 1920x1080,
  2560x1080, 2560x1440, 3440x1440 or 3840x2160. Fullscreen, the resolutions the
  monitor reports, from 1280x720 up. Borderless, always the desktop's size. The
  row also shows how many cells the size shows in the current scene.
- **VSync:** on or off.
- **Renderer:** `SDL` or `OPENGL`. This one takes effect the next time the game
  starts (the row says `ON RESTART` until then).
- **Apply** and **Quit.**

A cell is always the scene's scale in screen pixels (4 by default, 8 or 10 in
the painted scenes), so a bigger window shows more of the world rather than
bigger cells. Sizes are physical pixels: a 3440x1440 window is 3440x1440 pixels
even when Windows is scaled to 125% or 150%.

After a change of display mode or resolution the menu asks you to keep it. Press
`Enter` to keep it, or `ESC` to go back. If you do nothing, it goes back after 10
seconds, so a mode the monitor can't show undoes itself. Other changes are saved
as soon as they are applied.

The choices are saved to `settings.txt` next to the exe:

```
display=borderless
resolution=3440x1440
vsync=on
renderer=gl
```

Any key left out keeps its default (windowed, the largest window that fits,
VSync on, the OpenGL renderer, which falls back to SDL if
the driver can't run it), and lines the game doesn't recognise are kept when
it rewrites the file. An older file holding only `resolution=` still loads, as
a window of that size. A stored window that no longer fits opens at the largest
one that does, and one exactly the size of the desktop opens borderless. A
fullscreen resolution the monitor no longer has opens at the desktop's. For one
run, `--renderer=gl` or `--renderer=sdl` on the command line overrides
`renderer=` without saving.

## Gameplay

- **Health:** 100. Fire or burning wood hurts on contact, and so does landing
  too fast: a drop of about three body heights is free, and a landing at
  terminal velocity costs 80. Flapping slows a fall, and the drop at spawn is
  free.
- **Objective:** the gold-and-white marker near the east end of each terrain
  scene, at the column its level file names (`objective`), or an eighth of the
  world in from the east edge if it names none. You reach it by getting within
  6 cells of it (`Run::OBJECTIVE_REACH`). The HUD's `GOAL:` shows the distance
  and direction. The `empty` scene has no objective.
- **End of a run:** you win by reaching the marker and lose at 0 health. The
  world freezes either way, and `R` restarts.
- **HUD:** health, goal, FPS, material, brush size, and awake chunks (`+FALLING`
  while a structural piece is in the air), plus `FOES:` and `KILLS:`.

## Enemies

Every enemy pixel is a cell of body. An arrow removes the pixels around where it
lands, anything cut off from the chest drops as sand, and fire burns them away
where it touches. An enemy dies when it loses both eyes, its chest, both legs, or
more than 60% of its body. The limbs bend at their joints rather than switching
between drawn frames, so arrows hit the limb wherever it is drawn.

No scene places enemies at the moment (`PLANT_ENEMIES` is off in
`src/game/level.h`, so the `enemy` lines in level files are read but not used).
Spawn them at the cursor with `T`, `Y`, `U` and `I`.

- **Ghoul** (`T`): wanders, chases you when it sees you, and swipes for 8
  damage.
- **Troll** (`Y`): its eyes glow before it slams its club for 30 damage and
  leaves a crater. Its arms take two or three arrows to sever, and it needs an
  arrow in each eye.
- **Fish** (`U`): a bass on legs with a lure-arm on its head that whips down for
  20 damage. Two shots sever the arm, and the eye takes two.
- **Angler** (`I`, experimental): the fish on tentacles. Shooting a tentacle
  drops everything past the hole.

## Assets

`assets/` is copied next to the exe at build time, so a hand edit doesn't show
up until you rebuild or run `python tools/load_sprite.py --stage`. How files
under `assets/` are named and laid out is in [assets/README.md](assets/README.md).

- **Sprites:** run `python tools/load_sprite.py <key> <file>.bmp` to bind a BMP
  in `assets/sprites.txt`, check it against the frame grid, and stage it, with
  no rebuild needed. `--list` shows the current bindings.
- **Backdrops:** a folder of layer BMPs plus a `backdrop.txt`, chosen with
  `backdrop=<folder>` in `assets/scenes.txt`. The format is documented in
  `src/render/backdrop_set.h`.
- **Levels:** player column, objective and enemies (not placed for now; see
  [Enemies](#enemies)), chosen with `level=<file>`.
- **Player sheet:** the frame grid and animation table are in
  `tools/player_sheet.py`, which generates `src/render/player_sprite.h`.

## More

[ARCHITECTURE.md](ARCHITECTURE.md) maps the modules and how a frame moves
through them. [TUNING.md](TUNING.md) indexes the gameplay-feel constants. The
reasoning behind a value or a test is in the code comment next to it.
