# Architecture

Map of the codebase: what each directory is for, which way includes may point,
where to start for a given task, and how one frame flows. Paths are given in
full from the repo root so they can be opened or grepped directly. Symbols are
named, never line numbers, so this file survives code moving.

The rules that hold across the whole tree, and their reasons, are under
*Invariants* below; the rest of this file shows where those lines fall.
The rationale for an individual design is in the header comment of the file
that holds it -- read that before changing the design.

## Invariants

These hold across the whole tree. Each exists for a reason given alongside it; if a
task seems to require breaking one, stop and say so rather than working around it.

- **The simulation has no SDL** — no header, type, or include. `slop_engine` (the
  OBJECT library built from `ENGINE_SOURCES`: all of `src/physics/` plus
  `game/run.cpp`, `game/level.cpp`, `game/input_log.cpp`, `scene/scene.cpp`) links
  nothing, so it never gets SDL's include dirs; that is what keeps the simulation
  headless-testable, and `sdl_boundary_test` fails if that ever changes. Linking
  SDL into `slop_engine`, or moving a file between the source sets in
  `CMakeLists.txt` to fix a build, means the boundary has been crossed.
- **`src/physics/` has no float.** Replay determinism needs every simulation number
  to be reproducible across compilers and `/fp` modes. Use `fx::v` (signed 16.16,
  `src/physics/fixed.h`); `fx::to_float` is for render and tests only.
- **Rendering never feeds simulation.** `light.cpp` and `player_anim.cpp`
  read the grid `const` and write only their own buffers.
  Includes run render → physics, never the reverse.
- **The step loop allocates nothing in steady state.** `Run::step()` /
  `Grid::update()` reuse persistent scratch vectors cleared with `.clear()`;
  those with a hard bound are reserved to it in the `Grid` constructor, and the
  few with none (the support queues, the body pool and its contacts) grow to a high-water mark
  and stay there. No local `std::vector`, `std::string`, or `std::function` in
  anything reachable from `Run::step()`. `alloc_test` counts allocations and
  fails if a replayed step makes any.
- **Parsers reject the whole file, never a single line.** `load_prop_list`,
  `load_level`, `backdrop_set::load`, `mood::load`, `load_scene_list`, and
  `load_sprite_manifest` return empty (a mood: `legacy()`) and set `*error` on
  the first malformed record; callers log to `stderr` and continue degraded. Don't
  add per-line skip-and-continue — a scene that renders wrong silently is the
  failure this design prevents (`src/scene/props.h` has the full argument).
