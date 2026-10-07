#pragma once
#include "element.h"
#include "random.h"
#include "fixed_trig.h"
#include <vector>
#include <cstdint>

class Grid {
public:
    // Side length of one simulation chunk, in cells. Each chunk tracks the bounds
    // of the cells inside it that might still move, and a chunk with nothing moving
    // is skipped entirely.
    //
    // Swept over 16/32/64/128 with tests/bench_grid.cpp: the active scenarios are
    // identical to within run-to-run noise and the sleeping ones get slightly
    // cheaper as the chunk grows. 64 is the largest size that still gives useful
    // culling granularity when several small things move in different parts of the
    // world.
    static constexpr int CHUNK_SIZE = 64;

    // Largest connected structure the support check will judge. Past this, the
    // structure is assumed supported and left alone.
    //
    // The asymmetry is deliberate. A missed collapse is invisible -- a slab that
    // should have fallen simply does not. A wrong collapse turns a level into
    // rubble. When the answer is too expensive to compute, guess the harmless way.
    static constexpr int MAX_SUPPORT_CELLS = 4096;

    // A falling structure speeds up. It moves one cell on the step it comes loose
    // and gains a cell per step for every TICKS_PER_SPEEDUP steps it stays in the
    // air, up to MAX_FALL_SPEED.
    //
    // Speed is repetition, not a longer stride: a piece never travels a cell
    // without first re-deriving what is holding it up, so moving at 8 costs eight
    // flood fills in a step. That is why there is a ceiling. It also means a fast
    // piece cannot pass through a floor thinner than its speed, which a "move N
    // cells then look" implementation would have to guard against explicitly.
    //
    // Read against 1 cell = 1 cm: at 60 Hz, +1 cell/step every 4 steps is ~15 m/s^2
    // with a ceiling of 4.8 m/s, reached in about half a second.
    static constexpr int MAX_FALL_SPEED = 8;
    static constexpr int TICKS_PER_SPEEDUP = 4;

    // Cells per step for a piece that has been falling for `ticks` steps.
    static int fall_speed(uint8_t ticks);

    // Used when a caller does not supply a seed. Any fixed value would do; the
    // point is that it is fixed, so a test is reproducible without naming a seed it
    // does not care about.
    static constexpr uint64_t DEFAULT_SEED = 1;

    // The grid takes its seed rather than finding one. Nothing here reads the clock
    // or the OS entropy pool, so a given (seed, sequence of writes, number of
    // steps) always produces the same world. Deciding where the seed comes from is
    // the caller's job.
    Grid(int width, int height, uint64_t seed = DEFAULT_SEED);
    ~Grid() = default;

    // Puts the grid back to exactly the state a fresh Grid(width, height, seed)
    // would be in, without reallocating cells/pixels/the chunk and support vectors.
    // Written as an explicit member-by-member wipe rather than assignment from a
    // temporary so that the test for "a field left out of the wipe" has something
    // to catch.
    //
    // Takes a seed rather than keeping the old one, because the seed has to be one
    // of the things this clears for "reset run == fresh run with the same seed" to
    // mean anything.
    //
    // One exception: vent_radius survives a reset, because it is configuration
    // rather than world state. See the end of reset()'s definition.
    void reset(uint64_t seed);

    // Advances the simulation by exactly one fixed step. The caller is responsible
    // for calling this at a fixed rate, not once per rendered frame.
    void update();

    void set_element(int x, int y, ElementType type);
    void paint(int x, int y, ElementType type, uint32_t color);

    // The brush's write path, and the only one that conserves what was already
    // standing in the cell. set_element overwrites, which is right for every other
    // caller -- a reaction turning Wood into Fire means the wood is gone, worldgen
    // writes into a grid nobody has filled yet, and the eraser's job is deletion --
    // but wrong for a player pushing sand into a pool, where it would delete the
    // water rather than move it.
    //
    // Displacement only, not a general "move this cell somewhere sensible": the
    // occupant goes straight up, because up is where a fluid's own surface is and
    // raising a level is what displacing volume looks like. See make_room_above for
    // what it will and will not push through.
    void displace(int x, int y, ElementType type);
    Element get_element(int x, int y) const;

    // Raw pixel colours for rendering to an SDL texture.
    const std::vector<uint32_t>& get_pixels() const { return pixels; }

    int get_width() const { return width; }
    int get_height() const { return height; }

    // Readable so it can be written to a save file, shown to a player, or quoted in
    // a bug report. Reproducing a run needs the number, not just the guarantee that
    // one exists.
    uint64_t seed() const { return world_seed; }

    // Steps completed since construction and, while a step is in progress, the
    // 1-based index of that step. Readable for the same reason the seed is: a save
    // file needs both numbers to say where a run had got to.
    uint64_t steps() const { return step_count; }

    // Number of chunks that will be swept on the next step. Exposed for tests and
    // for the on-screen diagnostic, so "is anything actually sleeping?" is
    // observable rather than assumed.
    //
    // Zero does not mean the world has come to rest. A falling structural piece is
    // carried by pending_support, and resolve_support() runs before update() swaps
    // the chunk rects, so its writes mark the set that is about to become this
    // step's work while the sweep adds nothing of its own. A slab can fall the
    // whole height of the world with this reading zero the entire way.
    //
    // At rest is this AND has_pending_support_checks(); code asking whether the
    // world has settled has to ask both.
    int active_chunk_count() const;

