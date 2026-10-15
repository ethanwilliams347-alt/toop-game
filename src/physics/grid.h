#pragma once
#include "element.h"
#include "random.h"
#include "fixed_trig.h"
#include <vector>
#include <cstdint>

// Members are grouped by subsystem, each beside the comment that argues for it,
// not ordered for packing. One Grid exists per world and owns megabytes of cell
// and scratch storage, so the tens of bytes of padding the order costs are not
// worth scattering the groups for.
class Grid {  // NOLINT(clang-analyzer-optin.performance.Padding)
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
    void reset(uint64_t seed);

    // Advances the simulation by exactly one fixed step. The caller is responsible
    // for calling this at a fixed rate, not once per rendered frame.
    void update();

    void set_element(int x, int y, ElementType type);
    void paint(int x, int y, ElementType type, uint32_t color);

    // The brush's write path, and the only one that conserves what was already
    // standing in the cell. set_element overwrites, which is right for every other
    // caller -- a reaction turning Wood into Fire means the wood is gone, scene loading
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

    // --- liquids ---
    //
    // A full cell's worth of liquid, in Element::mass units. Every amount of liquid
    // in the engine is counted in these, and a cell placed by the brush or a scene
    // starts with exactly this much.
    //
    // Fine-grained on purpose. Every transfer is a whole number of units, and a
    // difference smaller than the rounding can move is a difference that stays:
    // at 4096 to a cell, the dead band that lets a surface sleep is under half a
    // percent of a cell, and a partial top row levels to a few hundredths of one.
    static constexpr int LIQUID_FULL = 4096;

    // Element::flow is one signed byte, so it stores velocity in steps of this many
    // units a step: up to a quarter of a cell a step either way. Finer than that
    // matters more than range: on a gentle slope a step's acceleration is a few
    // tens of units, and a coarser byte truncated every one of them to zero, so
    // momentum never built and the slope crept level by diffusion alone.
    static constexpr int LIQUID_FLOW_SCALE = 8;

private:
    // Bounds of the cells within one chunk that may still move, in world
    // coordinates, inclusive at both ends. max < min means the chunk is asleep.
    //
    // Empty is stored as an inverted rect at the extremes of int rather than as
    // (0, 0, -1, -1), so that growing it is four plain min/max operations with no
    // "was it empty?" branch: the first box included replaces both ends outright
    // because anything is smaller than INT_MAX and larger than INT_MIN. Every
    // reader checks is_empty() before looking at the bounds, which is the only
    // thing the sentinel values have to survive.
    struct DirtyRect {
        static constexpr int NONE_MIN = 0x7FFFFFFF;
        static constexpr int NONE_MAX = -0x7FFFFFFF - 1;
        int min_x = NONE_MIN, min_y = NONE_MIN, max_x = NONE_MAX, max_y = NONE_MAX;

        bool is_empty() const { return max_x < min_x; }
        void clear() {
            min_x = NONE_MIN;
            min_y = NONE_MIN;
            max_x = NONE_MAX;
            max_y = NONE_MAX;
        }

