# Architecture

Map of the codebase: what each directory is for, which way includes may point,
where to start for a given task, and how one frame flows. Paths are given in
full from the repo root so they can be opened or grepped directly. Symbols are
named, never line numbers, so this file survives code moving.

The invariants (no SDL / no float under `src/physics/`, render never feeds
simulation, no allocation in the step loop, whole-file parser rejection, assets
staged at build) and their reasons live in [CLAUDE.md](CLAUDE.md) under
*Invariants*; this file shows where those lines fall and doesn't restate why.
The rationale for an individual design is in the header comment of the file
that holds it -- read that before changing the design.

## Model in one paragraph

A falling-sand world: a grid of cells, one material each, advanced by a
cellular automaton at a fixed 60 Hz step. The player, enemies and arrows are
*not* cells -- they are bodies outside the grid that query it and change it only
through a few verbs (`Grid::set_element`, `swap_elements`, `displace`, the dig
tool). A run is a pure function of (seed, starting world, one `Input` per step),
so a recorded session replays exactly and nearly every decision is testable by a
headless C++ executable. SDL is a thin shell at the edge: window, event pump,
textures, draw calls.

## Where to start

| Task | Start in | Also touch / check |
|---|---|---|
| Cell behaviour (falling, flow, heat, burning) | `src/physics/grid.cpp`, `src/physics/material.h` | `grid_test`, `golden_frame_test`, `grid_bench --focus` |
| New material | `src/physics/material.h` (`MATERIALS`), `src/physics/element.h` | `src/scene/legend.h` if it can be painted in a scene BMP; `src/ui/hotbar.h` if it's on the hotbar |
| Reaction between materials | `src/physics/reaction.h` (`REACTIONS`, ordered) | `static_assert`s in `reaction.h` |
| Liquids (water, oil) | `src/physics/grid.cpp` (`step_liquid`, `transfer_liquid`, `level_liquid`, `disperse_liquid`) | see *Liquids* below; `tests/fluid_probe.cpp` |
| Structural collapse / toppling | `src/physics/grid.cpp` (support), `src/physics/topple.cpp` | `collapse_test` |
| Player movement or feel | `src/physics/player.h` / `.cpp` | `TUNING.md` in the same commit; `player_test`, `tuning_test` |
| Dig or bow | `src/physics/tool.h` / `.cpp`, `src/physics/arrow.*` | `tool_test` |
| Enemy behaviour | `src/physics/enemy.h` / `.cpp` (`species::GHOUL/TROLL/FISH`), `rig.h` | `enemy_test` |
| Enemy body shape / hitbox | `src/physics/*_art.h`, `body_art.h` | the art *is* the hitbox |
| Per-step order, commands, win/loss | `src/game/run.cpp` (`Run::step`) | `src/game/events.h`; `run_test`, `alloc_test` |
| Scene start-up | `src/game/level.cpp` (`level::start`), `src/game/boot.h`, `scene_activation.h` | `level_test`, `boot_test` |
| Recording / replay | `src/game/input_log.*` | `grid_bench` replays `session.rec` |
| Camera, window modes | `src/game/camera.h`, `src/game/display.h` | `camera_test` |
| Debug keys, settings menu | `src/game/debug_view.h`, `src/game/settings_menu.h`, `src/main.cpp` (event switch) | `debug_test` |
| Asset file format | the parser in `src/scene/` (or `src/render/backdrop_set.*`), `src/util/text_record.h` | the file's own header in `assets/`; `*_list_test`, `props_test`, `sprites_test`, `backdrop_set_test` |
| New scene | `assets/scenes.txt`, a level file, BMPs | `scene_test`, `scene_list_test` |
| Bind a sprite | `python tools/load_sprite.py <key> <file.bmp>` | never hardcode asset paths |
| What appears on screen from game state | `src/present/present.*` | `present_test` |
| Draw order, layers, grade, light pass | `src/render/frame.cpp` (`frame::LAYERS`) | `golden_frame_test` |
| Lighting | `src/render/light.*` | `light_test`, `preview_light.exe`, `grid_bench --focus` light rows |
| Player animation | `src/render/player_anim.*` | `src/render/player_sprite.h` is generated -- regenerate with `python tools/player_sheet.py --header` |
| HUD, reticle, hotbar, menu drawing | `src/render/overlay.*`, `src/ui/*` | `golden_frame_test` |
| Parallax backdrops | `src/render/backdrop_set.*`, `backdrop_wrap.h`, `depth_rig.h`; generators in `tools/generate_bg_*.py` | `backdrop_test`, `backdrop_set_test`, `rig_test` |
| Textures, render targets, cell upload | `src/shell/shell.*` | `shell_test` |
| Frame loop, wiring | `src/main.cpp` (its header comment lists what moved out and where) | |
| Build / which target links what | `CMakeLists.txt` (each set has a comment explaining its boundary) | `sdl_boundary_test` |