    // Whether the chunk containing (x, y) is one of them. Out of bounds is false --
    // there is no chunk there to be awake.
    //
    // Read off the same array active_chunk_count() counts, so the two can never
    // disagree about what awake means, and answering for the next step rather than
    // the last one for the same reason it does: what a debugger wants to know is
    // whether this pile is still going to move.
    bool chunk_awake_at(int x, int y) const;

    // Whether a structure removal is still waiting to be re-examined by
    // resolve_support(). Exposed for the same reason active_chunk_count() is: it is
    // the one piece of internal state a reset test needs to see to prove the queue
    // was cleared rather than merely idle.
    //
    // A piece tipping over its edge counts as one still waiting: it is a structure
    // in motion exactly as a falling one is, and every caller of this asks "has the
    // building stopped moving yet", which a half-toppled tower has not.
    bool has_pending_support_checks() const { return !pending_support.empty() || tip_count > 0; }

    // How many pieces are pivoting over an edge right now. Exposed for tests and
    // the diagnostic for the same reason active_chunk_count() is.
    int tipping_count() const { return tip_count; }

    // How far vent_fluid searches for somewhere to put displaced fluid. See
    // vent_radius in the private section for what the number means; this trio
    // exists so the sweep that priced it can be re-run.
    //
    // Settable at runtime because the only other way to price it is a rebuild, and
    // a rebuild moves the compiler's layout of the hot loop along with the value
    // under test. One binary, one sitting, the same input stream, with the radius
    // moved between runs is the only reading this project accepts for a cost knob.
    //
    // This is configuration, not world state, which is why reset() leaves it alone
    // and nothing derives it from the seed. A grid at a non-default radius is still
    // fully deterministic, but two grids at different radii are different
    // simulations that diverge from identical inputs -- so a replay at a
    // non-default radius reports a different end state, and that is not evidence of
    // a stale log.
    //
    // Public so a test and the bench can name the shipped value rather than writing
    // the number and quietly disagreeing with this header later.
    static constexpr int DEFAULT_VENT_RADIUS = 3;

    int get_vent_radius() const { return vent_radius; }

    // Negatives clamp to zero, and zero is a real setting: at r=0 the search box is
    // the fluid's own cell, which is never Empty, so vent_fluid always returns false
    // and every caller falls back to the plain swap. That is venting off, on the
    // same instrument as every other row of the sweep.
    void set_vent_radius(int r) { vent_radius = r > 0 ? r : 0; }

    // The other two displacement rules, switchable by the same argument as the
    // radius above: pricing a rule needs it turned off as well as on.
    //
    // Venting, seek_level and the brush's make_room_above are the displacement
    // machinery, and a whole-step timing cannot say how much of itself is any one
    // of them. Ablation can, without a rebuild per data point.
    //
    // Gated at the call site rather than inside the function, so an ablated rule
    // costs a predictable branch and not a call. Both default on, so a Grid nobody
    // configures runs the shipped simulation.
    //
    // Turning one off does not merely remove its cost, it changes the simulation,
    // so every later step does different work and the world diverges. That is
    // unavoidable in an ablation and is why these numbers are a share of a scenario
    // rather than a subtraction.
    bool seek_level_enabled() const { return seek_level_on; }
    void set_seek_level_enabled(bool on) { seek_level_on = on; }

    bool room_above_enabled() const { return room_above_on; }
    void set_room_above_enabled(bool on) { room_above_on = on; }

private:
    // Bounds of the cells within one chunk that may still move, in world
    // coordinates, inclusive at both ends. max < min means the chunk is asleep.
    struct DirtyRect {
        int min_x = 0, min_y = 0, max_x = -1, max_y = -1;

        bool is_empty() const { return max_x < min_x; }
        void clear() { min_x = 0; min_y = 0; max_x = -1; max_y = -1; }

        void include(int x, int y) {
            if (is_empty()) {
                min_x = max_x = x;
                min_y = max_y = y;
                return;
            }
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    };

    int width;
    int height;

    int chunks_x;
    int chunks_y;

    // Two sets of bounds, not one. Growing the rect currently being iterated would
    // change the loop bounds underneath the loop; instead every write during a step
    // lands in chunk_next, which is swapped in at the start of the following step.
    std::vector<DirtyRect> chunk_current;
    std::vector<DirtyRect> chunk_next;

    // Wakes the cell at (x, y) and its neighbours for the next step.
    void mark_dirty(int x, int y);

    // Two arrays in step: one for the physics logic (elements) and one raw colour
    // buffer (pixels) that SDL reads directly.
    std::vector<Element> cells;
    std::vector<uint32_t> pixels;

    bool is_within_bounds(int x, int y) const;
    int get_index(int x, int y) const;
    void swap_elements(int x1, int y1, int x2, int y2);

    // The body shared by set_element and paint once each has resolved its own
    // colour and confirmed the write is in bounds. See the definition for why
    // this is one function rather than two copies.
    void place(int x, int y, ElementType type, uint32_t color);

    // Moves the fluid at (fx, fy) to the nearest free surface of its own kind, so a
    // powder sinking into it does not have to hand it upwards. Returns false when
    // no such surface is within vent_radius, in which case the caller keeps the
    // plain swap.
    bool vent_fluid(int fx, int fy);