        // Grows the rect to cover [x0, x1] x [y0, y1]. The caller guarantees
        // x0 <= x1 and y0 <= y1.
        void include_box(int x0, int y0, int x1, int y1) {
            min_x = x0 < min_x ? x0 : min_x;
            min_y = y0 < min_y ? y0 : min_y;
            max_x = x1 > max_x ? x1 : max_x;
            max_y = y1 > max_y ? y1 : max_y;
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

    // Whether any cell whose 3x3 reaches into the chunk has ever been off ambient
    // since the last reset. Sticky: set by note_warm, cleared only by reset().
    //
    // What it buys is step_thermal's early exit for free. A cell at ambient may
    // skip heat only if its forward neighbours are within a degree of ambient too
    // (it owns those exchanges), and reading them costs every awake cell four
    // loads -- about a fifth of `cascading`, a scenario with no heat in it at all.
    // In a chunk that has never been warm, no cell and no neighbour of a cell is
    // off ambient, so the skip needs no reads. Conservative by construction: it
    // can only ever say "maybe warm" of a chunk that has cooled down again.
    std::vector<uint8_t> chunk_warm;

    // Marks every chunk the 3x3 around (x, y) touches as warm -- the same box
    // mark_dirty wakes, for the same reason: a warm cell's neighbours are the
    // cells whose skip it would otherwise fool. Called wherever a cell's
    // temperature can become something other than ambient: place(), a swap
    // carrying a warm cell somewhere new, a tipping body's write, and conduction.
    void note_warm(int x, int y);
    void note_warm_if(int x, int y, uint8_t temperature) {
        if (temperature != AMBIENT_TEMPERATURE) note_warm(x, y);
    }

    // chunk_warm for the chunk the sweep is walking, read once per run of cells
    // instead of once per cell: the per-cell lookup alone was a few percent of
    // `cascading`. note_warm keeps it current when that chunk turns warm mid-run,
    // so it is never stale. -1 outside the sweep.
    int sweep_chunk = -1;
    bool sweep_warm = true;

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
    bool step_gas(int x, int y, const Material& mat);
    bool step_liquid(int x, int y, const Material& mat);

    // --- liquids ---
    //
    // A liquid is an amount, not a set of cells. Each liquid cell holds some of
    // it (Element::mass) and a sideways velocity (Element::flow), and a step
    // moves amounts between neighbouring cells rather than moving cells:
    //
    //   down      as much as the cell below will hold;
    //   sideways  some of the difference to each lower neighbour, and in the
    //             direction it is already flowing, by the momentum it carries;
    //   up        whatever it holds past full, while the cell above has room;
    //   level     if none of those moved anything: see below.
    //
    // Every transfer moves an exact amount between exactly two cells, so liquid is
    // conserved by construction; a cell left with none becomes Empty, and Empty
    // that receives some becomes liquid.
    //
    // Liquid does not compress: a full cell holds LIQUID_FULL whatever is above
    // it. Pressure is a rule instead (level_liquid): an idle surface cell looks
    // through its own body for a lower surface and sends it half the difference,
    // which is what levels a U-tube -- through a tunnel whose cells are all full
    // and so have no difference to flow down.
    //
    // Pressure as compression was tried first, and at this engine's depths it
    // cannot work. A U-tube levels only as fast as the compression difference
    // across its tunnel can push, so it has to be stiff; a body hides its
    // compression as missing volume, so it has to be soft. At 3% a cell a U-tube
    // took a thousand steps to level and a lake painted 339 cells deep settled to
    // 118. The only overfull cells now are the ones a sinking grain presses its
    // water into, and they pass the excess up until it reaches the surface.
    //
    // Momentum is what makes released water run rather than slump: without it a
    // dam break spread by diffusion alone, which the dead band stops while the
    // water still stands ten cells uneven. The velocity is pushed by the
    // difference in amount to either side and decays by LIQUID_DAMP each step, so
    // a pool at rest has none, sleeps, and stays asleep.
    //
    // This replaces three separate rules that each covered an artefact of the one
    // before: venting a sinking grain's water to a nearby surface, draining films
    // off a pile, and moving whole cells between the arms of a body to level it.
    // The last survives as level_liquid, moving amounts rather than cells, so it
    // needs no hysteresis: half a difference cannot overshoot.

    // A cell this light does not spread onto dry ground, so a puddle stops thinning
    // at a quarter of a cell rather than smearing across the whole floor.
    static constexpr int LIQUID_MIN_SPREAD = LIQUID_FULL / 4;

    // How much a sinking grain may press into the liquid around it, as a ceiling on
    // the neighbours' amount. Twice full, so the grain's water goes sideways and
    // down first and then rises out of them through the up flow -- which is the
    // pool's surface rising. Also the ceiling on what the up flow piles into a
    // cell under sand, which keeps every amount well inside 16 bits.
    static constexpr int LIQUID_PUSH_CAP = 2 * LIQUID_FULL;

    // Momentum: velocity gains the difference in amount either side times
    // LIQUID_ACCEL/256 each step, and keeps LIQUID_DAMP/256 of itself. Two
    // differences closer than LIQUID_DEAD push nothing, so a level surface is
    // still. The ceiling on speed is a material's `spread` times
    // LIQUID_SPEED_PER_SPREAD, in mass per step, which is what makes oil slower
    // than water.
    //
    // As strong as it can be and stay stable. The velocity is a cell's, pushed by
    // its two neighbours' difference, so nothing stops alternate cells drifting
    // apart once the push outruns the spreading that evens them: at ACCEL 96, or
    // DAMP 244, pools stopped sleeping, and by 128 a still surface heaped into a
    // standing checkerboard. A flux on each face between cells would not have
    // that weakness, but needs every face's push taken from one consistent state,
    // and the sweep changes cells as it goes.
    static constexpr int LIQUID_ACCEL = 64;
    static constexpr int LIQUID_DAMP = 224;
    static constexpr int LIQUID_DEAD = 16;
    static constexpr int LIQUID_SPEED_PER_SPREAD = 256;

    // Momentum may carry liquid uphill -- into a neighbour holding more -- only
    // while it is moving at least this fast. That is the surge that runs a wave up
    // the far wall. Below it, sideways flow is downhill only, and downhill flow
    // that never overshoots is what guarantees a body of liquid comes to rest:
    // without the floor, a unit nudged uphill by a dying velocity and sent back by
    // the difference it made cycled forever and the pool never slept.
    static constexpr int LIQUID_MIN_SURGE = LIQUID_FULL / 64;

    // How far a sinking grain looks for its own liquid to press water into. The
    // cells beside it are often sand on the flank of a cone, so the neighbours
    // alone leave the water nowhere but up the grain's column; three cells reaches
    // past a flank into the pool.
    static constexpr int LIQUID_DISPERSE_RADIUS = 3;

    // Levelling (level_liquid). The search gives up after LIQUID_LEVEL_CELLS cells
    // of the body -- breadth-first, so in every direction at once -- and a wider
    // body levels in several hops instead of one. It moves nothing for a
    // difference under LIQUID_LEVEL_HEAD, which is what lets a level body sleep:
    // the last of a ripple is the ordinary sideways flow's. And it moves at most
    // LIQUID_LEVEL_RATE a step from any one cell, so a U-tube is seen to flow
    // rather than jump.
    static constexpr int LIQUID_LEVEL_CELLS = 512;
    static constexpr int LIQUID_LEVEL_HEAD = LIQUID_FULL / 4;
    static constexpr int LIQUID_LEVEL_RATE = LIQUID_FULL / 4;

    // How much of two stacked cells' combined amount the lower one holds: all of it
    // up to full, and never more. The only definition of the vertical
    // equilibrium; both the downward and the upward flow are computed from it, so
    // a column at rest moves nothing either way, and anything a sinking grain has
    // pressed into a cell past full is upward flow until it reaches the surface.
    static int stable_lower(int total);

    // Moves `amount` of liquid from cell (fx, fy) to cell (tx, ty), carrying
    // momentum and heat with it, turning an emptied source into Empty and an
    // Empty destination into liquid. Wakes the destination; waking the source is
    // the caller's, once however many transfers it makes from it.
    void transfer_liquid(int fx, int fy, int tx, int ty, int amount);

    // A sinking grain is about to swap into the liquid cell at (x, y): presses
    // that cell's amount into the same liquid nearby -- level with it or below,
    // within LIQUID_DISPERSE_RADIUS, nearest first -- up to LIQUID_PUSH_CAP, so
    // what the swap carries up is only what had nowhere else to go. Leaves
    // waking (x, y) to that swap.
    void disperse_liquid(int x, int y);

    // What a cell looks like, from what it holds: a liquid is drawn more
    // transparent as it thins below full. Everything else is its colour.
    static uint32_t pixel_of(const Element& e);

    // Pressure: a surface cell of liquid at (x, y) that had nothing else to do
    // searches its body for a lower surface with room and sends it half the
    // difference in height, up to LIQUID_LEVEL_RATE. Any path through the body
    // counts, since liquid that does not compress carries pressure through any
    // path. Returns true if anything moved.
    bool level_liquid(int x, int y);

    // level_liquid's search. The marks are cleared after every search by walking
    // the queue, which is at most LIQUID_LEVEL_CELLS long, rather than by an epoch:
    // there can be a search for every surface cell every step, and an epoch byte
    // would wrap -- and need the whole array cleared -- every 255 of them.
    std::vector<int> level_queue;
    std::vector<uint8_t> level_visit;

    // A per-cell "seen this pass" marker for fracture_landing's fill, on the same
    // epoch trick as support_visit.
    std::vector<uint8_t> fracture_visit;
    uint8_t fracture_epoch = 0;

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
    // (difference * conductivity) / divisor. Conduction between neighbours is
    // sixteen times faster than the bleed back to ambient, which is what lets a
    // flame push heat into a wall faster than the wall can shed it -- the other way
    // round and nothing would ever get hot enough to ignite.
    //
    // 512, not the 1024 it was, because each pair now exchanges once a step (see
    // step_thermal) where it used to exchange twice whenever both cells were
    // visited. At 1024 a burning plank stops spreading. burn_probe at seed 4242:
    // the old rule burnt the `front` plank at 1.98 steps a cell, this one at 1.77,
    // with consumption unchanged (1480 steps). Fire here sits close to the point
    // where it stops spreading at all -- 576 and 640 each stall a plank -- so the
    // number was chosen for reliability as well as speed: across six seeds the old
    // rule left a plank partly unburnt in 7 of 12 probe runs, this one in 3.
    static constexpr int CONDUCTION_DIVISOR = 512;
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

    // tip_move's "this cell is the body" marks, on the epoch trick. Its own
    // array rather than support_visit borrowed between passes: see tip_move.
    std::vector<uint8_t> tip_visit;
    uint8_t tip_epoch = 0;

    // Stamps for topple_if_unbalanced, so a piece with hundreds of seeds queued is
    // judged once per pass. Separate from fracture_visit because fracture_landing
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
