# Tuning Log

The knobs worth turning by feel, where they live, and what each one costs.
This is a running log: add a row when you retune something, and note it in the
History section at the bottom.

Scope is deliberately narrow. This lists constants you change to make the game
*feel* different — weight, speed, timing, animation. It is not an index of every
constant in the codebase. Simulation-correctness numbers (`MAX_SUPPORT_CELLS`,
`LIQUID_LEVEL_HEAD`, reaction thresholds) are documented at their point of use.

The movement values below represent an initial baseline for feel tuning.

Rules that apply to everything below:

- **Speeds are `fx` fixed point, not floats — write them as exact rationals.**
  `fx::from_int(500)` is 500 cells/s; `fx::from_ratio(225, 2)` is 112.5.
  Avoid `fx::v(112.5f * 65536)`: compile-time float folding can vary across
  platforms. The value in the `Now` column below is the intended number; the code
  writes it as the rational that produces it exactly. `MAX_STEP_HEIGHT` and
  `FLAP_INTERVAL_STEPS` are plain `int` (counting cells and steps).
- **Velocities are in cells per second, and one cell is 4 screen pixels**
  (`Camera::DEFAULT_SCALE`, [camera.h](src/game/camera.h); a scene may
  override it). The player
  body is 8x20 cells, so "a body height" is 20 cells.
- **Anything measured in `steps` is a fixed simulation step, not a rendered
  frame.** The sim runs at 60 steps/s regardless of display framerate (60 steps = 1 second).
- **Each row links the file that declares the knob, never a line in it.** Line
  links went stale with every unrelated edit above them, and every branch that
  re-pointed them conflicted with every other branch that did. `tuning_test`
  finds an upper-case constant's declaration in the linked file by name, so a
  row whose constant is renamed or removed still fails, and a row whose constant
  merely moved does not. A `#L` link anywhere in this file fails the test.

---

## Player weight and movement

[src/physics/player.h](src/physics/player.h)

| Knob | File | Now | What it does |
|---|---|---|---|
| `MOVE_SPEED` | [player.h](src/physics/player.h) | 112.5 | Horizontal speed. Applied directly to `vel_x` on Left/Right. Instant acceleration and instant stop — there is no inertia. |
| `JUMP_SPEED` | [player.h](src/physics/player.h) | 175.0 | Standing jump. Set out of `GRAVITY` to give a 4-cell jump height. |
| `GRAVITY` | [player.h](src/physics/player.h) | 500.0 | Downward acceleration. At 500 cells/s² it is ~1.5x Earth gravity (treating 1 cell as ~10 cm). |
| `MAX_FALL_SPEED` | [player.h](src/physics/player.h) | 400.0 | Terminal velocity. Higher reads as heavier. |
| `MAX_STEP_HEIGHT` | [player.h](src/physics/player.h) | 3 | Cells of vertical rise the player steps over automatically. 0 is caught on every pebble; 3 walks up powder slopes. |

---

## Flight weight

[src/physics/player.h](src/physics/player.h)

Flight is a deliberate flap mechanic rather than a jetpack: discrete pulses with
cooldown, governed by three constants:

| Knob | File | Now | What it does |
|---|---|---|---|
| `FLAP_IMPULSE` | [player.h](src/physics/player.h) | 177.0 | Upward impulse added to `vel_y` on a flap. |
| `FLAP_MAX_CLIMB` | [player.h](src/physics/player.h) | 98.0 | Ceiling on upward velocity after a flap. Caps sustained ascent rate. |
| `FLAP_INTERVAL_STEPS` | [player.h](src/physics/player.h) | 14 | Steps between flaps while holding the flap key (~4.3 flaps/s). |

---

## Damage and losing the run

[src/physics/player.h](src/physics/player.h)

| Knob | File | Now | What it does |
|---|---|---|---|
| `MAX_HEALTH` | [player.h](src/physics/player.h) | 100 | Maximum health points. |
| `BURN_TEMPERATURE` | [player.h](src/physics/player.h) | 100 | Temperature above which touching cells inflict burn damage. |
| `BURN_DAMAGE` | [player.h](src/physics/player.h) | 2 | Damage per burn tick while in contact with hot cells. With `BURN_INTERVAL_STEPS` this is 20/s: five seconds in flame is death. |
| `BURN_INTERVAL_STEPS` | [player.h](src/physics/player.h) | 6 | Steps between burn ticks. |

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

| Knob | File | Now | What it does |
|---|---|---|---|
| `RANGE` | [tool.h](src/physics/tool.h) | 3 body heights | Maximum reach distance from player body center to cursor. |
| `RADIUS` | [tool.h](src/physics/tool.h) | 3/4 body width | Excavation brush radius in cells. |
| `SWING_STEPS` | [tool.h](src/physics/tool.h) | 36 | Length of one dig swing in steps; the dig and its animation share this one clock. Longer than the walk cycle so a swing reads heavier than a stride. |