    // How far vent_fluid will look for a surface. This is the expensive part of the
    // powder step and it runs on every powder/fluid contact, so the radius is a
    // direct cost knob and was swept rather than picked.
    //
    // Measured one radius per run of a single binary, the cost is dead linear in
    // the area of the search box -- about 0.063 ms per extra cell scanned at every
    // radius, with no knee. The value is therefore chosen on quality: the gap
    // between r=3 and r=4 in water_probe is a transient that drains on brush
    // release, which is not worth 27% more scan.
    //
    // Runtime rather than constexpr so the sweep can be re-run without a rebuild;
    // the default is the whole of the behaviour, so a Grid nobody configures runs
    // the shipped simulation. The conversion is not free: churning, the only bench
    // scenario containing water, costs about a third more at both world sizes while
    // every control moves by under 2%. On a played session that lands inside the
    // noise, because the cost falls entirely on powder-into-fluid exchange and a
    // played world has little. Anything that makes powder livelier would put that
    // share back on the hot path.
    int vent_radius = DEFAULT_VENT_RADIUS;

    // The other two ablation switches. Configuration like vent_radius, and
    // surviving reset() for the same reason. Accessors are in the public section.
    bool seek_level_on = true;
    bool room_above_on = true;

    // Lifts the movable cell at (x, y) to the first Empty cell above it, so the
    // caller can write into (x, y) without deleting what was there. Returns false
    // when there is nowhere to put it, which is a real answer rather than a
    // failure: a sealed pool with no air above it has no room, and the caller
    // overwrites rather than refusing the stroke.
    //
    // The walk climbs through fluid and stops dead at anything solid. Climbing
    // through makes one call cost the depth of the column rather than a search of
    // the area around it, and it is the right physics: the column is one body, so
    // swapping the bottom cell with the Empty at the top raises the whole surface
    // by one. It stops at solids because a lid means the volume has nowhere to go.
    bool make_room_above(int x, int y);

    // How far up make_room_above will look. Bounds the worst case: a brush dragged
    // along the floor of a deep pool pays this per painted cell per step. Past it
    // the volume is treated as having nowhere to go, which is the answer a lid
    // gives and fails in the same visible direction.
    static constexpr int MAX_DISPLACE_RISE = 32;

    // Runs the one cell at (x, y) through its material's behaviour.
    void step_cell(int x, int y);

    // Incremented once per step and stamped onto every cell visited. Wraps at 256,
    // which is harmless: a cell asleep for an exact multiple of 256 steps is
    // skipped for a single step and runs the next one.
    uint8_t frame_tag = 0;

    // The simulation's clock. Distinct from frame_tag above despite both being
    // counters, and the two must not be merged.
    //
    // frame_tag is a skip flag -- its only question is whether this cell has
    // already moved this step -- and one byte is the right size for it. This is a
    // clock and feeds the randomness hash, where a byte would be actively wrong:
    // randomness for a given cell would repeat every 256 steps, which is visible
    // periodicity rather than noise.
    //
    // Incremented at the top of update(), before resolve_support(), so everything
    // within one step agrees on which step it is. frame_tag deliberately does not
    // move up with it: it is stamped after support resolves, so cells a falling
    // structure just moved are not marked as already updated and still get their
    // turn in the sweep.
    uint64_t step_count = 0;

    // True if the cell at (x, y) may move into (tx, ty). Vertical moves are allowed
    // only when gravitationally favourable: a mover travelling down must be denser
    // than its target, one travelling up must be lighter.
    bool can_displace(const Material& mover, int tx, int ty, int dy) const;

    // Per-behaviour steps. Each returns true if the cell moved.
    bool step_powder(int x, int y, const Material& mat);
    bool step_fluid(int x, int y, const Material& mat, int dy);

    // --- liquids find their level ---
    //
    // can_displace refuses every upward move unless the mover is lighter than its
    // target, and Empty has density 0, so a liquid can never rise. It falls and
    // spreads sideways into Empty, which is a powder that happens to flow: a U-bend
    // cannot equalise, because the short arm has no way to gain a cell.
    //
    // The rule below moves the tall arm's surface cell down onto the short arm's
    // surface rather than making the short arm rise. Rising is a swap, so it leaves
    // a bubble of Empty inside the body, and the transfer is only finished once the
    // ordinary fall and spread rules have walked that bubble back down the arm and
    // up the far side. During those steps the bubble cuts the body in two, the
    // search transiently answers "no head", the cells stop marking themselves dirty,
    // and the chunk sleeps with the tube parked out of level. Every fix for that
    // keeps a body awake while it is unlevel, which is a standing cost paid by every
    // pool in the world.
    //
    // Moving the tall cell makes each transfer a single atomic swap, so there is no
    // journey to stay awake for and the wake-up is local: the vacated cell's 3x3
    // mark is exactly the cell below it, which is the next one to transfer. A body
    // equalises at one cell per step and then sleeps.
    //
    // The visible cost is that a cell can travel further in one step than a cell
    // should. It is bounded by the search below, happens only between two points of
    // one connected body of the same liquid, and looks like one side dropping while
    // the other rises. Conservation is what keeps it honest: the obvious way to make
    // water level is to invent some.

    // How far the connected-liquid search may look before giving up. Bounded
    // because a settled pool's every surface cell asks this question on every step
    // it is awake, so the cost has to be a constant rather than the size of the
    // pool. Giving up early is the harmless direction: a body wider than this
    // equalises in several hops instead of one.
    //
    // The number reads shorter than it is. The search is breadth-first through the
    // body, not along its surface, and spends its budget in both directions at
    // once, so in a pool five cells deep it buys roughly six columns of reach
    // either way.
    //
    // It has to carry level transport on its own. The old lateral spread rule let a
    // surface cell walk sideways into any Empty at all, which jittered forever on a
    // flat surface but also carried cells off the top of a mound out to the thin
    // edges of a pool for free. Refusing the pointless half refused the useful half
    // too, and a poured column settled into a permanent heap.
    static constexpr int MAX_PRESSURE_CELLS = 512;

