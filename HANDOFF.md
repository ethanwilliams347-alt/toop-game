# Handoff: act on the SlopPhysics code review

You are picking up work in `C:\Users\Ethan\Desktop\game\toop-share` (Toop / Xoco,
`SlopPhysics` — C++20/SDL2 falling-sand engine). A prior review session profiled the
engine, ran four experimental patches, and restored the tree. Your job is to land the
fixes. `CLAUDE.md` is in the repo root and will be auto-loaded — it has the
invariants, command matrix and verification workflow. Do not re-derive them.

## Critical environment facts — read before your first edit

- **This is NOT a git repository.** There is no undo. Back up every file to your
  scratchpad before editing it, and verify with `diff` after any restore.
- `build/` is already configured (VS 18 2026 generator) and Release-built.
  `ctest --test-dir build -C Release` is **18/18 green right now**. Keep it that way.
- Verified baseline on this machine (20 logical cores, MSVC `/O2` Release). Re-run
  `.\build\Release\grid_bench.exe` first to confirm you reproduce it before changing
  anything — if you don't, stop and say so:

```
churning  (1920x1080)  50.2 ms/step  301%      cascading  41.1 ms/step  247%
burning                 8.0 ms/step   48%      collapsing  2.7 ms/step   16%
light/fire             15.2 ms/frame  91%      light/dark  1.24 ms/frame  7%
settled              0.0005 ms/step    0%
```

- `golden_frame_test` checksums a composited frame **including a lit fire**
  (`tests/test_golden_frame.cpp:189-195`), so it genuinely covers the light
  propagation path. An unchanged checksum is real evidence.
- Line numbers below are from the review at handoff time. **Verify each location by
  reading it before editing** — they shift as you work.

## Rules of engagement

1. One fix per build/test cycle. Do not batch a risky change with a safe one.
2. After every fix: `ctest --test-dir build -C Release --output-on-failure` → 18/18.
3. For anything touching `src/physics/` or `src/render/`: also run `grid_bench.exe`
   before and after, back to back, and report both numbers. Never claim a perf win
   without paired output.
4. If a fix changes replay behaviour, say so explicitly and stop for confirmation
   before proceeding.
5. Leave the tree building and green. Report honestly what you did and did not finish.

---

## TIER 1 — safe, do these first

### 1. Fix `TUNING.md` drift  <- highest priority, zero risk

`TUNING.md` is actively hazardous: it steers agents toward mechanics that don't exist.
Verified against source:

- **Documented but absent from the entire tree** (delete these rows):
  `WALL_SLIDE_SPEED`, `COYOTE_STEPS`, `JUMP_BUFFER_STEPS`, `FLAP_FALL_CANCEL`,
  `GLIDE_GRAVITY`, `CRUSH_PERCENT`
- **Wrong values:** `MAX_STEP_HEIGHT` is **3** (doc says 2);
  `BURN_DAMAGE`/`BURN_INTERVAL_STEPS` are **2 / 6** (doc says 5 / 10)
- **Every `player.h` line number is stale:** `MOVE_SPEED` L62 (doc L75),
  `FLAP_IMPULSE` L95 (doc L129), `MAX_HEALTH` L130 (doc L181),
  `BURN_TEMPERATURE` L145 (doc L198)
- `Camera::SCALE` doesn't exist; it is `Camera::DEFAULT_SCALE` (`camera.h:16`)

Then **add a `tuning_test`** that parses `TUNING.md` and asserts every named constant
exists with the stated value. This is the codebase's own idiom — it already
static_asserts `MATERIALS` against `Element` (`element.h:97-153`). Wire it into
`CMakeLists.txt` alongside the other 18 suites.

**Finally:** once `TUNING.md` is correct, delete the warning blocks from `CLAUDE.md`
and `GEMINI.md` — otherwise they steer agents away from a doc that is now right.

### 2. Turn on `/W4` while it's free

The whole tree is already `/W4`-clean except six `C4996`. I verified this in a
parallel build tree. Lock it in before that stops being true:

```cmake
if(MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W4 /permissive-)
endif()
```

Use the define, not `fopen_s` — the `std::fopen`/`std::sscanf` calls
(`display.h:71,78,143`, `input_log.cpp:99,111`, `bmp.cpp:29`) are portable and
`fopen_s` is MSVC-flavoured Annex K, which would undercut the "builds anywhere" goal.
Expect `/permissive-` to possibly surface new errors; if it does, drop it and keep `/W4`.