---

## Fire and steam timing

[src/physics/grid.h](src/physics/grid.h)

| Knob | File | Now | What it does |
|---|---|---|---|
| `STEAM_LIFETIME_MEAN` | [grid.h](src/physics/grid.h) | 200 | Average lifetime in steps for steam cells in contact with surfaces. |
| `STEAM_LIFETIME_SPREAD` | [grid.h](src/physics/grid.h) | 40 | Random jitter range (mean ± spread) for steam lifespan. |
| `FLAME_LIFETIME_MEAN` | [grid.h](src/physics/grid.h) | 13 | Average lifetime in steps for an emitted flame particle. |
| `FLAME_RISE_SKIP_PERCENT` | [grid.h](src/physics/grid.h) | 10 | Percentage chance per step that a flame cell skips rising (produces ragged top edge). |

---

## Liquids

[src/physics/grid.h](src/physics/grid.h). Amounts are in `LIQUID_FULL` units:
4096 is one full cell.

| Knob | File | Now | What it does |
|---|---|---|---|
| `LIQUID_ACCEL` | [grid.h](src/physics/grid.h) | 64 | How hard a difference in amount pushes a cell's sideways velocity, /256 per step. Higher runs faster, but 96 or more stops pools sleeping. |
| `LIQUID_DAMP` | [grid.h](src/physics/grid.h) | 224 | Velocity kept per step, /256. Higher carries a surge further; 244 or more stops pools sleeping. |
| `LIQUID_SPEED_PER_SPREAD` | [grid.h](src/physics/grid.h) | 256 | A liquid's top sideways speed is its `spread` times this, in units a step. Water's spread is 6 ([material.h](src/physics/material.h)), which is 1536, three-eighths of a cell; oil's is 3. A liquid's spread also sets how much of a difference it passes on each step (`2 + 10 / spread`), so a higher spread is runnier both ways. |
| `LIQUID_FLOW_SCALE` | [grid.h](src/physics/grid.h) | 16 | Units per step of the one-byte velocity, so the byte tops out at 127 times this. That has to stay above water's own ceiling, or the byte caps water instead. |
| `LIQUID_DEAD` | [grid.h](src/physics/grid.h) | 16 | A difference smaller than this moves nothing sideways, so a level surface sleeps. Lower levels flatter and settles slower. |
| `LIQUID_MIN_SPREAD` | [grid.h](src/physics/grid.h) | 1024 | A cell holding less than this does not spread onto dry ground: how thin a puddle gets before it stops. |
| `LIQUID_LEVEL_HEAD` | [grid.h](src/physics/grid.h) | 1024 | The smallest difference between two surfaces of one body that pressure moves water across. |
| `LIQUID_LEVEL_RATE` | [grid.h](src/physics/grid.h) | 1024 | The most pressure moves from one surface cell per step: how fast a U-tube is seen to flow. |
| `LIQUID_DISPERSE_RADIUS` | [grid.h](src/physics/grid.h) | 3 | How far a sinking grain reaches to press its water into the pool. Smaller lets a pour lift water out of the pool. |

---

## Loose bodies

[src/physics/grid.h](src/physics/grid.h), [src/physics/material.h](src/physics/material.h)

A piece going anywhere other than straight down -- toppling, tumbling, sliding,
being knocked over, floating -- is a rigid body with momentum and spin (see
"loose bodies" in `grid.h`). The per-material columns `weight`, `grip`,
`bounce` and `strength` in `MATERIALS` are physical properties rather than
knobs: real bulk densities in tenths of g/cm³ (stone 26, wood 6, charcoal 4,
sand 16, water 10), friction and restitution in percent, and bending strength
(wood 20000 all but never snaps, stone 3000 cracks on a hard landing, charcoal
300 crumbles). `tests/body_probe.cpp` shows
what each scene does in numbers and pictures; run it after turning any of these.