    // How much lower the receiving surface has to be before a cell will move to it,
    // in cells. Two, not one, and that is hysteresis rather than a tuning
    // preference: each transfer moves the two surfaces one cell towards each other,
    // so at a threshold of one a body one cell out of level would swap which side
    // was high, forever. Two settles, at the cost of level meaning level to within
    // a cell.
    static constexpr int MIN_PRESSURE_HEAD = 2;

    // Searches the body of `type` connected to (x, y) for a surface cell -- one
    // with Empty directly above it -- at least MIN_PRESSURE_HEAD rows lower.
    // Returns its index, or -1. Breadth-first and orthogonally connected:
    // nearest-first makes the cap bite evenly in every direction rather than all
    // down one arm, and a diagonal step would let two pools that merely touch at a
    // corner equalise into each other.
    int find_lower_surface(int x, int y, ElementType type);

    // The move itself: one cell from (x, y) onto whatever lower surface the search
    // found. Returns true if the cell moved.
    bool seek_level(int x, int y);

    std::vector<int> pressure_queue;     // scratch, reused across searches

    // A per-cell "seen this pass" marker, on the same epoch trick as support_visit,
    // and shared by two unrelated searches -- which is why it is named for what it
    // is rather than for either of them.
    //
    // The two never overlap, for an ordering reason rather than a local one:
    // fracture_landing() runs only inside resolve_support(), find_lower_surface()
    // only inside the cell sweep, and support resolves in full before the sweep
    // starts. They also advance the epoch on different schedules -- once per
    // support pass, so one landing costs one fill, and once per call, because two
    // adjacent surface cells are asking genuinely different questions.
    //
    // Nothing enforces that invariant, so a third user is not free. Anything
    // wanting these marks from inside both the sweep and support resolution, or
    // adding a pass between the two, needs its own array.
    std::vector<uint8_t> scratch_visit;
    uint8_t scratch_epoch = 0;

    // --- heat ---
    //
    // Heat stays a number on a cell that flows downhill and nothing more: no
    // energy, no mass, no phase state, no second pass over the world. It rides the
    // existing sweep, one visit per awake cell, and it reaches equilibrium and
    // stops, which is the property everything else here depends on.
    //
    // Integer arithmetic only. Floating-point diffusion would put cross-platform
    // nondeterminism back into Grid.

    // The two rates, as reciprocals: heat moved per step is
    // (difference * conductivity) / divisor. Conduction between neighbours is eight
    // times faster than the bleed back to ambient, which is what lets a flame push
    // heat into a wall faster than the wall can shed it -- the other way round and
    // nothing would ever get hot enough to ignite.
    static constexpr int CONDUCTION_DIVISOR = 1024;
    static constexpr int AMBIENT_DIVISOR = 8192;

    // Runs conduction, the ambient bleed and the heat-source clamp for one cell,
    // and marks it dirty if any of the three changed a temperature. Returns whether
    // anything changed.
    //
    // A cell whose temperature is still changing has to stay awake, and that mark
    // is the whole of how: heat that diffuses into a sleeping chunk and stops is
    // the same bug as material frozen in mid-air. What makes it terminate rather
    // than keeping the world awake is the dead band in heat_flow -- two neighbours
    // within one degree exchange nothing, so a warm world settles and sleeps
    // instead of trading a unit back and forth forever.
    bool step_thermal(int x, int y, const Material& mat);

    // True if any of the 8 cells surrounding (x, y) is of type t. Reads cells[]
    // directly rather than going through get_element(), which treats out-of-bounds
    // as Wall -- correct for physics sealing, wrong here, since it would make every
    // world edge act as a Wall catalyst.
    bool has_neighbor(int x, int y, ElementType t) const;

    // Checks (x, y) against the REACTIONS table and converts it via set_element()
    // on success. Returns true if the cell was converted, in which case step_cell
    // skips movement for it this frame.
    bool try_react(int x, int y);

    // How many steps a flame lives, and the only lifetime in the engine that is a
    // countdown rather than a decay chance.
    //
    // The reason is the colour ramp: a flame is drawn from white-hot at its source
    // through orange to dim red as it dies, which needs the cell to know how old it
    // is, and a dice roll has no age to read. Fire is a Gas and never structural,
    // so it is the one material that can spend Element::ticks on this without
    // colliding with the free-fall clock.
    //
    // Around a fifth of a second, which is short on purpose: reference footage of a
    // burning scene replaces the entire contents of a flame between frames roughly
    // 10-20 steps apart, while the fuel underneath takes seconds to be consumed.
    //
    // It is a range rather than a constant. A flame that rose exactly one cell per
    // step and lived exactly N steps died exactly N cells above its fuel, giving a
    // razor-straight line across the top of a fire. Real flames do not all live
    // equally long, and nothing about how flames move had to change to fix it.
    static constexpr uint8_t FLAME_LIFETIME_MEAN = 13;

    // Half-width of the jitter, so a flame lives 8-18 steps. Wide enough that the
    // top of a fire is visibly ragged rather than merely soft, and bounded so the
    // longest-lived flame is still decoration rather than something that outlives
    // what threw it.
    static constexpr int FLAME_LIFETIME_SPREAD = 5;
    static constexpr uint8_t FLAME_LIFETIME_MAX =
        static_cast<uint8_t>(FLAME_LIFETIME_MEAN + FLAME_LIFETIME_SPREAD);

