# CLAUDE.md — Toop / Xoco (`SlopPhysics`)

C++20 / SDL2 falling-sand engine. Windows x64, MSVC, CMake 3.14+. SDL2 2.30.0 is
fetched and built statically by `FetchContent` — do not vendor or system-install it.

## Non-negotiable invariants

1. **`src/physics/` never sees SDL.** Not a header, not a type, not a `#include`.
   Enforced by the link/include graph in `CMakeLists.txt`: `grid_test` and friends
   do not link `SDL2-static`, so they never get its include dirs, so `SDL.h` fails
   to resolve. If you find yourself adding a source file to `ENGINE_SOURCES` to fix
   a build, you have crossed the firewall — stop and reconsider.
2. **No float in `src/physics/`.** Floats are not reproducible across compilers or
   `/fp` modes, and replay determinism depends on every simulation number being
   reproducible. Use `fx` (signed 16.16, `src/physics/fixed.h`). `fx::to_float` is
   render/test only — its comment says so; honour it.
3. **Rendering never feeds simulation.** `light.cpp`, `player_anim.cpp`,
   `surface_plane.cpp` read the grid `const` and write only their own buffers. The
   `#include` graph runs one way: render → physics, never the reverse.
4. **The step loop allocates nothing.** `Run::step()` / `Grid::update()` reuse
   persistent scratch vectors cleared with `.clear()`, never freed. Do not add a
   local `std::vector`, `std::string`, or `std::function` to any function reachable
   from `Grid::update()`.
5. **Parsers reject the file, never the line.** `load_prop_list`, `load_scene_list`,
   `load_sprite_manifest` return an empty vector and set `*error` on the first
   malformed record. Callers print to `stderr` and continue degraded (no props /
   built-in scene) — loud, not fatal. **Never add per-line skip-and-continue
   recovery**; `src/scene/props.h` has the argument. A scene that renders wrong and
   says nothing is the failure mode being designed out.
6. **Assets stage at build time.** `assets/` is copied next to the exe by a
   POST_BUILD step, minus `assets/wip/`. Bind and stage sprites with
   `python tools/load_sprite.py <key> <file.bmp>` — never hardcode an asset path.

## Design rationales that look like cruft — do not "clean up"

- **`Element::ticks` is a union of three meanings** (fall clock / fire fuel / steam
  countdown). Safe because a gas is never structural, which
  `element.h`'s `lifetimes_are_never_structural()` static_asserts over every row.
  Splitting it costs 4 bytes/cell (alignment rounds 13 → 16) = ~8 MB at 1080p.
- **`updated_tag` stores a frame tag, not a bool** — so there is no per-step reset
  pass over a mostly-sleeping world.
- **`mark_dirty` wakes the full 3×3 per cell**, resolved per-cell so it crosses
  chunk borders. Narrowing it reintroduces floating piles and chunk-line seams.
- **`fx::trunc` truncates toward zero and is not `>>`.** Flooring drifts a
  left-moving body. Pinned by static_assert in `fixed.h`.
- **`is_grounded` compares `piece_tag`, not material** — a crack is stored as a
  disagreement between two cells so it survives the piece moving.
- **`MAX_SUPPORT_CELLS` guesses "supported" when it gives up.** A missed collapse is
  invisible; a wrong collapse is rubble. Keep erring that way.
- **Cross-file invariants are `static_assert`s, not comments** (`player.h:344-400`,
  `element.h:97-153`, `reaction.h`). If you retune a constant and the build breaks
  with a prose message, that message is the review — read it, do not suppress it.

## Command matrix

```bash
cmake -S . -B build                                  # configure (VS generator)
cmake --build build --config Release                 # build all + stage assets
cmake --build build --config Debug
cmake --build build --config Release --target grid_bench   # single target

.\build\Release\SlopPhysics.exe                      # run (F9 writes session.rec)

ctest --test-dir build -C Release --output-on-failure           # 18 suites
ctest --test-dir build -C Release -R grid_test --output-on-failure

.\build\Release\grid_bench.exe                       # timings; NOT a test
.\build\Release\burn_probe.exe                       # taste probes, assert nothing
.\build\Release\water_probe.exe
.\build\Release\preview_light.exe                    # headless frame dump
python tools/rawpng.py out.raw out.png 804 604
```

## Verification workflow — mandatory

**Any change under `src/physics/`:**
1. `ctest ... --output-on-failure` — all 18 must pass. `grid_test`, `player_test`,
   `collapse_test`, `run_test` are the ones that bite.
2. `golden_frame_test` must pass **unchanged**. It composites a real frame including
   a lit fire and checksums it. A changed checksum means you altered visible
   output — prove that was intended before re-baselining.
3. `grid_bench.exe` **before and after, back to back in one sitting**. Report both
   columns. `churning` and `cascading` at 1920×1080 are the budget rows.

**Any change under `src/render/`:** steps 2 and 3, plus the `light/fire` and
`light/dark` rows of `grid_bench`.

**Any change to feel constants:** update `TUNING.md` in the same commit — value,
correct line number, and a `History` entry saying why.

> ⚠ **`TUNING.md` is currently out of sync with the source.** It documents
> `WALL_SLIDE_SPEED`, `COYOTE_STEPS`, `JUMP_BUFFER_STEPS`, `FLAP_FALL_CANCEL`,
> `GLIDE_GRAVITY` and `CRUSH_PERCENT`, **none of which exist**; `MAX_STEP_HEIGHT` is
> 3 not 2; `BURN_DAMAGE`/`BURN_INTERVAL_STEPS` are 2/6 not 5/10; every `player.h`
> line number is stale. **Read `src/physics/player.h` for ground truth. Never
> implement a constant because `TUNING.md` names it.**

## Writing tuning constants

Exact rationals only — no float literals, no float folding:
```cpp
static constexpr fx::v MOVE_SPEED = fx::from_ratio(225, 2);  // 112.5 cells/s
static constexpr fx::v GRAVITY    = fx::from_int(500);
```
Units: velocity is **cells/second**; `fx::per_step()` converts. One cell is
`Camera::DEFAULT_SCALE` (4) screen pixels — the constant is `DEFAULT_SCALE`, not
`SCALE`, because a scene may override it. Anything in `steps` is a fixed 60 Hz
simulation step, never a rendered frame.

## Known hot spots (measured, `grid_bench`, 1920×1080)

| Row | Cost | Note |
|---|---|---|
| `churning` | 50.2 ms/step (301%) | `vent_fluid` alone is 25.5 ms of it |
| `cascading` | 41.1 ms/step (247%) | |
| `light/fire` | 15.2 ms/frame (91%) | 88% of it is the 22-iteration propagate sweep |
| `burning` | 8.0 ms/step (48%) | |

Do not claim a perf win without back-to-back `grid_bench` output. The comment at
`main.cpp:1566` calling the light field "too cheap to measure" is wrong — ignore it.

## House style

- Comments explain **why**, at length, at the point of use. Match that density.
- `/W4` is clean today except six `C4996` (`fopen`/`sscanf`). Do not add warnings.
- No test framework: suites are plain C++ returning non-zero. Keep it that way.
- Probes (`*_probe.exe`) assert nothing by design — they report taste. Do not
  promote one to `add_test()`.