| Knob | File | Now | What it does |
|---|---|---|---|
| `BODY_GRAVITY` | [grid.h](src/physics/grid.h) | 9/40 cell/step² | Gravity on every loose piece, falling or toppling (falling pieces are bodies too). Higher topples, tumbles and falls faster. |
| `AIR_DRAG` | [grid.h](src/physics/grid.h) | 1/250 | Air drag, per step, times speed squared. Sets the speed a fall eases into, sqrt(gravity / drag), about 7.5 cells/step. Higher falls slower and floatier. |
| `WARM_START` | [grid.h](src/physics/grid.h) | 1/4 | Share of last step's contact impulse each contact starts from. What lets a stack carry its top block's weight to the bottom one. At 1 a toppling stack threw its blocks across the scene; at 1/2 it scattered. |
| `GRIP_ROUGHNESS` | [grid.h](src/physics/grid.h) | 20 | Percent either way a surface's grip varies from spot to spot, so a slide catches and slips. 0 is perfectly uniform. |
| `NUDGE_SPIN` | [grid.h](src/physics/grid.h) | 1/1500 rad/step | Most spin a piece is given as it comes loose. 0 makes every topple of a shape identical. |
| `BALANCE_MARGIN` | [grid.h](src/physics/grid.h) | 1/2 cell | A loose piece whose centre is this close to the edge of its footing is left to the solver instead of being ruled standing. 0 is the knife edge. |
| `IMPACT_LOAD_MAX` | [grid.h](src/physics/grid.h) | 6 | Most times its own weight a landing loads a body with when deciding whether an overhang snaps. Higher breaks more on landing. Strength itself is per material: `strength` in [material.h](src/physics/material.h). |
| `BODY_SOLVER_PASSES` | [grid.h](src/physics/grid.h) | 32 | Contact solver passes per step. At 8 a six-block stack could not carry its top blocks' weight down to the bottom one and never went over. Cost is per contact (about two per face), so it is cheap. |
| `BODY_SLEEP_DRIFT` | [grid.h](src/physics/grid.h) | 1/2 cell | A body held by something that has drifted less than this over `BODY_SLEEP_STEPS` has stopped. |
| `BODY_SLEEP_STEPS` | [grid.h](src/physics/grid.h) | 30 | Half a second. Shorter risks stopping a topple that starts from the very edge of balance before it is past half a cell. |
| `ROLLING_ARM` | [grid.h](src/physics/grid.h) | 1/16 cell | Rolling resistance. Higher stops discs and rubble sooner, but a post only just past its balance point needs more torque than this to get going. |
| `BOUNCE_MIN_SPEED` | [grid.h](src/physics/grid.h) | 3/4 cell/step | Impacts slower than this do not bounce. Lower makes landings livelier and resting bodies jitter. |
| `WAKE_SPEED` | [grid.h](src/physics/grid.h) | 3/8 cell/step | How fast a body must strike a piece at rest to wake it and push it. |
| `PUSH_WAKE` | [grid.h](src/physics/grid.h) | 1/64 cell/step | How hard a body must lean sideways on a piece at rest to wake it -- a domino gone over onto the next. |
| `LIQUID_DRAG` | [grid.h](src/physics/grid.h) | 96 | Share of a fully submerged body's velocity lost per step, /256. At 48 a floating raft bobbed on its own waves for ever; at 96 it settles in about three seconds and stone sinks at about 24 cells/s. |
| `LOAD_SCAN` | [grid.h](src/physics/grid.h) | 48 | Tallest column of sand counted as load on a piece. Taller is a ceiling that holds itself. |
| `BODY_MAX_STEPS` | [grid.h](src/physics/grid.h) | 1800 | Safety cap: a body still moving after this many steps is frozen where it is. |

---

## Light

[src/render/light.h](src/render/light.h), [src/render/light.cpp](src/render/light.cpp)

| Knob | File | Now | What it does |
|---|---|---|---|
| `TRANSMIT_CLEAR` | [light.cpp](src/render/light.cpp) | 0.77 | Light transmission factor through empty/air cells per sample block. |
| `ITERATIONS` | [light.h](src/render/light.h) | 24 | Number of propagation passes for the light field. |
| `BLOCK` | [light.h](src/render/light.h) | 4 | Cell block size per light sample (4x4 cells). |

---

## Perspective rig (bg_tarn)

[assets/bg_tarn/backdrop.txt](assets/bg_tarn/backdrop.txt), [src/render/depth_rig.h](src/render/depth_rig.h)

Layer parallax factors in a rig set are **not** knobs: each is derived from the
row the layer stands on (`foot_row`). Move the horizon or contact row and every
factor moves together. The knobs are the camera and the motion.

| Knob | File | Now | What it does |
|---|---|---|---|
| `rig` horizon | [backdrop.txt](assets/bg_tarn/backdrop.txt) | 200 | Art row where the ground plane vanishes (factor 0). Must match `HORIZON` in `tools/generate_bg_tarn.py`. |
| `rig` contact | [backdrop.txt](assets/bg_tarn/backdrop.txt) | 264 | Art row where the plane meets the world (factor 1); the terrain surface. |
| `rig` vertical strength | [backdrop.txt](assets/bg_tarn/backdrop.txt) | 0.75 | Share of honest vertical parallax. 1.0 = true camera (plane magnifies 3.2x at the ceiling, 1080p); 0 = bg_valley's locked vertical. |
| `ripple` | [backdrop.txt](assets/bg_tarn/backdrop.txt) | 0.6 cells | Sideways shimmer of lake rows and the sun glint. Keep under 1 cell or rows visibly tear. |
| clouds `drift` | [backdrop.txt](assets/bg_tarn/backdrop.txt) | -0.6 cells/s | Cloud drift with no camera motion. |
| reeds `factor` | [backdrop.txt](assets/bg_tarn/backdrop.txt) | 1.30 | Foreground speed. Above 1.00 is legal only because rig layers wrap. |
| scene `scale` | [scenes.txt](assets/scenes.txt) | 8 | Screen pixels per cell for `bg_tarn` and `bg_tarn_wide`: how close the camera frames the lake (430x180 cells at 3440x1440). 8 is as far out as the current art allows; at 7 `bg_tarn_wide`'s hills tile repeats on screen. |

