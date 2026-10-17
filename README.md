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

- The 25 test suites are headless, one per concern (`tests/test_*.cpp`).
  `golden_frame_test` checksums a rendered frame, so a changed checksum means
  the visible output changed.
- The benchmark is run by hand, not in CI. Only compare timings taken back to
  back in one sitting, because runs on different occasions can differ by more
  than 2x. Its replayed row needs a `session.rec`; without one it prints
  "not run".

## Manual check

The suites test each mechanic alone. After changes to `src/physics/`,
`src/game/` or `main.cpp`, play through this list to check they still work
together.

| # | Check | Expected |
|---|---|---|
| 1 | Launch | Terrain is visible at once, and the console's `Scene: …, N cells placed` line shows a large `N`. |
| 2 | Movement | Walking, jumping and landing are flush with the ground, walls block, and one-cell steps need no jump. |
| 3 | Flight | Holding jump in the air beats the wings in countable, laboured strokes, not a hover. |
| 4 | Digging | Left-click carves a hole, and undercutting sand makes the pile above it fall in. |
| 5 | Materials | Sand slopes, water levels out, oil floats, and steam rises. |
| 6 | Fire | Fire runs along wood and burns out, water beside fire makes steam, and steam drips back as water. |
| 7 | Settling | Left alone, the HUD's awake-chunk count returns to 0. |
| 8 | Structures | An unsupported wall or wood shape falls as one piece, and a supported one never drifts. |
| 9 | Depth | Background layers scroll at different rates with no seams. |
| 10 | The run | Falls and fire hurt, death shows `YOU DIED`, `R` restarts, and flying east over the channel reaches the goal. |
| 11 | Enemies | Shot limbs fall off as sand, and a head shot collapses the body. |
| 12 | Stability | Minutes of all of the above together run with no crash or slowdown. |

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

**Settings menu**
| Key | Action |
|---|---|
| `↑` / `↓`, `W` / `S` | Move the cursor. |
| `Enter`, `Space` | Choose the highlighted item. |
| `ESC` | Close the menu and resume. |

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

**Recordings are not actively maintained.** No change is checked against
replaying a saved session. Before relying on recordings again, record a session,
replay it, and confirm the replay matches.

The settings menu offers 1920x1080, 2560x1440 and 3440x1440 at a fixed 4 screen
pixels per cell, so a wider window shows more of the world. The choice is saved
to `settings.txt` next to the exe, and a saved mode the display can't fit falls
back to the largest one that fits.

## Gameplay

- **Health:** 100. Fire or burning wood hurts on contact, and so does landing
  too fast: a drop of about three body heights is free, and a landing at
  terminal velocity costs 80. Flapping slows a fall, and the drop at spawn is
  free.
- **Objective:** the gold-and-white marker about 740 cells east, across a water
  channel you have to fly over. The HUD's `GOAL:` shows the distance and
  direction.
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

- **Ghoul:** placed around each scene. It wanders, chases you when it sees you,
  and swipes for 8 damage.
- **Troll:** one at the far end of each scene. Its eyes glow before it slams its
  club for 30 damage and leaves a crater. Its arms take two or three arrows to
  sever, and it needs an arrow in each eye.
- **Fish:** a bass on legs with a lure-arm on its head that whips down for 20
  damage. Two shots sever the arm, and the eye takes two. It isn't placed in any
  scene yet.
- **Angler** (experimental): the fish on tentacles. Shooting a tentacle drops
  everything past the hole. It isn't placed in any scene.

## Assets

`assets/` is copied next to the exe at build time, so a hand edit doesn't show
up until you rebuild or run `python tools/load_sprite.py --stage`.

- **Sprites:** run `python tools/load_sprite.py <key> <file>.bmp` to bind a BMP
  in `assets/sprites.txt`, check it against the frame grid, and stage it, with
  no rebuild needed. `--list` shows the current bindings.
- **Backdrops:** a folder of layer BMPs plus a `backdrop.txt`, chosen with
  `backdrop=<folder>` in `assets/scenes.txt`. The format is documented in
  `src/render/backdrop_set.h`.
- **Levels:** player column, objective and enemies, chosen with `level=<file>`.
- **Player sheet:** the frame grid and animation table are in
  `tools/player_sheet.py`, which generates `src/render/player_sprite.h`.

## More

[ARCHITECTURE.md](ARCHITECTURE.md) maps the modules and how a frame moves
through them. [TUNING.md](TUNING.md) indexes the gameplay-feel constants. The
reasoning behind a value or a test is in the code comment next to it.
