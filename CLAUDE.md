# CLAUDE.md — Toop / Xoco (`SlopPhysics`)

C++20 / SDL2 falling-sand engine. Windows x64, MSVC, CMake 3.14+. SDL2 2.30.0 is
fetched and built statically by `FetchContent`; don't vendor or system-install it.

## Invariants

These hold across the whole tree. Each exists for a reason given alongside it; if a
task seems to require breaking one, stop and say so rather than working around it.

- **`src/physics/` has no SDL** — no header, type, or include. The test targets
  don't link `SDL2-static`, so they don't get its include dirs; that is what keeps
  physics headless-testable. Moving a file into `ENGINE_SOURCES` to fix a build
  means the boundary has been crossed.
- **`src/physics/` has no float.** Replay determinism needs every simulation number
  to be reproducible across compilers and `/fp` modes. Use `fx` (signed 16.16,
  `src/physics/fixed.h`); `fx::to_float` is for render and tests only.
- **Rendering never feeds simulation.** `light.cpp` and `player_anim.cpp`
  read the grid `const` and write only their own buffers.
  Includes run render → physics, never the reverse.
- **The step loop allocates nothing.** `Run::step()` / `Grid::update()` reuse
  persistent scratch vectors cleared with `.clear()`. No local `std::vector`,
  `std::string`, or `std::function` in anything reachable from `Grid::update()`.
- **Parsers reject the whole file, never a single line.** `load_prop_list`,
  `load_level`, `backdrop_set::load`, `load_scene_list`, and
  `load_sprite_manifest` return empty and set `*error` on
  the first malformed record; callers log to `stderr` and continue degraded. Don't
  add per-line skip-and-continue — a scene that renders wrong silently is the
  failure this design prevents (`src/scene/props.h` has the full argument).
- **Assets stage at build time.** A POST_BUILD step copies `assets/` (minus
  `assets/wip/`) next to the exe. Bind sprites with
  `python tools/load_sprite.py <key> <file.bmp>`; don't hardcode asset paths.

## Intentional designs that look like cruft

Leave these as they are; each has a rationale comment at the definition.

- `Element::ticks` is a union of three meanings (fall clock / fire fuel / steam
  countdown), guarded by `lifetimes_are_never_structural()` in `element.h`.
  Splitting it grows every cell.
- `updated_tag` is a frame tag, not a bool, so there's no per-step reset pass.
- `mark_dirty` wakes the full 3×3 per cell across chunk borders. Narrowing it
  brings back floating piles and chunk seams.
- `fx::trunc` truncates toward zero and isn't `>>`; flooring drifts left-moving
  bodies.
- `is_grounded` compares `piece_tag`, not material, so cracks survive movement.
- An enemy's arm is geometry (pixels outside the box's columns) except where the
  art names arm letters (`body_art::Art::arm`), which only the fish does. Its
  lure arm grows out of the top of its head, where no column rule fits. Keep
  geometry for any body whose arms do hang beside the box.
- `MAX_SUPPORT_CELLS` assumes "supported" when it gives up — a missed collapse is
  invisible, a wrong one is rubble.
- Cross-file invariants are `static_assert`s (`player.h`, `element.h`,
  `reaction.h`). If a retuned constant trips one, its message explains the
  constraint — address it rather than loosening the assert.

## Commands

```bash
cmake -S . -B build                                   # configure (VS generator)
cmake --build build --config Release                  # build all + stage assets
cmake --build build --config Release --target grid_bench

ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build -C Release -R grid_test --output-on-failure

.\build\Release\SlopPhysics.exe                       # F9 writes session.rec
.\build\Release\grid_bench.exe                        # timings, not a test
.\build\Release\preview_light.exe                     # headless frame dump
python tools/rawpng.py out.raw out.png 804 604

just code_cleanup                                     # pre-PR pass, see below
just code_check                                       # same, changes nothing
just asan                                             # ASan build in build-asan\, all suites
```

`*_probe.exe` targets report numbers for judging feel and assert nothing; don't
turn them into `add_test()`.

## Verifying changes

- **Before opening or updating any PR:** `just code_cleanup` (needs `just`, LLVM's
  clang-format/clang-tidy, and `ruff`). It runs `tools/code_cleanup.py`:
  clang-format on the changed lines, clang-tidy on the changed C++ files, ruff on
  the changed Python files, then the Release build and ctest. Formatting is
  limited to changed lines because the tree predates `.clang-format`, and a
  whole-file reformat in a feature branch buries the real change and conflicts
  with every other open branch — don't run `clang-format -i` over whole files. Fix
  a tidy finding, or silence it at that line with `// NOLINT(check-name)` and a
  comment saying why; never edit `.clang-tidy` to make one change pass.
- **`src/physics/`:** run the full ctest suite. `golden_frame_test` must pass
  unchanged — it checksums a composited frame, so a new checksum means visible
  output changed; confirm that was intended before re-baselining. Run `grid_bench`
  before and after in the same sitting and report both; `churning` and `cascading`
  at 1920×1080 are the rows that matter.
- **`src/render/`:** `golden_frame_test` plus before/after `grid_bench`, including
  the `light/fire` and `light/dark` rows.
- **Perf claims** need back-to-back `grid_bench` numbers; run-to-run noise on the
  heavy rows is about ±10%.
- **Feel constants:** update `TUNING.md` in the same commit (value, the file that
  declares it, and a `History` entry with the reason). Rows link files, never
  lines; `tuning_test` finds each constant by name and rejects `#L` anchors.

## Code conventions

- Tuning constants are exact rationals, never float literals:
  `fx::from_ratio(225, 2)`, `fx::from_int(500)`.
- Velocity is cells/second; convert with `fx::per_step()`. A "step" is a fixed
  60 Hz simulation step, not a rendered frame. One cell is
  `Camera::DEFAULT_SCALE` screen pixels (scenes may override the scale).
- Comments explain *why*, at the point of use, and are fairly long — match the
  surrounding density.
- `/W4 /permissive-` with zero warnings; keep it that way.
- Tests are plain C++ executables returning non-zero on failure — no framework.

## Responses

- End every response with a short **TL;DR**.
- If any files were edited, list them after the TL;DR. If none were, say so.