---

## Camera framing

[src/game/camera.h](src/game/camera.h)

| Constant | Value | What it does |
|---|---|---|
| `Camera::VERTICAL_ANCHOR` | 0.80 | Viewport fraction where player sits vertically (0.5 is centered, 0.80 frames player in lower portion to emphasize sky and terrain). |

---

## Debug camera

[src/game/debug_view.h](src/game/debug_view.h)

| Knob | File | Now | What it does |
|---|---|---|---|
| `PAN_CELLS_PER_SECOND` | [debug_view.h](src/game/debug_view.h) | 200 | Free camera pan speed in cells per second. |
| `PAN_FAST_MULTIPLIER` | [debug_view.h](src/game/debug_view.h) | 4 | Multiplier applied when holding Shift during free camera pan. |

---

## Enemies

[src/physics/enemy.h](src/physics/enemy.h). Each kind of enemy is a `Species`
row (`species::GHOUL`, `species::TROLL`, `species::FISH`, `species::ANGLER`);
the art, and so which pixels can be hit, is the grid in
[src/physics/enemy_art.h](src/physics/enemy_art.h) (ghoul),
[src/physics/troll_art.h](src/physics/troll_art.h) (troll, generated by
`tools/troll_art.py`), [src/physics/fish_art.h](src/physics/fish_art.h)
(fish, generated by `tools/fish_art.py`) and
[src/physics/angler_art.h](src/physics/angler_art.h) (angler, generated by
`tools/angler_art.py`). Every row is checked by the `static_assert`s over
`species::ALL` at the bottom of enemy.h, one a rule.

### Ghoul (`species::GHOUL`) -- the player's 8x20 box

| Knob | File | Now | What it does |
|---|---|---|---|
| patrol speed | [enemy.h](src/physics/enemy.h) | 22 | Wandering walk speed, cells/s. |
| chase speed | [enemy.h](src/physics/enemy.h) | 45 | Speed once it has noticed you. Asserted below the player's `MOVE_SPEED`, so it can always be walked away from. |
| jump speed | [enemy.h](src/physics/enemy.h) | 120 | The hop it makes at a wall it cannot step over while chasing. |
| notice x | [enemy.h](src/physics/enemy.h) | 12 body widths | How close you have to be, horizontally, before it chases. |
| ledge drop | [enemy.h](src/physics/enemy.h) | 1 body height | A wandering enemy turns back at a drop deeper than this. |
| damage | [enemy.h](src/physics/enemy.h) | 8 | Damage per swipe when the boxes touch (needs an arm). |
| attack interval | [enemy.h](src/physics/enemy.h) | 40 | Steps between swipes. |
| collapse percent | [enemy.h](src/physics/enemy.h) | 40 | Below this share of its pixels it collapses into sand. |
| burn pixels per tick | [enemy.h](src/physics/enemy.h) | 2 | Pixels burned away per burn tick, lowest first. |

### Troll (`species::TROLL`) -- a 24x66 box, 52x70 frame

| Knob | File | Now | What it does |
|---|---|---|---|
| box | [enemy.h](src/physics/enemy.h) | 24 x 66 | Asserted over 2.5x the player's height. |
| patrol / chase speed | [enemy.h](src/physics/enemy.h) | 14 / 32 | Slower than the ghoul both ways; its reach does the work. |
| jump speed | [enemy.h](src/physics/enemy.h) | 140 | Heaves itself up a ledge of about 20 cells. |
| max step height | [enemy.h](src/physics/enemy.h) | 6 | Steps over what stops the player. |
| notice x / y | [enemy.h](src/physics/enemy.h) | 20 body widths / 90 | Sees farther, because its slam is slow. |
| damage | [enemy.h](src/physics/enemy.h) | 30 | Per slam. A third of the bar. |
| attack interval | [enemy.h](src/physics/enemy.h) | 50 | Steps it stands spent after a slam -- the window to punish it. |
| wind-up | [enemy.h](src/physics/enemy.h) | 42 | Steps from deciding to slam to the club landing. Asserted long enough to walk out from under. |
| reach | [enemy.h](src/physics/enemy.h) | 20 | How far past its front edge the slam lands. |
| crush radius | [enemy.h](src/physics/enemy.h) | 5 | Radius of ground the slam breaks and throws. Asserted to fit inside the reach with the debris clear of its feet. |
| burn pixels per tick | [enemy.h](src/physics/enemy.h) | 4 | Twice the ghoul's; it has ten times the pixels. |
| wades | [enemy.h](src/physics/enemy.h) | true | Powder never stops it; it shoves sand out of its box. |