- **Assets stage at build time.** A POST_BUILD step copies `assets/` (minus
  `assets/wip/`) next to the exe. Bind sprites with
  `python tools/load_sprite.py <key> <file.bmp>`; don't hardcode asset paths.

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
| New material | `src/physics/material.h` (`MATERIALS`, including its `weight`, `grip`, `bounce`), `src/physics/element.h` | `src/scene/legend.h` if it can be painted in a scene BMP; `src/ui/hotbar.h` if it's on the hotbar |
| Reaction between materials | `src/physics/reaction.h` (`REACTIONS`, ordered) | `static_assert`s in `reaction.h` |
| Liquids (water, oil) | `src/physics/grid.cpp` (`step_liquid`, `transfer_liquid`, `level_liquid`, `disperse_liquid`) | see *Liquids* below; `tests/fluid_probe.cpp` |
| Structural collapse, toppling, loose bodies | `src/physics/grid.cpp` (support), `src/physics/bodies.cpp` | `collapse_test`, `tests/body_probe.cpp`, `TUNING.md` |
| Player movement or feel | `src/physics/player.h` / `.cpp` | `TUNING.md` in the same commit; `player_test`, `tuning_test` |
| Dig or bow | `src/physics/tool.h` / `.cpp`, `src/physics/arrow.*` | `tool_test` |
| Enemy behaviour | `src/physics/enemy.h` / `.cpp` (`species::GHOUL/TROLL/FISH/ANGLER`), `rig.h`, `tentacle.h` | `enemy_test` |
| Enemy body shape / hitbox | `src/physics/*_art.h`, `body_art.h` | the art *is* the hitbox |
| Per-step order, commands, win/loss | `src/game/run.cpp` (`Run::step`) | `src/game/events.h`; `run_test`, `alloc_test` |
| Scene start-up | `src/game/level.cpp` (`level::start`), `src/game/boot.h`, `scene_activation.h` | `level_test`, `boot_test` |
| Recording / replay | `src/game/input_log.*` | `grid_bench` replays `session.rec` |
| Camera, window modes | `src/game/camera.h`, `src/game/display.h` | `camera_test` |
| Debug keys, settings menu | `src/game/debug_view.h`, `src/game/settings_menu.h`, `src/main.cpp` (event switch) | `debug_test` |
| Asset file format | the parser in `src/scene/` (or `src/render/backdrop_set.*`, `src/render/mood.*`), `src/util/text_record.h` | the file's own header in `assets/`; `*_list_test`, `props_test`, `sprites_test`, `backdrop_set_test`, `mood_test` |
| New scene | `assets/scenes.txt`, a level file, BMPs | `scene_test`, `scene_list_test` |
| Bind a sprite | `python tools/load_sprite.py <key> <file.bmp>` | never hardcode asset paths |
| What appears on screen from game state | `src/present/present.*` | `present_test` |
| Draw order, layers, grade, light pass | `src/render/frame.cpp` (`frame::LAYERS`) | `golden_frame_test` |
| Lighting | `src/render/light.*` | `light_test`, `preview_light.exe`, `grid_bench --focus` light rows |
| Player animation | `src/render/player_anim.*` | `src/render/player_sprite.h` is generated -- regenerate with `python tools/player_sheet.py --header` |
| HUD, reticle, hotbar, menu drawing | `src/render/overlay.*`, `src/ui/*` | `golden_frame_test` |
| Parallax backdrops | `src/render/backdrop_set.*`, `backdrop_wrap.h`, `depth_rig.h`; generators in `tools/generate_bg_*.py` | `backdrop_test`, `backdrop_set_test`, `rig_test` |
| Textures, render targets, cell upload | `src/shell/shell.*` | `shell_test` |
| OpenGL backend, shaders | `src/gpu/` (`gl_canvas.*`, loader list `gl_funcs.inl`), `assets/shaders/` | `gl_frame_test` (GL vs SDL within +-3 levels on 99.5% of pixels; skips without a GL context) |
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
| `src/render/` light, player_anim, cell_pack, hidden_cell, backdrop_set, backdrop_wrap, depth_rig, mood | `physics/` (read const), `util/`, each other | `game/run.h`, `present/`, `shell/` | never |
| `src/render/` frame, overlay; `src/ui/` | the above, `game/camera.h`, `game/display.h`, `physics/material.h` (hotbar) | `game/run.h`, `present/`, `shell/` | yes |
| `src/gpu/` | `render/canvas.h`, std | everything else in `src/` | yes (SDL's GL context and `SDL_opengl.h`; only `gl.h` includes the latter) |
| `src/shell/`, `src/main.cpp` | anything | | yes |

## Code map

### `src/physics/` -- the simulation (no SDL, no float; `fx` 16.16 from `fixed.h`)

- `grid.h` / `grid.cpp` -- `Grid`: falling, flowing, heat, reactions, structural
  support and collapse. Culled by 64x64 chunks that sleep when nothing moves.
  Cell writes go through `set_element` / `swap_elements` (`displace` resolves to
  `set_element`).
- `bodies.cpp` -- the non-cellular part of `Grid`: loose bodies. A piece that
  loses its support, topples, is knocked over or lands off balance becomes a
  rigid body (centre of mass, velocity, spin, mass and inertia from
  `Material::weight`), solved with sequential impulses, friction and
  restitution against contacts read off the grid, buoyancy and drag in liquid,
  and the weight of what rests on it. Bodies wake and push pieces at rest.
  Poses are Paeth shears, so cells are permuted, never resampled; the body lives
  in the grid throughout. Also the balance check, which weighs a piece with
  everything resting on it.
- `element.h` (what a cell holds), `material.h` (`MATERIALS`), `reaction.h`
  (ordered `REACTIONS`).
- `box_body.h` -- AABB-vs-grid collision shared by player and enemies.
- `player.h` / `.cpp` -- `Player`: movement, health. Holds the grid `const`;
  never writes a cell.
- `tool.h` / `.cpp` -- the player's verbs (dig, bow); the only player-side code
  that writes the grid.
- `enemy.h` / `.cpp`, `rig.h` -- enemies and the skeleton their pixels hang on.
- `tentacle.h` -- limbs that bend along their length: a chain of links, each a
  rig turn: a soft Verlet chain (`drift`) sprung toward its rest curl (`curl`),
  solved by FABRIK, with a travelling wave on top. A species opts in
  with `Species::tentacles`; each chain takes over one rig part. Also the
  second-order follower (`Response`, `follow`) the bulb and the body's lean
  and sink move on, and the swept test (`sweep_touches`) a lure strike hits by.
  Feet step from a planned home (`Enemy::plan_steps`), not on the stride, on a
  spring and with overlap; the body gathers speed (`Enemy::walk_speed`).
  After a sever (`Limb::intact`, `Limb::role`) the body hops on one leg or
  drags itself on its lure. The piece cut off stays in the body's own mask,
  posed by its own Verlet chain held in the same `Limb` (`Enemy::move_pieces`),
  so it is drawn and hit like any tentacle with no pool or render path of its
  own, and crumbles to sand when it times out or leaves the pad.
- `body_art.h`, `enemy_art.h`, `troll_art.h`, `fish_art.h`, `angler_art.h` --
  enemy bodies as character grids. Here, not in `assets/`, because the picture
  *is* the body: which pixels exist decides where an arrow hits. `troll_art.h`,
  `fish_art.h` and `angler_art.h` are generated by `tools/troll_art.py`,
  `tools/fish_art.py` and `tools/angler_art.py` (`--header`); don't edit them. The art is checked at compile time (every pixel
  connected, every row the right width, one `static_assert` per rule over
  `species::ALL`), which is why MSVC builds with a raised `/constexpr:steps`.