    // Flames rise on 9 steps out of 10. A gas moves a whole cell or none at all, so
    // 0.9 cells per step cannot be a smaller step -- it has to be a skipped one.
    // Rolled per cell per step rather than counted, because a flame has no spare
    // byte to count in: Element::ticks is its lifetime. The roll is drawn from the
    // deterministic stream, so this stays reproducible from the seed.
    //
    // The skip is per cell, so neighbouring flames fall out of step with each other
    // and the flat rising front breaks up as a side effect.
    static constexpr int FLAME_RISE_SKIP_PERCENT = 10;

    // Throws a flame from a burning cell into a randomly chosen empty neighbour, as
    // named by the `emits` column. Returns true if one was placed.
    //
    // This is the mechanism that makes flame a separate thing from fuel. A burning
    // cell stays put, keeps its structural role and heats what it touches; what the
    // player sees rising off it is manufactured here and dead within
    // FLAME_LIFETIME steps.
    bool emit_flame(int x, int y, const Material& mat);

    // Ages a flame by one step, ramps its colour to match, and kills it at zero.
    // Returns true if the cell is gone, in which case the caller must not go on to
    // move it.
    bool step_fire(int x, int y);

    // --- steam's condensation clock ---
    //
    // Steam's lifetime is a countdown on Element::ticks rather than the span
    // between its spawn temperature and a condensing point in REACTIONS. Those two
    // numbers cannot both be free: spawn temperature is pinned low by the
    // static_assert at the bottom of reaction.h, since steam hotter than the
    // coldest ignition point in REACTIONS is a fire-starter, so the only end left
    // to move was the condensing point, and moving it up shortens the life further.
    //
    // The result was that a puff lasted about a second in mid-air and far less
    // against stone, because Empty conducts nothing while a ceiling takes the heat
    // straight out of it -- so the one place steam is supposed to gather was the
    // one place it could not survive. A countdown decouples the two: spawn
    // temperature goes back to being only about heat, and how long a puff lasts can
    // be tuned without touching the ignition floor.

    // Steps of contact with something solid a steam cell lasts before it condenses.
    //
    // Contact rather than elapsed time, which is the rule rather than a tuning of
    // it: steam condenses on the cold surface it touches, not in the middle of its
    // own pocket, so a cell with nothing but more steam above it does not age at
    // all. Three things follow, none of them implemented anywhere:
    //
    //   - A pocket drains from the top down and takes as long as it is deep, so a
    //     big pocket collects and waits where a wisp does not.
    //   - The bulk of a pocket can sleep. A cell that is neither moving nor ageing
    //     generates no dirty marks; when the cell above it condenses, set_element
    //     wakes the 3x3 and it rises into contact.
    //   - Nothing gets stranded, by induction: a steam cell can only be blocked by
    //     something solid or by more steam, because it is lighter than everything
    //     else in the table including Empty, so can_displace always lets it rise.
    //     Follow a blocked column upwards and it ends at a solid or at the world's
    //     top edge, which get_element answers as Wall. The top of every column is
    //     therefore ageing, and each cell it clears wakes the one beneath.
    //
    // Bounded above by 255: `ticks` is one byte, so no lifetime in this engine can
    // exceed 4.25 seconds at 60 Hz. Anything materially longer needs a coarser tick,
    // not a bigger constant.
    static constexpr uint8_t STEAM_LIFETIME_MEAN = 200;

    // Half-width of the jitter, so a puff's cells live 160-240 steps. Wide for the
    // same reason FLAME_LIFETIME_SPREAD is: cells placed together in one stroke
    // would otherwise condense in lockstep, and a pocket that vanishes all at once
    // is the behaviour being fixed. 240 stays clear of the byte.
    static constexpr int STEAM_LIFETIME_SPREAD = 40;
    static_assert(STEAM_LIFETIME_MEAN + STEAM_LIFETIME_SPREAD <= 255,
                  "a steam lifetime would wrap Element::ticks and start again");

    // Where the drip comes from, and no code below writes a drop. A contact cell
    // that runs out becomes Water in place, and Water is denser than the steam
    // under it, so can_displace carries it down through the pocket and onto the
    // floor. Writing the drop into the cell below by hand would be a fourth write
    // path reimplementing a decision the density rule already makes, and would have
    // to answer what happens when that cell is not Empty.
    //
    // Drip rate scaling with pocket size is likewise not implemented: a wider
    // pocket has more cells against the ceiling, each on its own clock, so it drips
    // faster than a small one and slows as it drains.

    // Ages a steam cell by one step if it is touching something solid, and
    // condenses it to Water at zero. Returns true if the cell is no longer steam,
    // in which case the caller must not go on to move it -- the same contract as
    // step_fire.
    bool step_steam(int x, int y);

    // --- structures and falling ---
    //
    // Static materials hold their shape, which means they can also hold it
    // somewhere they have no business holding it: dig the ground out from under a
    // stone slab and it hangs in mid-air, while sand next to it falls correctly.
    //
    // An unsupported structure falls as one rigid piece, keeping its shape the
    // whole way down, rather than being converted into loose grains -- the shape is
    // what makes it read as masonry rather than gravel. The piece stays in the cell
    // grid while it falls, which is what keeps rendering, player collision and
    // digging working on it unchanged: it is a rigid body only in how it moves, not
    // in where it lives.
    //
    // Support is checked on disturbance only, never as a global truth. A sweep of
    // the whole world every step would cost more than the simulation it is attached
    // to, and a world as authored is assumed to be standing up on purpose. So a
    // structure nobody has touched is never questioned; the moment part of one is
    // removed, what was leaning on it gets re-examined.

