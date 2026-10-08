# Architecture

A bird's-eye view of the codebase: what each part is for, which way the
dependencies point, and how one frame moves through them. It names files and
types rather than linking lines, so symbol search finds them and the document
does not go stale when a function moves.

The rules the layering exists to protect -- no SDL and no float under
`src/physics/`, rendering never feeding the simulation, nothing allocated in the
step loop, whole-file parser rejection -- are stated once, with their reasons, in
[CLAUDE.md](CLAUDE.md) under *Invariants*. This file shows where those lines fall;
it does not restate them. The reasoning behind individual designs is in the
header comment of the file that holds them.

## Bird's-eye view

The game is a falling-sand world: a grid of cells, each holding one material,
updated by a cellular automaton at a fixed 60 Hz step. The player, enemies and
arrows are not cells -- they are bodies outside the grid that ask it where they
may go and, through a small set of verbs, change it.

Everything that decides what happens is deterministic and headless. A run is a
pure function of a seed, a starting world and one `Input` per step, which is
what lets a recorded session replay exactly and lets almost every decision be
checked by a plain C++ test with no window. SDL is confined to a thin shell at
the edge: the window, the event pump, textures and draw calls.

```
                 main.cpp  (frame loop, SDL event switch, wiring)
                    |
     +--------------+---------------+-----------------+
     |              |               |                 |
   shell/        render/         present/           game/
  (SDL: targets, (frame.cpp,     (const Run ->      (Run, boot, level,
   textures,     overlay.cpp:    sprite lists,      pacer, camera,
   cell upload)  SDL draws;      enemy atlas,       debug, menus,
                 light, anim,    HUD text)          input log)
                 backdrops:          |                 |
                 SDL-free)           +------+----------+
                     |                      |
                     +---- reads const ---> physics/   <--- scene/
                                           (Grid, Player,   (scene list, BMPs,
                                            Enemy, Arrow,    levels, props,
                                            tools, fx)       sprite manifest)
```

Arrows point from includer to included. Nothing under `src/physics/` includes
anything outside it.

## Code map

### `src/physics/` -- the simulation

The world and everything in it, in `fx` (16.16 fixed point, `fixed.h`). No SDL,
no float.

- `grid.h` / `grid.cpp` -- `Grid`, the cellular automaton: falling, flowing,
  heat, reactions, structural support and collapse. Work is culled by 64x64
  chunks that sleep when nothing in them moves. Every cell write goes through
  `set_element` / `swap_elements`.
- `topple.cpp` -- the one part of `Grid` that is not cellular: unsupported
  structures pivot about their last supporting edge as bodies with an angle.
- `element.h`, `material.h`, `reaction.h` -- what a cell holds, the material
  table (`MATERIALS`), and the ordered reaction table (`REACTIONS`).
- `box_body.h` -- axis-aligned box collision against the grid, shared by the
  player and every enemy.
- `player.h` / `player.cpp` -- `Player`: movement, health. Holds the grid
  `const`; it never writes a cell.
- `tool.h` / `tool.cpp` -- the player's verbs (dig, bow). The only player-side
  code that writes to the grid.
- `enemy.h` / `enemy.cpp`, `rig.h` -- enemies and the skeleton their pixels
  hang on. `species::GHOUL`, `species::TROLL`, `species::FISH`.