- `arrow.h` / `.cpp` -- arrows, as points outside the grid.
- `random.h` -- stateless randomness: f(position, step, seed).
- `fixed.h`, `fixed_trig.h`, `int_math.h` -- integer arithmetic for all of the above.

### `src/game/` -- the run (SDL-free; `run`, `level`, `input_log` are .cpp, the rest header-only)

- `run.h` / `.cpp` -- `Run`: one playthrough; owns `Grid`, `Player`, enemies
  (and the tentacled ones' `Enemy::Chains`, a smaller pool of their own, so
  `Run` is not copyable), quiver, dig tool. `Run::step(const Input&)` advances one step. `Input` is plain
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
  (`mode=`, `size=WxH`, `scale=`, `level=`, `backdrop=`, `mood=`).
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
  `scene/`, `render/backdrop_set` and `render/mood`. Header-only, in no source set.

### `src/present/`

- `present.h` / `.cpp` -- the one place that reads both `const Run&` and the
  frame record types: paints the enemy atlas, builds enemy/arrow sprite lists,
  writes HUD text. No SDL calls.

### `src/render/`

- SDL-free: `light.*` (emissive light field), `player_anim.*` (which sheet
  frame), `backdrop_set.*` (parses `backdrop.txt`), `backdrop_wrap.h`,
  `depth_rig.h` (parallax arithmetic), `cell_pack.*` (the visible window as
  one RGBA8UI texel per cell -- type, amount or ticks, temperature, flags --
  for a cell shader; reads `Grid::cell_data()` const and never
  `Element::color`; the bit layout a shader mirrors is in its header),
  `hidden_cell.h` (`frame::HiddenCell`, split out of `frame.h` so this layer
  can read it; `fill_type` is set by `present::paint_bodies`), `mood.*` (parses a scene's
  `*_mood.txt`: light model, ambient/sun, fog, bloom, grade LUT, vignette,
  grain, weather, point lights, per-material looks; `mood::legacy()` is
  today's look and what a scene with no `mood=` gets). The mood is loaded on
  scene switch in `main.cpp` and not yet drawn -- the planned GL passes
  consume it. Format: `src/render/mood.h`, summary in `assets/README.md`.
- `canvas.h` -- `gfx::Canvas`, the drawing seam (draw colour/blend, clear,
  fill_rect, blit, blit_ex, textures as `gfx::Texture` handles). No SDL.
  `blit_cells` is the cell layer's one draw: a plain blit by default (all
  `SdlCanvas` does), overridden by `GlCanvas` to draw through `cells.frag`.
  `sdl_canvas.*` -- `gfx::SdlCanvas`, the SDL_Renderer backend, and the only render
  file that calls SDL's renderer; golden_frame_test checksums it over a software
  renderer. The other backend is `gpu::GlCanvas` (below).