    // --- fracture ---
    //
    // drop_component translates an unsupported piece straight down with its shape
    // intact, so masonry descends like an elevator. Fracture was originally the
    // whole answer, on the grounds that rotation on a cell grid means resampling
    // the piece and destroying its authored pixels. Rotation by shears does not
    // resample, so pieces now tip as well (see "toppling" below); fracture keeps
    // the landings where most of the weight is over solid ground, and toppling
    // takes the ones where most of it is not.
    //
    // A piece breaks when it lands, not while it falls, and that timing is what
    // makes the feature safe. Every "nothing must move here" test in the suite is
    // about a piece at rest, and a piece at rest has `ticks` of zero and never
    // reaches this code. Fracture cannot start a collapse; it can only let one that
    // was already happening finish unevenly. Tying the trigger to landing speed is
    // how the missed-collapse-over-wrong-collapse asymmetry is kept by construction
    // rather than by care.
    //
    // Splitting a falling piece would have been a no-op: break one in mid-air into
    // two and both halves are unsupported, so both fall by exactly one cell on
    // exactly the same steps. Worse, the fill re-discovers them as one component
    // next step, because they are still adjacent. Fracture without persistent state
    // is impossible on this representation -- the crack has to survive into the
    // next fill or it never had any effect -- which is what Element::piece_tag is.

    // Smallest piece worth breaking. Below this, a break produces two fragments
    // small enough to read as gravel rather than as masonry coming apart. It also
    // terminates the recursion in practice: each break halves the piece, so a slab
    // fragments a bounded number of times and then stops being eligible.
    static constexpr int MIN_FRACTURE_CELLS = 20;

    // How long a piece must have been falling before landing can break it. Four
    // steps is one TICKS_PER_SPEEDUP interval, so this reads as "it was moving
    // faster than the speed it comes loose at". A piece that tips off a ledge and
    // drops one cell settles intact; one that has been accelerating breaks.
    static constexpr int FRACTURE_MIN_TICKS = 4;

    // Splits the piece that has just landed at (x, y) along a vertical seam, giving
    // everything on one side a fresh tag. Runs its own flood fill rather than
    // reusing the one in progress: the fill that detected the landing stopped at
    // the first grounded cell and holds only a partial trail, and a partial piece
    // is exactly the input that would put a crack in the wrong place. Costs a full
    // fill, once, per landing.
    void fracture_landing(int x, int y);

    // Tags handed out by fracture. Starts at 1 because 0 means never broken, and
    // wraps past 255 back to 1 rather than to 0 -- see Element::piece_tag for what
    // a collision costs.
    uint8_t next_piece_tag = 1;

    // Hands out the next tag, skipping any that a piece still in the air is
    // wearing. A bare counter suffices only while 255 landings cannot happen inside
    // one piece's flight; a long cascade makes that reachable, and the collision
    // does not look like a bug -- it silently welds two unrelated bodies into one
    // piece. Deterministic: it reads only pending_support and the counter, both
    // part of the simulation state.
    uint8_t alloc_piece_tag();

    std::vector<int> fracture_component; // scratch, reused across breaks

    // Column-lowest footprint scratch for fracture_landing(). A member rather than
    // a local for the same reason as fracture_component: a slab landing re-enters
    // this per piece, on the one step where the most work is already happening, and
    // a fresh vector per call is a heap round trip on the simulation's critical
    // path. Safe as a member because fracture_landing() is not re-entrant -- it
    // runs only from fall_if_unsupported(), which runs only from resolve_support(),
    // and neither recurses.
    std::vector<int> fracture_lowest;   // scratch, reused across breaks

    // True if (x, y) is held up from directly below -- by the floor of the world, or
    // by something solid that is not part of the same structure. Powders bear load;
    // liquids and gases do not.
    bool is_grounded(int x, int y) const;

    // Adds (x, y) to the list to re-examine, if it is a structure cell at all.
    void queue_support_check(int x, int y);

    // Works through that list. Called once per step, before the sweep, and
    // internally runs up to MAX_FALL_SPEED passes over it -- one cell of travel
    // each -- so that pieces already up to speed get further in the step than
    // ones that have just come loose.
    void resolve_support();

    // Flood-fills the structure containing (x, y). If no cell in it is grounded,
    // the whole piece moves down exactly one cell, shape intact.
    void fall_if_unsupported(int x, int y);

    // Translates the piece currently in support_component down by one cell and
    // re-queues it so it keeps falling on the next step.
    void drop_component();

    // What a fill concluded about a cell. Only meaningful while that cell's
    // support_visit stamp holds the current epoch.
    //
    // Recording the conclusion and not merely "seen" is load-bearing. A fill stops
    // the instant it finds one grounded cell, so it leaves a partial trail of marks
    // across a piece it has just decided is held up. Without a reason attached, a
    // later seed from that same piece starts its own fill, cannot cross that trail,
    // never reaches the grounded cell, and drops whatever subset it could reach --
    // a structure standing on solid ground sheds chunks of itself.
    enum class SupportState : uint8_t {
        Pending,   // reached by the fill running right now
        Supported, // its piece will not fall this step
        Moved,     // its piece already fell this step
    };

    // Stamps every cell this fill has marked with `state`, plus `extra` if it is
    // not negative (the cell in hand, popped but not yet filed anywhere).
    // Settling on Supported also puts those cells back at rest, since that is
    // the only way a piece ever stops falling.
    void settle_marks(SupportState state, int extra);

    std::vector<int> pending_support;
    std::vector<int> support_stack;     // scratch, reused across fills
    std::vector<int> support_component; // scratch, reused across fills
    // drop_component's sort keys, one per support_component entry. Scratch, and
    // reserved to the same bound, for the same no-allocation reason.
    std::vector<uint64_t> drop_keys;