## Layers and allowed includes

```
                 src/main.cpp  (frame loop, SDL event switch, wiring)
                    |
     +--------------+---------------+-----------------+
     |              |               |                 |
   shell/        render/         present/           game/
  (SDL)        frame,overlay:SDL  (const Run ->      (Run, boot, level,
               light,anim,         records; no       pacer, camera,
               backdrops: no SDL   SDL calls)        debug, menus, log)
                     |                 |                 |
                     +-- reads const --+-------+---------+
                                               v
                                           physics/   <--- scene/
                                                           (parsers for assets/)
           util/ (header-only, std only) is below scene/ and render/backdrop_set
```

Arrows point from includer to included.

| Directory | May include | Must not include | SDL |
|---|---|---|---|
| `src/physics/` | itself, std | anything else in `src/` | never |
| `src/scene/` | `physics/`, `util/` | `render/`, `game/`, `present/`, `shell/` | never |
| `src/util/` | std | everything in `src/` | never (and not includable from `physics/`: it uses float) |
| `src/game/` | `physics/`, `scene/` | `render/` record types, `present/`, `shell/` | no |
| `src/present/` | `game/`, `physics/`, `render/frame.h` record types | `shell/` | headers only (via `frame.h`), no calls |
| `src/render/` light, player_anim, backdrop_set, backdrop_wrap, depth_rig | `physics/` (read const), `util/`, each other | `game/run.h`, `present/`, `shell/` | never |
| `src/render/` frame, overlay; `src/ui/` | the above, `game/camera.h`, `game/display.h`, `physics/material.h` (hotbar) | `game/run.h`, `present/`, `shell/` | yes |
| `src/shell/`, `src/main.cpp` | anything | | yes |

## Code map

### `src/physics/` -- the simulation (no SDL, no float; `fx` 16.16 from `fixed.h`)

- `grid.h` / `grid.cpp` -- `Grid`: falling, flowing, heat, reactions, structural
  support and collapse. Culled by 64x64 chunks that sleep when nothing moves.
  Cell writes go through `set_element` / `swap_elements` (`displace` resolves to
  `set_element`).
- `topple.cpp` -- the non-cellular part of `Grid`: unsupported structures pivot
  about their last supporting edge as rigid bodies with an angle.
- `element.h` (what a cell holds), `material.h` (`MATERIALS`), `reaction.h`
  (ordered `REACTIONS`).
- `box_body.h` -- AABB-vs-grid collision shared by player and enemies.
- `player.h` / `.cpp` -- `Player`: movement, health. Holds the grid `const`;
  never writes a cell.
- `tool.h` / `.cpp` -- the player's verbs (dig, bow); the only player-side code
  that writes the grid.
- `enemy.h` / `.cpp`, `rig.h` -- enemies and the skeleton their pixels hang on.
- `body_art.h`, `enemy_art.h`, `troll_art.h`, `fish_art.h` -- enemy bodies as
  character grids. Here, not in `assets/`, because the picture *is* the body:
  which pixels exist decides where an arrow hits. `troll_art.h` and
  `fish_art.h` are generated by `tools/troll_art.py` and `tools/fish_art.py`
  (`--header`); don't edit them. The art is checked at compile time (every pixel
  connected, every row the right width, one `static_assert` per rule over
  `species::ALL`), which is why MSVC builds with a raised `/constexpr:steps`.
- `arrow.h` / `.cpp` -- arrows, as points outside the grid.
- `random.h` -- stateless randomness: f(position, step, seed).
- `fixed.h`, `fixed_trig.h`, `int_math.h` -- integer arithmetic for all of the above.

### `src/game/` -- the run (SDL-free; `run`, `level`, `input_log` are .cpp, the rest header-only)

- `run.h` / `.cpp` -- `Run`: one playthrough; owns `Grid`, `Player`, enemies,
  quiver, dig tool. `Run::step(const Input&)` advances one step. `Input` is plain
  bools + cursor (no SDL keycodes) and carries one-shot `Command`s, so every
  world change is in the recorded stream.
- `events.h` -- what happened in a step (kills, hits, slams, digs) for HUD,
  shake and sound to react to.
- `level.h` / `.cpp` -- `level::start`: everything between choosing a scene and
  its first step.
- `boot.h`, `scene_activation.h` -- decisions `main()` makes before the first
  frame and on scene switch, extracted for testing.