### Fish (`species::FISH`) -- a 30x66 box, 94x84 frame

| Knob | File | Now | What it does |
|---|---|---|---|
| frame | [fish_art.h](src/physics/fish_art.h) | 94 x 84 | A fifth taller than the troll's 70 rows (asserted). The arm's crown to the soles is 80 cells against the troll's 66. |
| box | [enemy.h](src/physics/enemy.h) | 30 x 66 | The fish's middle and the legs. Tail, snout and lure arm overhang it. |
| patrol / chase speed | [enemy.h](src/physics/enemy.h) | 18 / 40 | Between the troll and the ghoul: long legs walking, not a giant lumbering. |
| jump speed | [enemy.h](src/physics/enemy.h) | 140 | The troll's. |
| max step height | [enemy.h](src/physics/enemy.h) | 6 | The troll's. |
| notice x / y | [enemy.h](src/physics/enemy.h) | 20 body widths / 90 | The troll's: its attack is a wind-up too. |
| damage | [enemy.h](src/physics/enemy.h) | 20 | Per cast. A fifth of the bar: a slap, not a club. |
| attack interval | [enemy.h](src/physics/enemy.h) | 36 | Steps it stands spent after a cast. |
| wind-up | [enemy.h](src/physics/enemy.h) | 36 | Steps from deciding to cast to the hand landing. Asserted long enough to walk out from under. |
| reach | [enemy.h](src/physics/enemy.h) | 18 | How far past its front edge the hand lands. |
| crush radius | [enemy.h](src/physics/enemy.h) | 0 | None: a hand breaks no ground, and `Enemy::attack` skips the crater. |
| burn pixels per tick | [enemy.h](src/physics/enemy.h) | 4 | The troll's. |
| wades | [enemy.h](src/physics/enemy.h) | true | The troll's reason: its own arm shot off is a pile deeper than a step. |

### Angler (`species::ANGLER`) -- the fish on tentacles (experimental)

Every row of the fish's table holds for the angler too, value for value (box,
speeds, notice, damage, wind-up, reach, wades): the body and how it hunts are
the fish's. What differs is that its legs and lure are chains of links
([src/physics/tentacle.h](src/physics/tentacle.h)) that bend along their length,
moved by `ANGLER_TENTACLES` in [enemy.h](src/physics/enemy.h):