    // The two working queues of resolve_support(): the seeds taken for the pass in
    // hand, and the ones too slow for it. Members rather than locals so the
    // capacity survives the step -- a structural cascade calls this every step and
    // would otherwise allocate both from scratch. Both are cleared at the top of
    // resolve_support(), so nothing carries between steps; only the storage does.
    std::vector<int> support_seeds;
    std::vector<int> support_deferred;

    // Per-cell "seen this pass" marker. One byte and an epoch counter rather than a
    // bool array that would need clearing every time; the whole thing is cleared
    // once every 255 passes, when the epoch wraps.
    //
    // The epoch advances once per pass, not once per fill. Marks therefore outlive
    // the fill that made them, which is what lets one fill answer for the next: a
    // fill that runs into a cell an earlier one already settled adopts that answer
    // instead of re-deriving it, because two connected cells are by definition the
    // same piece. It is a memo, so it also makes the common case -- hundreds of
    // seeds queued off one piece -- cheap.
    //
    // They must not outlive the pass: the world has moved by then, so an answer
    // from the previous cell of travel is about a world that no longer exists.
    std::vector<uint8_t> support_visit;
    std::vector<uint8_t> support_state;
    uint8_t support_epoch = 0;

    // Falling writes cells, and those writes would otherwise queue support
    // checks for the fall that is already in progress.
    bool resolving_support = false;

    // --- toppling ---
    //
    // Support used to be one yes/no question: is any cell of this piece standing on
    // anything? Yes meant it stayed exactly where it was, however little of it was
    // underneath -- a tall post with one corner on a ledge stood there forever, and
    // a slab landing on the lip of a cliff hung off it by a single cell. Nothing
    // could fall over, only fall down.
    //
    // A held-up piece is now asked a second question: is its centre of mass over
    // what it is standing on? The cells it stands on span some range of columns;
    // if the mean of all its cells lies outside that range, gravity has a lever
    // arm about the last supporting edge and the piece tips over it.
    //
    // The objection in "fracture" below -- that rotating on a grid means
    // resampling the piece and destroying its authored pixels -- is answered by
    // rotating with three shears (Paeth's method) instead of by sampling. Each
    // shear slides whole rows, or whole columns, by an integer number of cells,
    // which is a permutation of the lattice: no cell is dropped, duplicated or
    // recoloured, so a tipped piece is the same cells in a new arrangement and
    // matter is conserved by construction. Every pose is re-derived from the shape
    // the piece had when it started tipping, never from the previous pose, so the
    // small staircase a shear leaves at an angle does not compound frame on frame.
    //
    // A tipping piece lives in the grid the whole time, exactly as a falling one
    // does, so rendering, the player, digging and fire all see it unchanged. What
    // it has that a falling piece does not is identity between steps -- an angle,
    // a spin and an original shape -- which is why it is a TipBody here rather
    // than something re-discovered by flood fill. It gets a fresh piece_tag when it
    // starts, which is both how its cells are recognised and how it stays a
    // separate piece from whatever it lands on afterwards.
    //
    // It stops the moment its next pose would overlap anything solid, and hands
    // back to the ordinary support logic, which asks both questions again from
    // where it lies. That is the whole collision model: no bounce, no slide. A
    // tower lands on its side and is balanced; one that clips a boulder on the
    // way down is re-judged about its new contact and may tip again from there.

    // Gravity for a tipping piece, in cells per step per step. The same quarter
    // cell that a falling piece gains every TICKS_PER_SPEEDUP steps, so a post
    // toppling and one dropping read as the same world.
    static constexpr fx::v TIP_GRAVITY = fx::from_ratio(1, TICKS_PER_SPEEDUP);

    // A tipping piece gives up and is handed back after this many steps, whatever
    // it is doing. Torque only ever grows as a piece goes over, so a real topple
    // ends by collision or by its centre passing below the pivot long before this;
    // the cap exists so that a case nobody thought of costs a frozen piece rather
    // than one that is simulated forever.
    static constexpr int TIP_MAX_STEPS = 600;

    struct TipCell { int16_t x, y; };  // relative to the pivot, in cells

    // A piece that is going over. Pool entries are reused rather than freed (see
    // tip_count), so the vectors inside keep their capacity across topples.
    struct TipBody {
        // The corner it pivots about, as a lattice point: (px, py) is the top-left
        // corner of cell (px, py), so it is shared by the four cells around it.
        int px = 0, py = 0;

        // The cell under the supporting corner, or -1 for the floor of the world.
        // Dig that out and the piece has nothing to pivot on, so it stops tipping
        // and goes back to falling.
        int ground_idx = -1;

        uint8_t tag = 0;
        int age = 0;
        bool moved = false;  // whether any cell has changed place since it started

        fx::v theta = 0;  // radians, clockwise on screen (y points down)
        fx::v omega = 0;  // radians per step

        // Sums over the original shape, taken in doubled coordinates so that the
        // half-cell offset of a cell's centre stays an integer: X2 = 2*lx + 1.
        // What gravity needs is the lever arm and the moment of inertia, and both
        // are linear in these, so they are computed once rather than per step.
        int64_t sx2 = 0, sy2 = 0;  // sum X2, sum Y2
        int64_t i4 = 0;            // sum X2^2 + Y2^2, which is 4 * moment of inertia
        int reach = 1;             // furthest cell centre from the pivot, rounded up

        std::vector<TipCell> shape;  // as it was when it started tipping
        std::vector<int> at;         // where each of those cells is now (grid index)
    };