- Draw through `gfx::Canvas`: `frame.*` -- `frame::compose`, walks `frame::LAYERS`
  in draw order; `overlay.*` -- screen-space layer (reticle, HUD, hotbar, run-over
  wash, settings screen) drawn after the world.
- `player_sprite.h` -- generated; don't edit.

### `src/gpu/` -- the OpenGL 3.3 core backend

- `gl_canvas.*` -- `gpu::GlCanvas`, a `gfx::Canvas` that batches every fill and
  blit as quads (one shader, `assets/shaders/sprite.*`; fills sample a 1x1 white
  texture) into an offscreen framebuffer the window's size, then blits it to
  the window and swaps on `present()`. For now a parity backend: same blend
  equations as SDL, and destination rects truncated field by field the way
  SDL's software renderer does, so the frame matches `SdlCanvas`'s. Reads the
  window size at the first draw of each frame, so `shell::apply_mode` resizing
  the window is all a mode change needs. `reload_shaders()` (F5) keeps the old
  program on a failed compile; `read_pixels()` feeds `gl_frame_test` and F12.
- The cells (R1): `blit_cells` draws the cell texture with
  `assets/shaders/cells.frag` (on `sprite.vert`), sampling the colour texture
  on unit 0 and the material pack on unit 1 -- an RGBA8UI texture filled by
  `upload_cell_pack()` from `cell_pack::CellPack::texels()`, which `main.cpp`
  packs each frame only when `display.gl` is set (same window as
  `shell::upload_cells`, the colour still uploaded straight from
  `Grid::get_pixels()` with `GL_UNPACK_ROW_LENGTH`). `CellView` picks the
  shader's mode: `Legacy` (default) outputs the colour exactly as a blit would;
  `Material` (F10) flat-colours cells by type and flags; `Raw` writes the pack's
  bytes, which `gl_frame_test` reads back against `CellPack` byte for byte.
  `cells.frag` mirrors the bit layout in `render/cell_pack.h`.
- `gl.h` / `gl.cpp` / `gl_funcs.inl` -- the loader: an X-macro list of every
  entry point used, as `gpu::gl::Name` pointers filled by
  `SDL_GL_GetProcAddress`. No GLAD/GLEW, nothing links opengl32.lib. A function
  must be listed before it is called; a driver missing one fails creation.
- Selected by `renderer=gl|sdl` in `settings.txt` (default `sdl`) or
  `--renderer=gl|sdl`; `shell::open_display` falls back to SDL on any GL
  failure (no context, missing entry point, shader compile, incomplete FBO).

### `src/ui/`, `src/shell/`, `src/main.cpp`

- `ui/text.*` (3x5 bitmap font), `ui/hotbar.*` (the eight number-key materials
  and icons): ASCII art drawn to a `gfx::Canvas`.