| Knob | File | Now | What it does |
|---|---|---|---|
| links | [angler_art.py](tools/angler_art.py) | 8 a leg, 12 the lure | Links per tentacle. More bend more smoothly; the lure is nearly twice as long and hooks on itself. |
| wave steps / amplitude / spread | [enemy.h](src/physics/enemy.h) | 110, 1.5, 45 deg | The travelling wave down each tentacle: steps per wave, cells either side at its fullest (midway along, none at the ends), phase from one joint to the next. |
| iterations | [enemy.h](src/physics/enemy.h) | 3 | FABRIK rounds a step. The chain carries on from where it stopped, so it need not converge in one step. |
| smoothing | [enemy.h](src/physics/enemy.h) | 1/4 | Share of the way each middle joint relaxes toward straight before the solve. Irons out the folds a slack chain makes. |
| max bend | [enemy.h](src/physics/enemy.h) | 35 deg | The furthest any joint may bend. Just over the lure's sharpest joint at rest (34); also bounds the gap filled at the outside of a bend. |
| step lift | [enemy.h](src/physics/enemy.h) | 5 | Cells a foot rises mid-swing. |
| bow | [enemy.h](src/physics/enemy.h) | 1 | Cells a step each leg's middle is pushed outward (rear back, front forward), so slack bows the same way every time -- and so a near-straight leg can buckle toward a foot brought in closer. |
| swing steps | [enemy.h](src/physics/enemy.h) | 12 | Steps a foot is in the air, whatever the speed: faster walking makes longer strides, not quicker ones. |
| step / settle trigger | [enemy.h](src/physics/enemy.h) | 10 / 3 | Cells a planted foot may fall behind its home before it steps; once the body has stopped, before it steps to tidy itself under the body. |
| step lead | [enemy.h](src/physics/enemy.h) | 1/2 | Share of a swing's travel home is led ahead by, so a foot lands where the body is going. |
| foot probe | [enemy.h](src/physics/enemy.h) | 4 | Columns either side of home searched for a foothold, nearest first; solid ground is preferred to powder within it. |
| stride | [enemy.h](src/physics/enemy.h) | 24 | Not read by the angler: its feet step on `step trigger` and its bob comes from its feet. Kept because every rig has one. |
| body follow | [enemy.h](src/physics/enemy.h) | 30 steps, zeta 3/4, r 0 | Second-order follower (period, damping, response -- see `tentacle::response`) the body's lean and sink take from the feet. |
| body sink / tilt | [enemy.h](src/physics/enemy.h) | 2 / 12 deg | Most the body sinks or rises with the feet's average height, and tilts toward the lower foot. |
| footfall dip | [enemy.h](src/physics/enemy.h) | 1/4 | Cells a step of downward speed each footfall knocks into the body. |
| lure follow | [enemy.h](src/physics/enemy.h) | 22 steps, zeta 9/26, r 0 | Second-order follower the bulb swings on. The same motion as the spring it replaced (1/12 of the gap, 1/5 of the speed a step): loose and underdamped, it sways past and swings back. |
| lure sway / reach | [enemy.h](src/physics/enemy.h) | 3 / 10 | Cells the bulb drifts with each breath; cells further forward it is held while chasing. |
| lure raise / arc | [enemy.h](src/physics/enemy.h) | (32,-26) / (88,-24) | Frame points: where the wind-up rears the bulb to (near the lure's full length, so it cannot fold), and what the strike curves over on its way to the ground -- no further out than 6 past where it is aimed. |
| lure hold | [enemy.h](src/physics/enemy.h) | 6 | Steps the bulb is held, trembling, at the top of the wind-up before the whip. |
| lure crack / flourish | [enemy.h](src/physics/enemy.h) | 4 / 3 | Cells across of the bend rolled down the stalk through the whip, and extra wave of the thrash after it lands. |
| lure live / bulb radius | [enemy.h](src/physics/enemy.h) | 3 / 3 | The bulb hurts through the whip and this many steps after it lands, and only what its swept path (this many cells round) touches. The strike is aimed when the wind-up starts, at the target's spot within `reach`. An arrow in the bulb calls a wind-up off and doubles the recovery. |
| `pad` | [enemy.h](src/physics/enemy.h) | 40 | Under the troll's 56, so the renderer's atlas slots are unchanged. |

### Animation (`Species::rig`, see [src/physics/rig.h](src/physics/rig.h))

Angles are degrees, clockwise on screen with the body facing right: a positive
lean tips forward, a negative arm angle swings the hand forward. The pose moves
where pixels are drawn and hit, never what the body is made of.

| Knob | File | Now (ghoul / troll / fish) | What it does |
|---|---|---|---|
| `stride` | [enemy.h](src/physics/enemy.h) | 14 / 34 / 40 | Cells walked per full step cycle. The gait runs on distance covered, so the feet never skate. |
| `leg_swing` | [enemy.h](src/physics/enemy.h) | 28 / 20 / 24 | Each leg's angle at the far end of a stride. |
| `arm_swing` | [enemy.h](src/physics/enemy.h) | 24 / 8 / 6 | Arms swung against their own side's leg. |
| `bob` | [enemy.h](src/physics/enemy.h) | 1 / 1 / 1 | Cells the body sinks at the far end of a stride. |
| `chase_lean` | [enemy.h](src/physics/enemy.h) | 8 / 6 / 5 | Forward lean while chasing. |
| `chase_arms` | [enemy.h](src/physics/enemy.h) | -80 / 0 / 12 | Arms held out in front while chasing (0 = keep swinging). The fish's is clockwise from its rest pose: the lure held further out and down toward you. |
| `breathe_steps` / `breathe` | [enemy.h](src/physics/enemy.h) | 96, 3 / 150, 2 / 90, 5 | One breath every N steps, heaving the body and swaying the arms. |
| `raise` | [enemy.h](src/physics/enemy.h) | -160 / 150 / -130 | Where the attacking arm starts its stroke: over the head forward (ghoul), back over the shoulder (troll), back over the head toward the tail (fish). |
| `strike` | [enemy.h](src/physics/enemy.h) | 25 / -14 / 25 | Where the stroke ends. Always reached turning clockwise, over the top and down the front. |
| `strike_steps` | [enemy.h](src/physics/enemy.h) | 8 / 6 / 6 | Steps the stroke takes. For the troll and the fish, the last steps of the wind-up. |
| `windup_lean` / `strike_lean` | [enemy.h](src/physics/enemy.h) | 0, 10 / -8, 10 / -4, 14 | Leaning away into the wind-up, then over the blow. |
| `flinch_steps` / `flinch` | [enemy.h](src/physics/enemy.h) | 12, -14 / 12, -5 / 12, -6 | Rocking back when an arrow takes pixels. |
| `pad` | [enemy.h](src/physics/enemy.h) | 14 / 56 / 16 | How far a pose reaches outside the frame. `enemy_test` checks every pose stays inside it; `Enemy::MAX_POSE_PAD` ([enemy.h](src/physics/enemy.h)), the largest of these, sizes the renderer's atlas slots. |

## Bow and arrows

[src/physics/arrow.h](src/physics/arrow.h)

| Knob | File | Now | What it does |
|---|---|---|---|
| `LAUNCH_SPEED` | [arrow.h](src/physics/arrow.h) | 480 | Arrow speed at release, cells/s (8 cells a step). |
| `GRAVITY` | [arrow.h](src/physics/arrow.h) | 150 | Arrow drop. About 8 cells over a 160-cell shot. |
| `DRAW_STEPS` | [arrow.h](src/physics/arrow.h) | 20 | Steps between shots while `E` is held. |
| `STUCK_STEPS` | [arrow.h](src/physics/arrow.h) | 240 | How long a stuck arrow stays in a wall. |
| `BITE_RADIUS` | [arrow.h](src/physics/arrow.h) | 2 | Radius of body an arrow takes out where it lands. Asserted wider than an arm. |
| `FLUID_DRAG_PERCENT` | [arrow.h](src/physics/arrow.h) | 80 | Speed kept per step through water or oil. |

---

## History

Record adjustments to tuning parameters and their rationale here.

- **2026-09-20 — resynced with source, no values changed.** This file had drifted:
  it documented `WALL_SLIDE_SPEED`, `COYOTE_STEPS`, `JUMP_BUFFER_STEPS`,
  `FLAP_FALL_CANCEL`, `GLIDE_GRAVITY`, `CRUSH_PERCENT` and `COOLDOWN_STEPS`, none
  of which exist; stated `MAX_STEP_HEIGHT` as 2 (it is 3) and burn as 5/10 (it is
  2/6); and every linked line number was stale. The rows now match the code, and
  `tuning_test` fails the build's test run if a linked row names a constant that
  is not on its line, or a `player.h` row states a value the code does not hold.
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
  see the header of `assets/bg_tarn/backdrop.txt`. Existing bg_valley-family sets are unchanged.
- **2026-10-08, `bg_tarn_wide`.** A second rig set, 2752 cells wide, with no
  new values: it takes bg_tarn's rig (horizon 200, contact 264, strength 0.75),
  ripple 0.6, cloud drift -0.6 and foreground 1.30 so the two read as the same
  lake under the same camera. The rows above stay bg_tarn's; retuning one set
  does not retune the other, since each is its own backdrop.txt.
- **Troll added** (first-pass values). The slam's reach went 16 -> 20 and its
  crush radius 4 -> 5 after the preview showed a 4-radius crater sitting under
  the club's own head where nobody could see it. Wading was added after a troll
  that had its arm shot off stood buried in the pile of its own arm for the
  rest of the run, and then -- allowed to climb out -- perched on the cone's tip
  with its feet in the air.
- **Enemies animated** (first-pass values). Each species got a rig: limbs that
  turn about their joints, driven by distance walked, breathing, the chase, the
  attack and being hit. The troll's stroke first ran from `raise` (+150) back to
  `strike` (-14) the short way, which swung the club down the troll's own back;
  the stroke now always turns clockwise, over the top and down the front.
- **2026-10-07 -- rows link files, not lines; no values changed.** Every
  feature branch was re-pointing line numbers it had not meant to touch, and the
  three open at once conflicted on them. The `File` column now names the file,
  and `tuning_test` checks that the file still declares the constant.
- **2026-10-07 -- one backdrop system; depth-grading rows removed.** Backdrops
  are now `assets/<dir>/backdrop.txt` files drawn by one rig-based path
  (`src/render/backdrop_set.h`). The generated sky/mountains/ground backdrop went
  with it, and so did its knobs: the `mountains` (0.60) and `ground` (0.53)
  grades, `PLANE_TEXEL_SCALE`, `GROUND_STRIPS` and the surface-plane pass's
  `SKIN_CELLS` / `FULL_END` / `DEPTH_END` / `PLANE_FADE_END_T`. Its only user was
  the `empty` debug scene, which now draws the clear colour. The `bg_tarn` rows
  moved to its backdrop.txt with the same values; bg_valley-family factors and bands
  are unchanged.
- **2026-10-08 -- fish added** (first-pass values). A largemouth bass on human
  legs with a lure arm out of the top of its head, a fifth taller than the troll
  (84-row frame against 70). Its attack is a cast built on the slam's rhythm --
  wind-up, blow, recovery -- so a blow from a body this size is telegraphed, but
  with no crater (`crush_radius` 0) because it is a hand. The cast's `raise`
  went from -80 to -130 after the first preview: at -80 the arm only lifted
  over the snout, which read as a wave, not a wind-up.
- **2026-10-12 -- liquids hold amounts.** A liquid cell holds an amount and a
  sideways velocity, and water moves by amounts flowing between cells rather
  than by cells moving. The old rules -- venting a sinking grain's water to a
  nearby surface (`vent_radius` 3), moving whole cells between the arms of a
  body (`MAX_PRESSURE_CELLS` 512, `MIN_PRESSURE_HEAD` 2) -- are gone; the last
  survives as `LIQUID_LEVEL_*`, moving half a difference instead of a cell. A
  compressible model was tried first and dropped: at 3% a cell a U-tube took a
  thousand steps to level and a lake painted 339 cells deep settled to 118.
  `LIQUID_ACCEL` and `LIQUID_DAMP` are the strongest pair that stayed stable in
  `fluid_probe` and grid_test. The cost is the `churning` bench row, 21.7 ->
  35.0 ms: a grain sinking into water presses it into the pool around it, which
  is what stops a pour lifting water out of the pool.
- **2026-10-17 -- angler added** (experimental, first-pass values). The fish's
  body on tentacles, as a first try at limbs that bend along their length --
  FABRIK and a travelling wave, after the Smitner-Studio tentacle-arm-showcase,
  in fixed point. Three things found by rendering it: the lure's rest hook
  needed `link_of` to measure distance rather than cut by bisectors (the bulb was
  given to the first link); a slack chain folded into zigzags until `smoothing`
  and `max bend` went in; and `stride` came down from 32 to 24, because the first
  stance after standing still dragged a foot past the tentacle's reach.
- **2026-10-17 -- angler: feet step when they need to, the bulb is the hitbox.**
  First pass of the procedural-animation plan, judged in `angler_probe` frames.
  The lure's spring became a second-order follower with the same motion
  (`lure follow`), and the same follower carries the body on its feet (`body
  follow`, `sink / tilt`, `footfall dip`). Feet now step on distance from a
  led home with a ground search (`swing steps`, `step / settle trigger`, `step
  lead`, `foot probe`) instead of on the `stride` cycle. `bow` went from 1/3 to
  1: at 1/3 a near-straight front leg could not buckle toward a foot swung in
  closer, missed its goal by six cells and was drawn never lifting. The strike
  now hurts only through the bulb's swept path (`lure live / bulb radius`),
  aimed when the wind-up starts, with a hold, a rolled bend and a thrash
  (`lure hold`, `crack / flourish`); before, a box test at the last wind-up step
  hit whatever stood in reach, wherever the bulb came down.
- **2026-10-17 -- water slightly less viscous.** A playtest found water too
  thick. `LIQUID_ACCEL` and `LIQUID_DAMP` were already at their stability limit,
  and the speed ceiling was not water's but the velocity byte's: at
  `LIQUID_FLOW_SCALE` 8 it topped out at 1016 units a step, under water's 1280.
  The scale went to 16 and water's spread from 5 to 6 (ceiling 1536, spreading
  share 1/4 -> 1/3). In `fluid_probe`'s dam break the front reaches the far wall
  in 232 steps instead of 265, the tank settles in 778 instead of 1020, and two
  waves now reach the far wall where none did. Spread 7 and 8 were tried: faster
  still (223, 217) but sloshier, settling in 885 and 1226. Oil is unchanged.