    std::vector<TipBody> tip_bodies;
    int tip_count = 0;  // live bodies are tip_bodies[0, tip_count)

    // Which piece tags belong to a tipping body. A per-tag flag rather than a
    // per-cell array: a body's cells all wear its fresh tag, so the tag is the
    // ownership test, at no cost per cell.
    bool tip_tag_live[256] = {};

    // What balance treats as one material. Charred is a state of Wood, not a
    // different thing: a beam that has caught in the middle is still one beam.
    static ElementType material_family(ElementType t) {
        return t == ElementType::Charred ? ElementType::Wood : t;
    }

    bool is_tipping(int idx) const {
        return is_structural(cells[idx].type) && tip_tag_live[cells[idx].piece_tag];
    }

    // Called by fall_if_unsupported at the point it concludes a piece is held up.
    // Fills the piece, compares its centre of mass with the span of columns it
    // stands on, and starts it tipping if the centre is outside. `in_flight` means
    // the piece has just landed: it is then the cells that were moving (ticks > 0),
    // because the instant it touches down it is connected to the floor it landed
    // on, and the floor is not part of what is falling over.
    //
    // Returns whether it started one.
    bool topple_if_unbalanced(int x, int y, bool in_flight);

    // Turns balance_component into a TipBody pivoting about corner (px, py).
    void start_tipping(int px, int py, int ground_idx);

    // Advances every tipping body by one step. Runs after resolve_support() and
    // before the sweep, for the same reason that does.
    void step_tipping();

    // Where body `b`'s cells would be at angle `theta`, into tip_next. False if
    // any of them would leave the world.
    bool tip_pose(const TipBody& b, fx::v theta);

    // Tries to move body `b` to the pose in tip_next. False, with nothing
    // written, if that pose overlaps anything solid that is not the body itself.
    bool tip_move(TipBody& b);

    // Ends body `i`: its cells stay where they are and become an ordinary piece.
    // With `requeue`, they are queued for support so they fall, settle or tip
    // again from there; without, they are left at rest where they stopped.
    void end_tipping(int i, bool requeue);

    std::vector<int> balance_component;  // scratch, reused across checks
    std::vector<int> balance_stack;      // scratch, reused across checks
    std::vector<int> tip_next;           // scratch: the pose being tried
    std::vector<Element> tip_carry;      // scratch: the body's cells in transit
    std::vector<Element> tip_displaced;  // scratch: fluid the body moves into

    // Stamps for topple_if_unbalanced, so a piece with hundreds of seeds queued is
    // judged once per pass. Separate from scratch_visit because fracture_landing
    // runs immediately before on the same piece and has already stamped it there.
    //
    // A fresh epoch per check, not per pass, and balance_pass_start marks the
    // first one this pass. That lets a check tell its own stamps from an earlier
    // one's: reaching a cell of the same piece that an earlier check this pass
    // stamped means this is a piece already judged -- and since a judgement that
    // finished stamps its whole piece, one only reachable this way is a piece
    // that check gave up on as too big. So it gives up at once instead of walking
    // another MAX_SUPPORT_CELLS of it. Without that, fire eating along a large
    // welded slab queues seeds a few cells apart, each far enough from the last
    // to start its own walk, and every walk is a full budget.
    std::vector<uint8_t> balance_visit;
    uint8_t balance_epoch = 0;
    uint8_t balance_pass_start = 1;

    // Randomness for the step in progress. These fold in the seed and the step
    // count so that a call site names only what varies: where it is asking from,
    // and which decision it is making. Defined in the header because the call sites
    // run per active cell, and an out-of-line call would cost more than the mix.
    bool coin(uint64_t index, sim_random::Stream s) const {
        return sim_random::coin(world_seed, step_count, index, s);
    }
    bool chance(int pct, uint64_t index, sim_random::Stream s) const {
        return sim_random::chance(pct, world_seed, step_count, index, s);
    }
    bool chance_per_myriad(int per_myriad, uint64_t index, sim_random::Stream s) const {
        return sim_random::chance_per_myriad(per_myriad, world_seed, step_count, index, s);
    }
    int pick(int n, uint64_t index, sim_random::Stream s) const {
        return sim_random::pick(n, world_seed, step_count, index, s);
    }
    // A symmetric offset that does take the clock, unlike authored_spread below.
    // Flame lifetime needs it: pinned at step 0 the draw would be a property of the
    // cell rather than of the flame, so the same spots would always throw the long
    // flames and the ragged top of a fire would be frozen in place.
    int spread(int range, uint64_t index, sim_random::Stream s) const {
        return sim_random::spread(range, world_seed, step_count, index, s);
    }

    // The exception among these three: no step input, pinned at 0 instead.
    //
    // Colour jitter is not a decision the cell retakes every step, it is a property
    // of the material at that spot. Feeding the step in would repaint the whole
    // world every frame.
    int authored_spread(int range, uint64_t index, sim_random::Stream s) const {
        return sim_random::spread(range, world_seed, 0, index, s);
    }
    // The same pinning, for a draw that is one-sided rather than symmetric.
    // Ignition-point jitter needs it: see temp_jitter in reaction.h for why that
    // variation may only ever make a cell easier to light and never harder.
    int authored_pick(int n, uint64_t index, sim_random::Stream s) const {
        return sim_random::pick(n, world_seed, 0, index, s);
    }

    uint32_t jittered_color(const Material& mat, uint64_t index) const;

    // The seed and the step count are the whole of what randomness depends on.
    // Nothing carries state between draws, so a save file that records these two
    // numbers records everything.
    uint64_t world_seed;
};