- `shell/shell.*` -- opening the window with its backend (`open_display`, GL
  with SDL fallback), render targets, display-mode switching, BMP textures,
  backdrop textures, visible-grid upload, F12 frame BMPs. Game executable only.
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
5. **Present** -- `present::paint_bodies` (loose bodies, drawn as turned
   sprites; their cells go out as `frame::HiddenCell`s, each carrying the
   colour of the nearest liquid or powder within 2 cells so the space a turned
   sprite doesn't cover reads as the water around it instead of a gap; over its
   last `SETTLE_STEPS` a body eases onto the pose its cells will take),
   `present::paint_spray`, `present::paint_enemies`, `present::arrows`,
   `present::run_readout` read the `Run` const.
6. **Draw** -- `shell::upload_cells`; `frame::compose` walks `frame::LAYERS`
   (backdrop, props, cells, bodies, spray, objective, enemies, player, arrows, foreground,
   grade, light pass); `overlay::draw`; `canvas.present()`.

Only step 3 changes the world; 4-6 read the `Run` const.

## Liquids

Water and oil are the one part of the grid that doesn't move as whole cells.

- **State:** `Element::mass` (amount; `Grid::LIQUID_FULL` = 4096 is a full
  cell), `Element::flow` (sideways velocity) and, in `Element::ticks`, an
  upward velocity (`TickRole::Rise`). The upward velocity is zero in any pool at
  rest; only a body pushing water or spray landing sets it, so it costs a
  sleeping pool nothing.
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
- **Rise (`Grid::step_rise`):** a cell with upward velocity skips the rest of
  `step_liquid` while it climbs. Buried, it hands the velocity to the cell on
  top (pressure carries a push up the column in one sweep); at the surface it
  heaves part of itself into the empty cell above, under gravity, or past
  `SPRAY_MIN_RISE` throws a drop of spray.
- **Spray (`Grid::Spray`, `Grid::step_spray`):** drops fly as points in a
  fixed pool (`SPRAY_MAX`, reserved in the constructor) until they meet
  anything that is not empty, then go back into the grid as an amount. Stepped
  after the bodies and before the sweep. A world's liquid is the cells' plus
  `Grid::spray_mass()`.
- **Bodies push water (`Grid::tip_move`):** liquid in the cells a body enters
  goes out beside it with the body's momentum (`push_aside`): to the side
  cells, else around to the ends of its leading face and up past full cells
  there. Cells it leaves draw water in from beside them (`draw_in`). What has
  no room still goes into the cells it left, as before, so liquid is conserved.
  Below `SPLASH_MIN_SPEED` a body gives the water no motion.
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
| `RENDER_SOURCES` | `slop_render` | `render/light`, `render/player_anim`, `render/cell_pack` (need engine) | no |
| `BACKDROP_SOURCES` | `slop_backdrop` | `render/backdrop_set`, `render/mood` (no engine) | no |
| `SCENE_PROP_SOURCES` | `slop_scene_props` | rest of `scene/` | no |
| `PRESENT_SOURCES` | `slop_present` | `present/present` (needs engine) | include path only |
| `FRAME_SOURCES` | `slop_frame` | `render/frame`, `render/overlay`, `ui/*` | yes |
| `GPU_SOURCES` | `slop_gpu` | `gpu/gl`, `gpu/gl_canvas` | yes |
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
  any visible change), `gl_frame_test` (the same kind of frame through
  `GlCanvas` vs `SdlCanvas`, cells via `cells.frag`, plus a byte-exact
  readback of the uploaded cell pack; needs a GL 3.3 context, returns 77 =
  skipped without one), `alloc_test` (allocations in a replayed step),
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
`bg_*/backdrop.txt` directory, `level=`, `mood=` a `*_mood.txt` in that
directory). `assets/sprites.txt` maps sprite keys
to BMPs. Each format is documented at the top of its file and in its parser.