- **2026-10-17 -- the tarn scenes zoomed out, scale 10 -> 8.** A playtest found
  `bg_tarn` and `bg_tarn_wide` framed too close. At 8 a 1080p window shows 240x135
  cells instead of 192x108, and a player standing on the shore sees 58 rows above
  the horizon instead of 36. No rig value moved: a taller window has less room to
  climb, so the plane stretches less at the ceiling (2.7x at strength 0.75, was
  2.9x), inside what was judged when 0.75 was picked. 8 is as far out as the art goes:
  at 7 the 688-wide hills tile in `bg_tarn_wide` would show twice on a 3440
  screen. The valley and forest scenes stay at 10.
- **2026-10-17 -- loose bodies replace toppling.** `TIP_GRAVITY` and
  `TIP_MAX_STEPS` are gone with the hinge they tuned. A piece that topples is
  now a rigid body with momentum, friction and restitution, so it flies off a
  ledge in an arc, tumbles, bounces and knocks other pieces over. `BODY_GRAVITY`
  keeps the 1/4 cell/step² both shared. `BODY_MAX_STEPS` is 1800 against the old
  600, since bodies now end by stopping and the cap is only a backstop.
  Materials gained `weight`, `grip` and `bounce` columns, from real densities.
  `LIQUID_DRAG` went 20 -> 48 -> 96 in the body probe's float scene: below 96 a
  wooden block never stopped bobbing. `BODY_SOLVER_PASSES` went 8 -> 32 for the
  stack scene, as above.
- **2026-10-17 -- falling is a body too; less ideal.** A playtest found falls
  5-10% too fast and sudden, momentum missing, balance too ideal and motion
  mathematical. Every unsupported piece is now a body, so it accelerates from
  rest instead of lurching a cell and climbing a staircase of speeds.
  `BODY_GRAVITY` 1/4 -> 9/40; new `AIR_DRAG` 1/250 eases a fall toward its
  top speed instead of a hard cap. `GRIP_ROUGHNESS` 20, `NUDGE_SPIN` 1/1500 and
  `BALANCE_MARGIN` 1/2 take the perfection out of surfaces, starts and
  balance. Overhangs snap by bending strength (material `strength`,
  `IMPACT_LOAD_MAX` 6), which replaces fracture-on-landing for bodies.
  `WARM_START` 1/4 added for stacks.
