# Tuning Log

The knobs worth turning by feel, where they live, and what each one costs.
This is a running log: add a row when you retune something, and note it in the
History section at the bottom.

Scope is deliberately narrow. This lists constants you change to make the game
*feel* different — weight, speed, timing, animation. It is not an index of every
constant in the codebase. Simulation-correctness numbers (`MAX_SUPPORT_CELLS`,
`MIN_PRESSURE_HEAD`, reaction thresholds) are documented at their point of use.

The movement values below represent an initial baseline for feel tuning.

Rules that apply to everything below:

- **Speeds are `fx` fixed point, not floats — write them as exact rationals.**
  `fx::from_int(500)` is 500 cells/s; `fx::from_ratio(225, 2)` is 112.5.
  Avoid `fx::v(112.5f * 65536)`: compile-time float folding can vary across
  platforms. The value in the `Now` column below is the intended number; the code
  writes it as the rational that produces it exactly. `MAX_STEP_HEIGHT` and
  `FLAP_INTERVAL_STEPS` are plain `int` (counting cells and steps).
- **Velocities are in cells per second, and one cell is 4 screen pixels**
  (`Camera::SCALE`, [src/game/camera.h:10](src/game/camera.h#L10)). The player
  body is 8x20 cells, so "a body height" is 20 cells.
- **Anything measured in `steps` is a fixed simulation step, not a rendered
  frame.** The sim runs at 60 steps/s regardless of display framerate (60 steps = 1 second).

---

## Player weight and movement

[src/physics/player.h](src/physics/player.h)

| Knob | Line | Now | What it does |
|---|---|---|---|
| `MOVE_SPEED` | [75](src/physics/player.h#L75) | 112.5 | Horizontal speed. Applied directly to `vel_x` on Left/Right. Instant acceleration and instant stop — there is no inertia. |
| `JUMP_SPEED` | [76](src/physics/player.h#L76) | 175.0 | Standing jump. Set out of `GRAVITY` to give a 4-cell jump height. |
| `GRAVITY` | [77](src/physics/player.h#L77) | 500.0 | Downward acceleration. At 500 cells/s² it is ~1.5x Earth gravity (treating 1 cell as ~10 cm). |
| `MAX_FALL_SPEED` | [78](src/physics/player.h#L78) | 400.0 | Terminal velocity. Higher reads as heavier. |
| `MAX_STEP_HEIGHT` | [79](src/physics/player.h#L79) | 2 | Cells of vertical rise the player steps over automatically. 0 is caught on every pebble; 2 walks up gentle powder slopes. |
| `WALL_SLIDE_SPEED` | [80](src/physics/player.h#L80) | 50.0 | Slower fall while holding the stick into a wall. 0 is wall cling; = `MAX_FALL_SPEED` is no slide effect. |
| `COYOTE_STEPS` | [81](src/physics/player.h#L81) | 6 | Steps after walking off an edge during which a jump input is still honoured (~100 ms). |
| `JUMP_BUFFER_STEPS` | [82](src/physics/player.h#L82) | 6 | Steps before landing during which a jump press is remembered. |

---

## Flight weight

[src/physics/player.h](src/physics/player.h)

Flight is a deliberate flap mechanic rather than a jetpack: discrete pulses with
cooldown, governed by five constants:

| Knob | Line | Now | What it does |
|---|---|---|---|
| `FLAP_IMPULSE` | [129](src/physics/player.h#L129) | 177.0 | Upward impulse added to `vel_y` on a flap. |
| `FLAP_MAX_CLIMB` | [130](src/physics/player.h#L130) | 98.0 | Ceiling on upward velocity after a flap. Caps sustained ascent rate. |
| `FLAP_INTERVAL_STEPS` | [131](src/physics/player.h#L131) | 14 | Steps between flaps while holding the flap key (~4.3 flaps/s). |
| `FLAP_FALL_CANCEL` | [132](src/physics/player.h#L132) | 0.50 | Fraction of downward velocity cancelled before applying impulse when flapping mid-fall. |
| `GLIDE_GRAVITY` | [133](src/physics/player.h#L133) | 100.0 | Reduced gravity applied while holding flap key and falling (`vel_y > 0`). |

---

## Damage and losing the run

[src/physics/player.h](src/physics/player.h)

| Knob | Line | Now | What it does |
|---|---|---|---|
| `MAX_HEALTH` | [181](src/physics/player.h#L181) | 100 | Maximum health points. |
| `BURN_TEMPERATURE` | [198](src/physics/player.h#L198) | 100 | Temperature above which touching cells inflict burn damage. |
| `BURN_DAMAGE` / `BURN_INTERVAL_STEPS` | [205-206](src/physics/player.h#L205) | 5 dmg / 10 steps | Burn rate: 5 damage every 10 steps while in contact with hot cells. |
| `CRUSH_PERCENT` | [215](src/physics/player.h#L215) | 50% | Fraction of player body overlap with falling structural cells that triggers an instant defeat. |

---

## Animation timing

[src/render/player_sprite.h](src/render/player_sprite.h) (authored via `tools/player_sheet.py`)

| Animation | Now | Notes |
|---|---|---|
| `idle` | 2 frames, wait 30 | 30 simulation steps per frame (0.5 s). |
| `walk` | 6 frames, wait 6 | 6 steps per frame (36 steps = 0.6 s per walk cycle). |
| `rise` / `fall` | 1 frame, wait 0 | Single-frame directional flight/fall poses. |
| `dig` | 3 frames, wait 8 | 8 steps per frame, non-looping. Runs during dig action. |
| `fly` | 6 frames, wait 3 | 3 steps per frame. Flap cycle. |

---

## Dig tool

[src/physics/tool.h](src/physics/tool.h)

| Knob | Line | Now | What it does |
|---|---|---|---|
| `RANGE` | [23](src/physics/tool.h#L23) | 3 body heights | Maximum reach distance from player body center to cursor. |
| `RADIUS` | [29](src/physics/tool.h#L29) | 3/4 body width | Excavation brush radius in cells. |
| `COOLDOWN_STEPS` | [34](src/physics/tool.h#L34) | 6 | Steps between dig operations while holding the tool button. |

---

## Fire and steam timing

[src/physics/grid.h](src/physics/grid.h)

| Knob | Line | Now | What it does |
|---|---|---|---|
| `STEAM_LIFETIME_MEAN` | [grid.h](src/physics/grid.h) | 200 | Average lifetime in steps for steam cells in contact with surfaces. |
| `STEAM_LIFETIME_SPREAD` | [grid.h](src/physics/grid.h) | 40 | Random jitter range (mean ± spread) for steam lifespan. |
| `FLAME_LIFETIME_MEAN` | [grid.h](src/physics/grid.h) | 13 | Average lifetime in steps for an emitted flame particle. |
| `FLAME_RISE_SKIP_PERCENT` | [grid.h](src/physics/grid.h) | 10 | Percentage chance per step that a flame cell skips rising (produces ragged top edge). |

---

## Toppling

[src/physics/grid.h](src/physics/grid.h)

| Knob | Line | Now | What it does |
|---|---|---|---|
| `TIP_GRAVITY` | [776](src/physics/grid.h#L776) | 1/4 cell/step² | How hard a piece leaning over its edge is pulled round. Tied to `TICKS_PER_SPEEDUP` so tipping and falling share one gravity. Higher topples faster. |
| `TIP_MAX_STEPS` | [783](src/physics/grid.h#L783) | 600 | Safety cap: a piece still tipping after this many steps is frozen where it is. A real topple ends long before. |

---

## Light

[src/render/light.h](src/render/light.h), [src/render/light.cpp](src/render/light.cpp)

| Knob | Line | Now | What it does |
|---|---|---|---|
| `TRANSMIT_CLEAR` | [light.cpp:71](src/render/light.cpp#L71) | 0.77 | Light transmission factor through empty/air cells per sample block. |
| `ITERATIONS` | [light.h:58](src/render/light.h#L58) | 24 | Number of propagation passes for the light field. |
| `BLOCK` | [light.h:43](src/render/light.h#L43) | 4 | Cell block size per light sample (4x4 cells). |

---

## Depth grading

[src/render/frame.cpp](src/render/frame.cpp), [src/render/surface_plane.h](src/render/surface_plane.h)

Grading multipliers shape the relative brightness of background, terrain, and light.

| Knob | Line | Now | What it does |
|---|---|---|---|
| `mountains` grade | [frame.cpp](src/render/frame.cpp) | 0.60 (153) | Darkness of distant mountain silhouette against the sky. |
| `sky` grade | [frame.cpp](src/render/frame.cpp) | 1.00 | Identity. Serves as base reference for other layers. |
| `cells` grade | [frame.cpp](src/render/frame.cpp) | 1.00 | Identity. Simulated matter renders with authored palette colors. |
| `ground` grade | [frame.cpp](src/render/frame.cpp) | 0.53 (135) | Grade multiplier for receding ground plane. |
| `PLANE_TEXEL_SCALE` | [backdrop_wrap.h](src/render/backdrop_wrap.h) | 2.5 | Screen pixels per row of ground tile, controlling apparent depth. |
| `SKIN_CELLS` / `FULL_END` / `DEPTH_END` | [surface_plane.h](src/render/surface_plane.h) | 4 / 256 / 320 cells | Depth range for blending terrain surface into receding plane values. |
| `PLANE_FADE_END_T` | [surface_plane.h](src/render/surface_plane.h) | 125 (= 1.25) | Normalized ground plane position where near-ground blend terminates. |
| `GROUND_STRIPS` | [frame.cpp](src/render/frame.cpp) | 24 | Number of horizontal strips used to render receding plane parallax. |

---

## Perspective rig (bg_tarn)

[src/render/rig_backdrop.h](src/render/rig_backdrop.h), [src/render/depth_rig.h](src/render/depth_rig.h)

Layer parallax factors in a rig set are **not** knobs: each is derived from the
row the layer stands on (`foot_row`). Move the horizon or contact row and every
factor moves together. The knobs are the camera and the motion.

| Knob | Line | Now | What it does |
|---|---|---|---|
| `Rig::horizon_row` | [rig_backdrop.h:119](src/render/rig_backdrop.h#L119) | 200 | Art row where the ground plane vanishes (factor 0). Must match `HORIZON` in `tools/generate_bg_tarn.py`. |
| `Rig::contact_row` | [rig_backdrop.h:119](src/render/rig_backdrop.h#L119) | 264 | Art row where the plane meets the world (factor 1); the terrain surface. |
| `Rig::vertical_strength` | [rig_backdrop.h:119](src/render/rig_backdrop.h#L119) | 0.75 | Share of honest vertical parallax. 1.0 = true camera (plane magnifies 3.6x at the ceiling, 1080p); 0 = bg1's locked vertical. |
| `ripple_amplitude` | [rig_backdrop.h:119](src/render/rig_backdrop.h#L119) | 0.6 cells | Sideways shimmer of lake rows and the sun glint. Keep under 1 cell or rows visibly tear. |
| clouds `drift` | [rig_backdrop.h:106](src/render/rig_backdrop.h#L106) | -0.6 cells/s | Cloud drift with no camera motion. |
| reeds `factor` | [rig_backdrop.h:115](src/render/rig_backdrop.h#L115) | 1.30 | Foreground speed. Above 1.00 is legal only because rig layers wrap. |

---

## Camera framing

[src/game/camera.h](src/game/camera.h)

| Constant | Value | What it does |
|---|---|---|
| `Camera::VERTICAL_ANCHOR` | 0.80 | Viewport fraction where player sits vertically (0.5 is centered, 0.80 frames player in lower portion to emphasize sky and terrain). |

---

## Debug camera

[src/game/debug_view.h](src/game/debug_view.h)

| Knob | Line | Now | What it does |
|---|---|---|---|
| `PAN_CELLS_PER_SECOND` | [debug_view.h](src/game/debug_view.h) | 200 | Free camera pan speed in cells per second. |
| `PAN_FAST_MULTIPLIER` | [debug_view.h](src/game/debug_view.h) | 4 | Multiplier applied when holding Shift during free camera pan. |

---

## Enemies

[src/physics/enemy.h](src/physics/enemy.h). The body is the player's 8x20 box;
the art (and so which pixels can be hit) is the ASCII grid in
[src/physics/enemy_art.h](src/physics/enemy_art.h).

| Knob | Line | Now | What it does |
|---|---|---|---|
| `PATROL_SPEED` | [47](src/physics/enemy.h#L47) | 22 | Wandering walk speed, cells/s. |
| `CHASE_SPEED` | [48](src/physics/enemy.h#L48) | 45 | Speed once it has noticed you. Asserted below the player's `MOVE_SPEED`, so it can always be walked away from. |
| `JUMP_SPEED` | [54](src/physics/enemy.h#L54) | 120 | The hop it makes at a wall it cannot step over while chasing. |
| `NOTICE_X` | [69](src/physics/enemy.h#L69) | 12 body widths | How close you have to be, horizontally, before it chases. |
| `LEDGE_DROP` | [74](src/physics/enemy.h#L74) | 1 body height | A wandering enemy turns back at a drop deeper than this. |
| `SWIPE_DAMAGE` | [81](src/physics/enemy.h#L81) | 8 | Damage per swipe when the boxes touch (needs an arm). |
| `SWIPE_INTERVAL_STEPS` | [82](src/physics/enemy.h#L82) | 40 | Steps between swipes. |
| `COLLAPSE_PERCENT` | [92](src/physics/enemy.h#L92) | 40 | Below this share of its pixels it collapses into sand. |
| `BURN_PIXELS_PER_TICK` | [106](src/physics/enemy.h#L106) | 2 | Pixels burned away per burn tick, lowest first. |

## Bow and arrows

[src/physics/arrow.h](src/physics/arrow.h)

| Knob | Line | Now | What it does |
|---|---|---|---|
| `LAUNCH_SPEED` | [57](src/physics/arrow.h#L57) | 480 | Arrow speed at release, cells/s (8 cells a step). |
| `GRAVITY` | [64](src/physics/arrow.h#L64) | 150 | Arrow drop. About 8 cells over a 160-cell shot. |
| `DRAW_STEPS` | [69](src/physics/arrow.h#L69) | 20 | Steps between shots while `E` is held. |
| `STUCK_STEPS` | [72](src/physics/arrow.h#L72) | 240 | How long a stuck arrow stays in a wall. |
| `BITE_RADIUS` | [79](src/physics/arrow.h#L79) | 2 | Radius of body an arrow takes out where it lands. Asserted wider than an arm. |
| `FLUID_DRAG_PERCENT` | [84](src/physics/arrow.h#L84) | 80 | Speed kept per step through water or oil. |

---

## History

Record adjustments to tuning parameters and their rationale here.

- **Enemies and the bow added** (all values above are first-pass). Arrow
  gravity was taken down from 260 to 150 before landing: at 260 a shot across
  160 cells dropped about 19, enough to land in the ground in front of a target
  aimed at the chest. Enemy burning was capped at 2 pixels a tick after the
  first version took every pixel in a flame at once, which removed both feet on
  contact and killed the body the step it touched fire.
- **2026-10-07** — Added `TIP_GRAVITY` (1/4 cell/step²) and `TIP_MAX_STEPS` (600)
  with toppling. Gravity matches the falling-piece acceleration
  (`TICKS_PER_SPEEDUP` = 4) so a post going over and one dropping read as the
  same world; the step cap is a backstop, not a feel knob.
- **2026-10-07, perspective rig (`bg_tarn`).** Added `vertical_strength` 0.75,
  `ripple_amplitude` 0.6, cloud `drift` -0.6, reeds 1.30. 0.75 because at 0.5 the
  valley drops out of a ceiling-height window and at 1.0 the plane smears at 3.6x;
  see the comment above `TARN_LAYERS`. Existing bg1-family sets are unchanged.