### 3. Light field: 3.6x speedup — already verified, just re-apply

Measured 15.28 -> 4.23 ms/frame (91.7% -> 25.4% of a 60 Hz budget), 18/18 passing with
the golden-frame checksum **unchanged**. Three changes to `src/render/light.cpp`:

**(a) Table the `std::log`.** MSVC does not fold `std::log` of a constant at `/O2`,
so `cell_log_transmit` (`light.cpp:58-65`) makes ~311k libm calls per frame. Replace
the switch with a `std::array<float, size_t(ElementType::Count)>` built once at
static-init (place it *after* `LOG_CELL_AIR`, which it reads). Measured alone:
dark 1.246 -> 0.923 ms.

**(b) `pow` -> `exp`.** At `light.cpp:213-215`, `log(k)` is already in hand, so
`pow(k,c) == exp(c * log_k)` with one fewer round trip:

```cpp
const float log_k = log_transmit / (CELLS_PER_BLOCK / BLOCK);
const float k = std::exp(log_k);
transmit_diag[i]   = std::exp(log_k * 1.41421356f);
transmit_knight[i] = std::exp(log_k * 2.23606798f);
```

Measured cumulative: dark -> 0.564 ms. **Note this is not bit-identical** — it differs
in the last ulp (slightly more accurate). It survived the golden checksum here, but
treat it as a numerical change, not a pure refactor.

**(c) Thread the propagate sweep.** It is 88% of a lit frame (13.48 of 15.35 ms,
running 22 of 24 iterations — `CONVERGED` rarely fires). Each output block is a pure
function of `front[]`, `emission[]` and `transmit*[]`, none of which the pass writes,
so row slices are independent and the result is identical at any thread count. Use a
**persistent** pool (22 iterations x N spawns/frame would cost more than the work).

> **The single most important detail:** the per-slice convergence accumulator must
> be `alignas(64)`-padded. My first attempt used a plain `std::vector<float>` — 32
> bytes, one cache line, written 3x per block by every thread — and it scaled **1.15x**.
> A spin barrier made it *worse* than a condvar, which is the signature of contention,
> not dispatch latency. Padding it took propagate to 3.76x on 4 threads
> (13.54 -> 3.60 ms). Keep `local_change` in a register inside the slice and write
> `change[tid].v` exactly once at the end.

Measured scaling: 1t 13.54 ms -> 2t 6.93 (1.95x) -> 4t 3.60 (3.76x) -> 8t ~1.8.

*Shortcut:* a complete working version may still exist at
`C:\Users\Ethan\AppData\Local\Temp\claude\c--Users-Ethan-Desktop-game-toop-share\466657d4-4b0a-4760-b656-cfc37dbe080b\scratchpad\light.cpp.optimized`.
That path is from a prior session and may have been cleaned up — if it's gone, the
description above is sufficient to rebuild it.

### 4. Three `reserve()` calls

There is not a single `reserve()` in `src/physics/`. Scratch vectors are `.clear()`ed
(good) but grow by `push_back`, so the first collapse reaching a new high-water mark
reallocates mid-step. Three have hard known bounds — add to the `Grid` constructor:

```cpp
support_component.reserve(MAX_SUPPORT_CELLS + 1);  // guarded AFTER push, grid.cpp:474
support_stack.reserve(MAX_SUPPORT_CELLS);
pressure_queue.reserve(MAX_PRESSURE_CELLS);        // 512, grid.cpp:918
```

### 5. Kill the division-heavy sort comparator

`drop_component` (`grid.cpp:680-686`) runs `std::sort` with a comparator doing **two
divisions and two modulos per comparison**:

```cpp
const int ax = a % w, ay = a / w;   // ~49k comparisons x 4 idiv at 4096 cells
```

That's ~4-8 M cycles per large collapse. Precompute sort keys in one O(n) pass
(`(x << 20) | (H - y)`) and sort those. Verify with `collapse_test` and the
`collapsing`/`shattering` bench rows.

### 6. Delete dead per-frame work in the blit path

`main.cpp:1521` sets `PLANE_ON_NEAR_TERRAIN = false`, so `plane_src_row_for` stays all
`-1` and `surface_plane::apply` takes the straight-copy branch (`surface_plane.cpp:124`)
for every row — but it has already run `depth_map` unconditionally over the full
861x361 window, and `cell_depth` is **written at `main.cpp:1547,1557` and read
nowhere**. With the pass off, the copy is unnecessary too: `SDL_UpdateTexture` takes a
pitch, so upload `pixels.data() + view_y*grid_w + view_x` with pitch `grid_w * 4`
directly and skip `cell_window` entirely. Saves ~311k depth computations + ~2.5 MB of
traffic per frame. Keep the disabled pass reachable — don't delete the feature, just
stop paying for it while it's off.