- `pacer.h` -- fixed-step pacer: steps per wall-clock frame and interpolation
  alpha. Told the elapsed time; never reads a clock.
- `camera.h` (cell -> screen pixel), `display.h` (window modes).
- `debug_view.h` (pause, single-step, free camera, cell inspector),
  `settings_menu.h` (state machine).
- `input_log.h` / `.cpp` -- `SLOPREC` recording: seed, starting world, one
  `Input` per step. F9 writes `session.rec`.

### `src/scene/` -- authored files into records (no SDL; each parser rejects a malformed file whole)

- `scene_list.*` -- `assets/scenes.txt`: five positional fields, then named
  (`mode=`, `size=WxH`, `scale=`, `level=`, `backdrop=`).
- `bmp.*` -- reads material and albedo BMPs without SDL; `legend.h` is the
  frozen colour -> material table they're authored in.
- `scene.*` -- `Scene`, `load_scene` (stamps into a `Grid`).
- `level_list.*` -- level file: player start, objective, enemy placements.
- `level_files.*` -- loads everything a scene row names; shared by game and
  replay bench.
- `props.*` (non-simulated props), `sprites.*` (`assets/sprites.txt` manifest).

### `src/util/`

- `text_record.h` -- shared reading for every line-per-record file: `#`
  comments, the name check that keeps paths inside `assets/`, strict number
  readers (whole field or error), the `path:line: why` error format. Used by
  `scene/` and `render/backdrop_set`. Header-only, in no source set.

### `src/present/`

- `present.h` / `.cpp` -- the one place that reads both `const Run&` and the
  frame record types: paints the enemy atlas, builds enemy/arrow sprite lists,
  writes HUD text. No SDL calls.

### `src/render/`

- SDL-free: `light.*` (emissive light field), `player_anim.*` (which sheet
  frame), `backdrop_set.*` (parses `backdrop.txt`), `backdrop_wrap.h`,
  `depth_rig.h` (parallax arithmetic).
- SDL: `frame.*` -- `frame::compose`, walks `frame::LAYERS` in draw order;
  `overlay.*` -- screen-space layer (reticle, HUD, hotbar, run-over wash,
  settings screen) drawn after the world.
- `player_sprite.h` -- generated; don't edit.

### `src/ui/`, `src/shell/`, `src/main.cpp`

- `ui/text.*` (3x5 bitmap font), `ui/hotbar.*` (the eight number-key materials
  and icons): ASCII art drawn to `SDL_Renderer`.
- `shell/shell.*` -- render targets, display-mode switching, BMP textures,
  backdrop textures, visible-grid upload. Game executable only.
- `main.cpp` -- creates the window, owns the state the event switch dispatches
  to (menu, debug keys, recorder, scene index), runs the frame loop.

## One frame

1. **Events** -- `SDL_PollEvent` -> event switch in `main.cpp` -> menu/debug
   state and an `Input`.
2. **Pace** -- `pacer::Pacer` turns elapsed seconds into N fixed steps;
   `pacer::world_advances` freezes the world while paused, in settings, or after
   the run ends.
3. **Step** (xN) -- `Run::step(input)`: commands, brush, `Grid::update()`,
   player, dig tool, bow and arrows, enemies, win/loss check. Emits `events.h`
   records.
4. **Interpolate** -- player draw position blended by the pacer's alpha; camera
   follows.
5. **Present** -- `present::paint_enemies`, `present::arrows`,
   `present::run_readout` read the `Run` const.
6. **Draw** -- `shell::upload_cells`; `frame::compose` walks `frame::LAYERS`
   (backdrop, props, cells, objective, enemies, player, arrows, foreground,
   grade, light pass); `overlay::draw`; `SDL_RenderPresent`.

Only step 3 changes the world; 4-6 read the `Run` const.

## Liquids

Water and oil are the one part of the grid that doesn't move as whole cells.

- **State:** `Element::mass` (amount; `Grid::LIQUID_FULL` = 4096 is a full
  cell) and `Element::flow` (sideways velocity).
- **Conservation:** liquid moves only by amounts passing between two cells
  (`Grid::transfer_liquid`). A cell left with none becomes Empty; Empty that
  receives some becomes liquid. Moves between *unlike* materials (water through
  oil, a grain through water) are still whole-cell swaps.
