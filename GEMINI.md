# GEMINI.md — Toop / Xoco (`SlopPhysics`)

Context for Google Antigravity and the Gemini CLI. C++20 / SDL2 falling-sand engine,
Windows x64, MSVC, CMake 3.14+. SDL2 2.30.0 is built statically via `FetchContent`.

## Repository map

| Path | Role | Constraint |
|---|---|---|
| `src/physics/` | Simulation: grid, player, tool | **Zero SDL. Zero float.** |
| `src/game/` | Run loop, camera, pacer, input log | `run.cpp`/`input_log.cpp` are engine sources |
| `src/render/` | Light field, anim, surface plane | SDL-free; headless-testable |
| `src/scene/` | BMP + text parsers → records | No SDL, no physics |
| `src/ui/`, `src/render/frame.cpp` | SDL draw calls only | `FRAME_SOURCES` |
| `src/main.cpp` | Window, event pump, blit | The only place SDL may leak in |
| `tests/` | 19 CTest suites + 6 probes | Plain C++, no framework |
| `tools/` | Python asset pipeline | Staged at build time |

`CMakeLists.txt` splits sources into five variables. **That split is the
architecture.** A file in the wrong variable silently links SDL into a headless
test. Read the comments above each `set()` before touching them.

## Hard rules (violating any of these is a defect, not a style choice)

- **RULE-SDL** — nothing under `src/physics/` includes, links, or names SDL.
- **RULE-FLOAT** — no `float`/`double` in `src/physics/`. Use `fx` (16.16 fixed
  point, `src/physics/fixed.h`). Determinism for replay depends on it.
- **RULE-ONEWAY** — render may read physics; physics may never read render.
  Lighting must never become a simulation input.
- **RULE-NOALLOC** — no allocation on any path reachable from `Grid::update()`.
  Scratch vectors are members, `.clear()`ed and reused. Never add a local container.
- **RULE-FAILHARD** — parsers reject the **whole file** on the first bad record and
  report via `std::string* error`. Callers log to `stderr` and continue degraded.
  Never add per-line recovery. Rationale: `src/scene/props.h`.
- **RULE-ASSETS** — bind sprites with `python tools/load_sprite.py`. Never hardcode
  a path. `assets/wip/` is excluded from staging on purpose.

## Anti-patterns — changes that compile, pass review, and are wrong

| Do not | Because |
|---|---|
| Split `Element::ticks` into separate fields | Alignment rounds 13 → 16 bytes; +8 MB at 1080p. A gas is never structural, which `element.h` static_asserts. |
| Convert `Element` to full SoA | `step_thermal` reads `type` **and** `temperature` over a 3×3 stencil — AoS costs 3 cache lines, SoA costs 6. SoA only helps single-field scans. |
| Replace `fx::trunc` with `>> 16` | `>>` floors; a left-moving body then drifts. Pinned by static_assert. |
| Narrow `mark_dirty` below 3×3 | Piles hang in mid-air; seams appear on chunk lines. |
| Make `updated_tag` a `bool` | Reintroduces a full-world reset pass every step. |
| Raise `MAX_SUPPORT_CELLS` "for correctness" | It guesses *supported* on purpose. A wrong collapse destroys a level; a missed one is invisible. |
| Suppress a failing `static_assert` | Those messages are the cross-file design review (`player.h:344-400`). |
| Edit `TUNING.md` rows by hand without running `tuning_test` | The test checks every linked row's line number and every `player.h` value; it is what keeps the doc from drifting again. |

## Commands

```bash
# configure / build
cmake -S . -B build
cmake --build build --config Release
cmake --build build --config Debug
cmake --build build --config Release --target grid_bench

# run
.\build\Release\SlopPhysics.exe          # F9 writes session.rec for replay

# test — 19 suites
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build -C Release -R golden_frame_test --output-on-failure

# measure — not part of the suite; slow machines must not fail the build
.\build\Release\grid_bench.exe
.\build\Release\preview_light.exe && python tools/rawpng.py out.raw out.png 804 604
```

## Required verification sequence

Run in this order. Do not report success having skipped a stage.

```
0. CLEANUP     just code_cleanup
               -> clang-format on changed lines, clang-tidy on changed C++
               files, ruff on changed Python, then build + ctest. Never
               reformat whole files: the tree predates .clang-format.

1. UNIT        ctest --test-dir build -C Release --output-on-failure
               -> 19/19. No exceptions.

2. VISUAL      golden_frame_test must pass with its checksum UNCHANGED.
               It composites a real frame, including a lit fire, through the
               real draw calls. A changed checksum = changed visible output.
               Re-baseline only with an explicit, stated reason.

3. PROFILE     .\build\Release\grid_bench.exe, before AND after, back to back,
               same sitting, same machine. Report both. Budget rows at
               1920x1080: `churning`, `cascading`, `light/fire`.
```

Physics change → all three. Render change → 2 and 3. Feel constant → all three
plus a `TUNING.md` row and `History` entry in the same commit.

## Tuning constants

```cpp
static constexpr fx::v MOVE_SPEED = fx::from_ratio(225, 2);  // 112.5 cells/s
static constexpr fx::v GRAVITY    = fx::from_int(500);       // 500 cells/s^2
```
Exact rationals only. `fx::v(112.5f * 65536)` is forbidden — compile-time float
folding varies by platform. Velocities are cells/second; `fx::per_step()` converts.
One cell = `Camera::DEFAULT_SCALE` (4) screen pixels. `steps` always means a fixed
60 Hz simulation step, never a rendered frame.

## Performance baseline (measured, 1920×1080, Release)

```
churning    50.2 ms/step  301%   <- vent_fluid is 25.5 ms of this (ablation-proven)
cascading   41.1 ms/step  247%
burning      8.0 ms/step   48%
collapsing   2.7 ms/step   16%
light/fire   4.3 ms/frame  26%   <- was 15.2; propagate sweep threaded
light/dark   0.6 ms/frame   3%
settled      0.0005 ms/step       <- chunk sleeping works; keep it working
```

Never claim an optimization without back-to-back `grid_bench` output.

## Determinism contract

Replay = seed + input log. `Run::step()` at a fixed 60 Hz, `fx` integer math, and
`sim_random` streams hashed from `(world_seed, step, index, stream_tag)`. Anything
that changes evaluation order, tie-breaking, or RNG stream usage **changes replays
and invalidates `session.rec`** — say so explicitly in the change description.

Note: the world checksum in `input_log.cpp` mixes `type`, `color`, `updated_tag`,
`ticks` — it does **not** cover `temperature` or `piece_tag`. Divergence in those
two fields will not be caught by a replay check.

## Style

Comments carry the reasoning, in full, at the point of use — this codebase argues
with itself in prose and that is deliberate. Match the density. `/W4 /permissive-`
is on and the tree builds with zero warnings; do not add warnings. Tests are plain C++
returning non-zero — no framework. `*_probe.exe` assert nothing by design.