### 7. Close the replay-checksum blind spot  <- invalidates `.rec` files

`input_log.cpp:60-68` mixes `type`, `color`, `updated_tag`, `ticks` — but **not
`temperature` and not `piece_tag`**, both of which are mutable simulation state. Two
runs could diverge in heat or fracture topology and a replay would verify clean. Add
two `mix()` calls.

**This invalidates any existing `session.rec`.** Confirm with the user before landing,
and check whether `test_run.cpp` or `test_scene.cpp` depend on a recorded fixture.

### 8. Fix one misleading comment

`main.cpp:1566-1569` claims the light field is "affordable" and "already too cheap to
measure". `grid_bench`'s own `light/fire` row says 91% of a 60 Hz frame. Replace with
the measured figure (post-fix-3, ~25%).

---

## TIER 2 — larger refactor, checksum-neutral

### 9. Drop `Element::color`

`Element::color` and `pixels[idx]` are provably always equal. There are exactly three
writers, and each mirrors into `pixels[]` on the same or next line: `grid.cpp:230+253`,
`grid.cpp:271-272`, `grid.cpp:1325-1326`. The field is 8.29 MB of duplication.

Removing it makes `Element` five `uint8_t`s -> `sizeof == 5`, `alignof == 1`:

| | now | after |
|---|---|---|
| `cells` @1080p | 23.7 MiB | 9.9 MiB |
| cells / 64B line | 5.33 | 12.8 |
| total hot state | 37.6 MiB | 23.7 MiB (drops below a 32 MB L3) |

`swap_elements` becomes a 5-byte swap plus a 4-byte `std::swap(pixels[...])`. Add
`uint32_t Grid::color_at(int x, int y) const`. External readers are five mechanical
sites: `input_log.cpp:64`, `plane_probe.cpp:251`, `rim_probe.cpp:93,104,108`,
`test_grid.cpp:1121,1134`, `test_run.cpp:52`, `test_scene.cpp:126`.

**Because the value is bit-identical, the replay checksum is unchanged and existing
`.rec` files still validate** — you can prove this refactor correct against recordings.
Update the `static_assert` in `element.h:97` to `== 5` and rewrite its comment (the
existing one reasons about a 3-byte hole that no longer exists).

Expect a broad win across every sweep. Measure `churning`, `cascading`, `burning`.

---

## TIER 3 — changes replays; do not start without explicit sign-off

### 10. Replace `vent_fluid`'s 2D scan

`vent_fluid` is **25.5 ms of the 50.2 ms `churning` step — 51% of the worst-case
simulation.** This is ablation-proven by `grid_bench`'s own table
(`all 50.24` vs `no vent 24.73`; `no seek` and `no lift` are noise).

`grid.cpp:1010-1013` runs an unpruned 49-cell scan for every powder grain whose target
is occupied. **I already tried the obvious micro-fix** (equivalence-preserving early
exit at `best_score == 1`, plus skipping probes that can't beat the incumbent since
the `drain` fallback is dead once `best_x >= 0`). It was worth **only 3.9%**
(50.22 -> 48.29 ms, tests green). That negative result is informative: the scan isn't
wasting time past a good hit — it genuinely fails to find a near candidate and walks
all 49 cells.

The real fix is algorithmic: the question ("nearest Empty within r whose neighbour
below is this fluid") is a **1D per-column query answered with a 2D sweep**. A
per-column fluid-surface index (`int16_t surface_y[width]`, ~3.8 KB -> L1-resident,
lazily refreshed and stamped by `step_count`) turns 49 strided probes into a 24.9 MB
AoS array into ~7 sequential probes into L1. Cache lines touched: ~14 -> 1.

**This changes tie-breaking.** The current strict `score < best_score` picks the first
minimum in scan order; a column-ordered scan picks a different one. Replays and any
golden checksum over a pour **will** need re-baselining. Land it with a fresh recording
and a `TUNING.md` History entry.

---

## Start here

Run `.\build\Release\grid_bench.exe` and `ctest --test-dir build -C Release` to confirm
you reproduce the baseline, then begin with Tier 1 item 1. Report the paired bench
numbers for every perf change.
