# CLAUDE.md — Toop / Xoco (`SlopPhysics`)

C++20 / SDL2 falling-sand engine. Windows x64, MSVC, CMake 3.14+. SDL2 2.30.0 is
fetched and built statically by `FetchContent`; don't vendor or system-install it.

**Read [ARCHITECTURE.md](ARCHITECTURE.md) before starting large code task. It can be wrong though, so use it as just a starting point.**

## Commands

```bash
cmake -S . -B build                                   # configure (VS generator)
cmake --build build --config Release                  # build all + stage assets (parallel: Directory.Build.rsp)
cmake --build build --config Release --target grid_bench

ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build -C Release -R grid_test --output-on-failure

.\build\Release\SlopPhysics.exe                       # F9 writes session.rec
.\build\Release\grid_bench.exe                        # timings, not a test
.\build\Release\grid_bench.exe --focus                # the four A/B rows, 5 reps, ~4 min
.\build\Release\preview_light.exe out.raw 90          # headless frame dump (default file: preview.raw, 90 steps)
python tools/rawpng.py out.raw out.png 804 604

just code_cleanup                                     # pre-PR pass, see below
just code_check                                       # same, changes nothing
just asan                                             # ASan build in build-asan\, all suites
```

`*_probe.exe` targets report numbers for judging feel and assert nothing; don't
turn them into `add_test()`.

## Verifying changes

- **Before opening or updating any PR:** `just code_cleanup` (needs `just`, LLVM's
  clang-format/clang-tidy, `ruff`, and a configured `build/`; diffs against
  `origin/main`, else `main` — pass `--base <ref>` otherwise). It runs `tools/code_cleanup.py`:
  clang-format on the changed lines, clang-tidy on the changed C++ files, ruff on
  the changed Python files, then the Release build and ctest. Formatting is
  limited to changed lines because the tree predates `.clang-format`, and a
  whole-file reformat in a feature branch buries the real change and conflicts
  with every other open branch — don't run `clang-format -i` over whole files. Fix
  a tidy finding, or silence it at that line with `// NOLINT(check-name)` and a
  comment saying why; never edit `.clang-tidy` to make one change pass.
- **`src/physics/`:** run the full ctest suite. `golden_frame_test` must pass
  unchanged — it checksums a composited frame, so a new checksum means visible
  output changed; confirm that was intended before re-baselining. Run
  `grid_bench --focus` before and after in the same sitting and report both;
  `churning` and `cascading` at 1920×1080 are the rows that matter.
- **`src/render/`:** `golden_frame_test` plus before/after `grid_bench --focus`,
  which includes the `light/fire` and `light/dark` rows.
- **`src/ui/`, `render/overlay.cpp`:** these are drawn into `golden_frame_test`, so
  the same re-baselining rule applies.
- **Perf claims** need back-to-back `grid_bench --focus` runs: compare medians and
  quote the spread beside them. Check the `placement:` line first; if it doesn't
  say `on` (P-cores pinned, power throttling off), the numbers are not comparable
  — E-core drift alone nearly doubles `churning`. With it, one binary repeats to
  1–3%; close other heavy programs and don't build while it runs. Code layout
  alone moves rows by up to churning 2.5%, light/fire 3%, cascading 7%,
  light/dark 13% (`/GL`+`/LTCG` doesn't fix it). A difference inside its row's
  band is unproven until it survives rebuilding both sides with
  `-DSLOP_BENCH_PAD=16` (or `32`). Background is in `tests/bench_grid.cpp` and
  `CMakeLists.txt`.
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