- **`Grid::step_liquid` order:**
  1. Down -- as much as the cell below holds.
  2. Sideways -- a share (material's `spread`) of the difference to each lower
     side, plus momentum along `flow`. `flow` is pushed by the side difference
     and damped each step, so a resting pool has none and sleeps; it's what
     makes a dam break slosh instead of slump.
  3. Up -- whatever exceeds full. Rows sweep bottom to top, so pressed water
     surfaces in one sweep.
  4. Level -- only if 1-3 moved nothing: `Grid::level_liquid` searches the body
     breadth-first for a lower surface with room and sends half the height
     difference. This is pressure; it levels a U-tube through full cells.
- **Grains into liquid:** `Grid::disperse_liquid` first pushes the cell's amount
  into same-or-lower liquid within three cells, up to 2x full; only the rest
  rides up with the swap. That's why pouring sand raises a pool instead of
  lifting its water up the stream. Steam carries the amount across boiling and
  condensing.
- **Incompressible on purpose:** a full cell holds `LIQUID_FULL` regardless of
  depth. A compressible model was tried and dropped: U-tubes need it stiff,
  deep bodies need it soft, and no value served both (at 3%/cell a U-tube took
  ~1000 steps and a 339-deep lake settled to 118). Constants and reasons: the
  liquid section of `src/physics/grid.h`.
- **Rendering:** a part-full cell draws translucent (`Grid::pixel_of`).

## Build targets

Source sets in `CMakeLists.txt` mirror the layers; a target gets only the sets
it links, so crossing a boundary is a build error. Shared sets are OBJECT
libraries, and each target names every library it uses (CMake doesn't pass
object code through another library, so a missing one is a link error on that
target).

| Set | Library | Contents | SDL |
|---|---|---|---|
| `ENGINE_SOURCES` | `slop_engine` | `physics/*.cpp`, `game/run`, `game/level`, `game/input_log`, `scene/scene` | no -- links nothing |
| `RENDER_SOURCES` | `slop_render` | `render/light`, `render/player_anim` (need engine) | no |
| `BACKDROP_SOURCES` | `slop_backdrop` | `render/backdrop_set` (no engine) | no |
| `SCENE_PROP_SOURCES` | `slop_scene_props` | rest of `scene/` | no |
| `PRESENT_SOURCES` | `slop_present` | `present/present` (needs engine) | include path only |
| `FRAME_SOURCES` | `slop_frame` | `render/frame`, `render/overlay`, `ui/*` | yes |
| `SHELL_SOURCES` | -- (game only) | `shell/shell` | yes |

`SlopPhysics` links all of them plus SDL2. Each test links the fewest sets it
can (see its comment in `CMakeLists.txt`). `sdl_boundary_test` runs the
compiler on a one-line probe with `slop_engine`'s include path and fails if
`SDL.h` is found.

## Tests and tools

- `tests/test_<name>.cpp` -> ctest target `<name>_test` (exception:
  `test_depth_rig.cpp` -> `rig_test`). Plain executables returning non-zero on
  failure; shared helpers in `test_util.h`, `liquid.h`, `world_match.h`.
  Suites that read `assets/` run with the repo root as working directory.
  Suites keep each `Run` on the heap, so the ASan build (`just asan`) needs no
  raised stack.
- Guards worth knowing: `golden_frame_test` (checksum of a composited frame --
  any visible change), `alloc_test` (allocations in a replayed step),
  `tuning_test` (`TUNING.md` vs. the constants it names), `sdl_boundary_test`.
- `tests/bench_grid.cpp` -> `grid_bench`: timings incl. a replayed session. Not
  a test.
- `tests/*_probe.cpp`, `tests/preview_*.cpp`: print numbers or dump frames for
  judging by eye; assert nothing. Don't make them `add_test()`.
- `tools/*.py`: asset pipeline -- backdrop generators (`generate_bg_*.py`),
  props (`generate_props.py`), fixture scene (`generate_test_scene.py`), palette
  (`snap_to_palette.py`, `validate_palette.py`), player sheet
  (`player_sheet.py`), troll and fish art (`troll_art.py`, `fish_art.py`),
  sprite binding (`load_sprite.py`). `pixel_art.py` is
  their shared base: palette, BMP/PNG codecs, periodic primitives (`wsin`,
  `wnoise`, `rnd`), and `write_layered_backdrop`. `code_cleanup.py` is the
  pre-PR pass behind `just code_cleanup`.

## Assets

`assets/` is copied next to the exe at build time (minus `assets/wip/`); source
art is in `art_src/`. `assets/scenes.txt` lists scenes (material BMP, albedo
BMP, optional prop list, then `mode=`, `size=`, `scale=`, `backdrop=` a
`bg_*/backdrop.txt` directory, `level=`). `assets/sprites.txt` maps sprite keys
to BMPs. Each format is documented at the top of its file and in its parser.