- `body_art.h`, `enemy_art.h`, `troll_art.h`, `fish_art.h` -- enemy bodies as
  character grids. They live here, not in `assets/`, because for an enemy the
  picture *is* the body: which pixels exist decides where an arrow hits. An arm
  is geometry (outside the box's columns) unless the art names arm letters, which
  only the fish does: its lure arm grows out of the top of its head.
- `arrow.h` / `arrow.cpp` -- arrows, as points outside the grid.
- `random.h` -- stateless randomness: a value is a function of position, step
  and seed, so a run reproduces from the seed alone.
- `fixed.h`, `fixed_trig.h`, `int_math.h` -- the integer arithmetic everything
  above uses.

### `src/game/` -- the run and the decisions around it

SDL-free. `run`, `level` and `input_log` build with the simulation; the rest are
header-only.

- `run.h` / `run.cpp` -- `Run`: one playthrough. Owns the `Grid`, `Player`,
  enemies, quiver and dig tool; `Run::step(const Input&)` advances one fixed
  step. `Input` is plain bools and a cursor -- no SDL key codes -- and carries
  one-shot `Command`s so every change to the world is in the recorded stream.
- `events.h` -- what happened during a step (kills, hits, slams, digs), for the
  HUD, screen shake and sound to react to without side channels.
- `level.h` / `level.cpp` -- `level::start`: everything between choosing a scene
  and its first step.
- `boot.h`, `scene_activation.h` -- the decisions `main()` takes before its first
  frame and when switching scene, pulled out so a test can reach them.
- `pacer.h` -- the fixed-step pacer: how many steps a wall-clock frame buys and
  the interpolation alpha. It is told the elapsed time; it never reads a clock.
- `camera.h`, `display.h` -- world-cell to screen-pixel mapping, and the window
  modes.
- `debug_view.h`, `settings_menu.h` -- pause, single-step, free camera and cell
  inspector; the settings menu as a state machine.
- `input_log.h` / `input_log.cpp` -- the `SLOPREC` session recording (seed,
  starting world, one `Input` per step). F9 writes one; `grid_bench` replays it.

### `src/scene/` -- authored files into records

Parsers for what is in `assets/`. No SDL. Each rejects a malformed file whole.

- `scene_list.h` -- `assets/scenes.txt`: which scenes exist and what each names.
- `bmp.h` -- reads a scene's material and albedo BMPs without SDL; `legend.h` is
  the frozen colour -> material table those maps are authored in.
- `scene.h` -- `Scene` and `load_scene`, which stamps one into a `Grid`.
- `level_list.h` -- a level file: player start, objective, enemy placements.
- `level_files.h` -- loads everything a scene row names, one function shared by
  the game and the replay bench.
- `props.h`, `sprites.h` -- non-simulated props and the sprite manifest
  (`assets/sprites.txt`).

### `src/present/` -- simulation state into frame records

`present.h` is the one place allowed to read both a `const Run&` and the
frame's record types. It paints the enemy atlas, builds the enemy and arrow
sprite lists, and writes the HUD text. No SDL calls.

### `src/render/` -- drawing

Split by whether it touches SDL:

- **SDL-free** (testable headlessly): `light.h` (the emissive light field),
  `player_anim.h` (which sheet frame to draw), `backdrop_set.h` (parsing
  `backdrop.txt`), `backdrop_wrap.h` and `depth_rig.h` (parallax arithmetic).
- **SDL draw calls**: `frame.h` / `frame.cpp` -- `frame::compose`, the world
  layers in draw order; `overlay.h` / `overlay.cpp` -- the screen-space layer
  (reticle, HUD, hotbar, run-over wash, settings screen), drawn after the world.
- `player_sprite.h` is generated by `tools/player_sheet.py`; don't edit it.

### `src/ui/` -- hand-authored UI art

`text.h` (a 3x5 bitmap font) and `hotbar.h` (the eight number-key materials and
their icons), both as ASCII art drawn straight to `SDL_Renderer`.

### `src/shell/` -- the SDL side

`shell.h`: render targets and display-mode switching, loading BMPs as textures,
backdrop textures, and uploading the visible grid to the cell texture. Only the
game executable links it.

### `src/main.cpp` -- the loop and the wiring

Creates the window, owns the state the SDL event switch dispatches to (menu,
debug keys, recorder, scene index), and runs the frame loop. Its header comment
lists what has been moved out of it and where.

## One frame

1. **Events.** `SDL_PollEvent` feeds the event switch in `main.cpp`, which
   updates the menu and debug state and fills an `Input`.
2. **Pace.** `pacer::Pacer` turns the elapsed seconds into a number of fixed
   steps; `pacer::world_advances` freezes the world while paused, in settings or
   after the run ends.
3. **Step.** `Run::step(input)` runs once per step, in this order: commands and
   brush, `Grid::update()`, player, dig tool, bow and arrows, enemies, then the
   win/loss check. Each step emits `events.h` records.
4. **Interpolate.** The player's drawn position is blended between the last two
   steps by the pacer's alpha; the camera follows it.
5. **Present.** `present::paint_enemies`, `present::arrows` and
   `present::run_readout` read the `Run` const and fill buffers and sprite lists.
6. **Draw.** `shell::upload_cells` uploads the visible grid. `frame::compose`
   walks the ordered `frame::LAYERS` table in `frame.cpp` -- backdrop, props,
   cells, objective, enemies, player, arrows, foreground, then the grade and the
   light pass. `overlay::draw` draws the UI on top, then `SDL_RenderPresent`.

Steps 4-6 read the `Run` const. In the frame loop, only `Run::step` changes the
world.

## Build targets

`CMakeLists.txt` groups sources into sets that mirror the layers above. A
target only gets the sets it links, so crossing a boundary is a build error
rather than a habit.

| Set | Contents | SDL |
|---|---|---|
| `ENGINE_SOURCES` | `physics/*.cpp`, `game/run`, `game/level`, `game/input_log`, `scene/scene` | no |
| `RENDER_SOURCES` | `render/light`, `render/player_anim`, `render/backdrop_set` | no |
| `SCENE_PROP_SOURCES` | the rest of `scene/` | no |
| `PRESENT_SOURCES` | `present/present` | no |
| `FRAME_SOURCES` | `render/frame`, `render/overlay`, `ui/*` | yes |
| `SHELL_SOURCES` | `shell/shell` | yes |

`SlopPhysics` (the game) links all of them plus SDL2. Every test links the
fewest sets it can; see the comment on each target in `CMakeLists.txt`.

## Tests and tools

- `tests/test_*.cpp` -- one executable per suite, registered with ctest, returning
  non-zero on failure (no framework; shared helpers in `test_util.h`).
  `golden_frame_test` checksums a composited frame and is the guard on visible
  output. `tuning_test` checks [TUNING.md](TUNING.md) against the constants it
  names.
- `tests/bench_grid.cpp` -- `grid_bench`, timings including a replayed recorded
  session. Not a test.
- `tests/*_probe.cpp`, `tests/preview_*.cpp` -- instruments that print numbers or
  dump frames for judging by eye; they assert nothing.
- `tools/` -- Python for assets (backdrop and prop generators, palette
  snapping and validation, player sheet building, `load_sprite.py` for binding a
  sprite) plus `code_cleanup.py`, the pre-PR pass behind `just code_cleanup`.

## Assets

`assets/` is copied next to the exe at build time (minus `assets/wip/`).
`scenes.txt` lists the scenes; each names a material BMP, an albedo BMP, and
optionally a backdrop directory (`bg_*/backdrop.txt`), a level file and a prop
list. `sprites.txt` maps sprite keys to BMPs. Source art lives in `art_src/`.
The formats are documented at the top of each file and in the parser that reads
it.
