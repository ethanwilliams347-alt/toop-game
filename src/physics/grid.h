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

    // Every cell, row-major, get_width() wide -- the same layout as get_pixels(),
    // so index i is the same cell in both. For render code that reads a whole
    // window of cells per frame (render/cell_pack.h), where get_element's bounds
    // check and 12-byte copy per cell would be most of the cost. Read-only: the
    // simulation's own write paths are the only ones, and a pointer that could
    // write would let a renderer feed the simulation. Valid until the grid is
    // destroyed or assigned to; the vector is sized once in the constructor, and
    // reset() refills it in place.
    const Element* cell_data() const { return cells.data(); }

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

    // How many pieces are moving as rigid bodies right now -- toppling, tumbling,
    // sliding, floating. Exposed for tests and the diagnostic for the same reason
    // active_chunk_count() is.
    int tipping_count() const { return tip_count; }

    // One of those bodies, i in [0, tipping_count()), as numbers: where its centre
    // of mass is and how it is moving. For tests, probes and the diagnostic; the
    // simulation never reads it back.
    struct BodyState {
        fx::v x = 0, y = 0;    // centre of mass, cells
        fx::v vx = 0, vy = 0;  // cells per step
        fx::v theta = 0;       // radians, clockwise
        fx::v omega = 0;       // radians per step
        int cells = 0;
        int64_t mass = 0;      // sum of Material::weight
        int age = 0;           // steps since it started moving

        // Where it was at the start of the last step, for drawing it between the
        // two: a body is drawn where it continuously is, not where its cells were
        // rounded to, or it would move a whole cell at a time.
        fx::v prev_x = 0, prev_y = 0, prev_theta = 0;

        // The centre of mass in the shape's own frame, relative to the origin
        // body_shape is stored about. The shape turns about this point.
        fx::v shape_cx = 0, shape_cy = 0;

        // Steps left before it goes back to being cells, if it stays as still as
        // it has been, and the pose those cells will be in, if it is lying near
        // enough square to be set down square (`squares`). The renderer eases the
        // picture onto that pose over the last few steps, so the hand-back is not
        // a jump of up to a cell.
        int steps_to_sleep = 0;
        bool squares = false;
        fx::v rest_x = 0, rest_y = 0, rest_theta = 0;
    };
    BodyState body_state(int i) const;

    // A body's shape as it was when it started moving, one entry per cell, in its
    // own frame -- cell (x, y) spans [x, x + 1) -- and, entry for entry, which grid
    // cell each of those is now. Together they are a picture of the body to draw
    // turned, with the colours the grid holds for it now (a burning beam chars as
    // it tumbles).
    struct BodyCell {
        int16_t x, y;
    };
    const std::vector<BodyCell>& body_shape(int i) const;
    const std::vector<int>& body_cells(int i) const;

    // Whether the cell at (x, y) is part of one of those bodies. Out of bounds is
    // false. For the cell inspector, the renderer and tests.
    bool moving_at(int x, int y) const {
        return is_within_bounds(x, y) && is_tipping(get_index(x, y));
    }

    // A drop of liquid thrown clear of the grid: water heaved up fast enough to
    // leave the surface (see step_rise) flies as a point until it meets anything
    // that is not empty space, and then goes back into the grid as an amount. The
    // Noita-style hybrid -- the grid is good at pools and bad at anything moving
    // faster than a cell a step, and a splash is all that.
    //
    // Its amount is out of the grid while it flies, so a count of a world's
    // liquid is the cells' plus spray_mass(). Positions and velocities are 16.16
    // cells and cells per step, like a body's; (prev_x, prev_y) is where it was at
    // the start of the step, for drawing it between steps.
    struct Spray {
        fx::v x = 0, y = 0;
        fx::v vx = 0, vy = 0;
        fx::v prev_x = 0, prev_y = 0;
        uint32_t color = 0;
        uint16_t mass = 0;
        ElementType type = ElementType::Empty;
        uint8_t temperature = AMBIENT_TEMPERATURE;
    };
    const std::vector<Spray>& spray() const { return spray_drops; }
    int64_t spray_mass() const {
        int64_t m = 0;
        for (const Spray& d : spray_drops) m += d.mass;
        return m;
    }

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
    // units a step: up to half a cell a step either way. At 8 the byte topped out at
    // a quarter of a cell, under water's own ceiling, and a released body ran at
    // that limit for most of its run, which read as thick. Coarser steps no longer
    // lose small pushes: step_liquid rounds the stored velocity at random in
    // proportion to the remainder, so a gentle slope's few tens of units a step
    // still add up on average. Before that rounding, a coarser byte truncated every
    // one of them to zero and the slope crept level by diffusion alone.
    static constexpr int LIQUID_FLOW_SCALE = 16;

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
    // than water: three-eighths of a cell a step for water at spread 6, under the
    // byte's half (LIQUID_FLOW_SCALE), and three-sixteenths for oil at 3.
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

    // --- vertical velocity and spray ---
    //
    // A liquid cell's upward velocity (its `ticks`, see element.h) is in steps of
    // 1/LIQUID_RISE_SCALE of a cell a step, so the signed byte reaches about four
    // cells a step. Nothing in a pool at rest has any: the down and up rules above
    // are what move water vertically, and they are instant, so a velocity would
    // add nothing to them. It exists for what they cannot do -- water thrown
    // upward by a body pushing it aside, which rises against gravity for a few
    // steps and falls back, or leaves the surface altogether as spray.
    static constexpr int LIQUID_RISE_SCALE = 32;

    // Taken off a rising cell's velocity every step: BODY_GRAVITY's 9/40 cell a
    // step squared at this scale, rounded down, so a heave falls back the way a
    // body thrown up does.
    static constexpr int LIQUID_RISE_GRAVITY = 7;

    // Share of its rise a cell keeps each step after gravity, /256, and the share
    // of what is left it hands the liquid on top of it when it is buried: a push
    // deep in a pool is carried up the column to the surface, losing some at
    // every cell, which is pressure doing the work. Strong enough that a plunge's
    // ring of pushed water heaves the surface beside the body; weak enough that a
    // push two bodies deep is gone before it gets there.
    static constexpr int LIQUID_RISE_DAMP = 224;

    // A surface cell rising at least this fast, in LIQUID_RISE_SCALE units --
    // three quarters of a cell a step -- throws a drop instead of heaving. Slower,
    // and the surface bulges and settles back; at this speed a heave would climb
    // over a cell in a step, which the grid draws as water teleporting up a row.
    static constexpr int SPRAY_MIN_RISE = 24;

    // Most a single drop takes from its cell. Half a cell: a full cell thrown
    // whole reads as a block, not a drop, and the rest of it heaves next step
    // and may throw another.
    static constexpr int SPRAY_MASS = LIQUID_FULL / 2;

    // How many drops can be in the air at once. The pool is reserved to this in
    // the constructor and never grows; water that would have thrown a drop past
    // the limit heaves instead.
    static constexpr int SPRAY_MAX = 2048;

    // Share of a moving body's speed the water it pushes aside is given, 16.16.
    // That water goes out from the body along the face it was pushed off, and
    // back along the body's path -- out and up from a body plunging in, the bow
    // wave of one crossing a pool. All of the speed: a body moving a cell a step
    // moves the water in front of it a cell a step, and it is the drag that takes
    // energy out, not the push.
    static constexpr fx::v SPLASH_SHARE = fx::ONE;

    // Speed, in cells a step, that a body pushes water aside at without giving it
    // any motion; above it the water gets SPLASH_SHARE of only the excess. A
    // floating body bobs at a tenth of this, and with no dead band every bob
    // pushed a ring of water out at its own speed, the ring came back off the
    // walls and lifted it again, and a raft that had settled in three seconds
    // bobbed for the whole of the probe's twenty-five: the push was adding energy
    // the drag on the body had not taken out. A plunge, at a cell a step and
    // more, still throws most of its speed into the water.
    static constexpr fx::v SPLASH_MIN_SPEED = fx::from_ratio(1, 4);

    // How far along a body's leading face push_aside looks for its end, in cells,
    // when the water in front of the middle of it has nothing beside it but body.
    // Past this -- a body wider than twice it -- the middle goes behind the body
    // as it always did.
    static constexpr int MAX_AROUND = 24;

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

    // Adds `amount` of liquid like `like` to cell `to`, moving at (flow, rise) in
    // Element::flow and LIQUID_RISE_SCALE units, mixed by amount with what is
    // there. `to` must be Empty or that liquid already. Wakes it.
    void deposit_liquid(int to, const Element& like, int amount, int flow, int rise);

    // The vertical half of step_liquid, for a cell with a rise: gravity and
    // damping, then either a heave into empty space above, a drop thrown clear
    // (spray), or the push handed on to the liquid on top. True if the cell has
    // moved and step_liquid has nothing more to do with it this step.
    bool step_rise(int x, int y, int rise);

    // Flies every drop of spray one step and lands the ones that hit something.
    // Before the sweep, so landed water starts flowing on this step.
    void step_spray();

    // Puts drop `d` back into the grid, into the liquid it hit at `hit` if that is
    // the same liquid, else into the empty cell it is in (or the first empty cell
    // or same liquid above it, if something has moved in under it). False, with
    // the drop still flying, if there is nowhere in its column at all.
    bool land_spray(const Spray& d, int at, int hit);

    // A body is entering the cell `from`, which held `e`, moving at (vx, vy):
    // pushes as much of that liquid as there is room for into the cells beside
    // `from` that the body is neither entering nor leaving, giving it the body's
    // momentum (SPLASH_SHARE). Returns how much went; tip_move puts the rest into
    // a cell the body has left, as it always did.
    int push_aside(int from, const Element& e, int64_t vx, int64_t vy);

    // A cell a body has just left, still empty after its displaced liquid is
    // placed: draws liquid in from beside and above it -- below too, if it is
    // under the surface -- moving with the body, the way water closes behind a
    // body instead of leaving a hole.
    void draw_in(int idx, int64_t vx);

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

    // The spray in the air, at most SPRAY_MAX, reserved to that. A drop that
    // lands is replaced by the last one, so order is by when each was thrown
    // only until the first lands -- deterministic, which is all it needs to be.
    std::vector<Spray> spray_drops;

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
    // Pieces a balance check found carrying something it weighed, to be weighed
    // themselves next step (see topple_if_unbalanced). Grows to a high-water mark.
    std::vector<int> support_rejudge;
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

    // --- loose bodies ---
    //
    // Support used to be one yes/no question -- is any cell of this piece standing
    // on anything? -- and then two: is its centre of mass over what it stands on?
    // The second made pieces tip, but only as a hinge: a piece turned about one
    // fixed corner, stopped dead on the first thing it touched, and anything that
    // came clear of its corner was handed to falling, which drops straight down. A
    // post tipping off a ledge lost its spin and its sideways speed the moment it
    // left the edge, and fell like a lift; nothing ever bounced, slid, rolled or
    // knocked anything else over. Gravity was a set of rules about pieces rather
    // than a force acting on them.
    //
    // A piece that is going anywhere other than straight down is now a rigid body
    // with real state: a centre of mass, a linear and an angular velocity, a mass
    // and a moment of inertia summed from what it is made of (Material::weight --
    // stone is four times as heavy as wood). Every step gravity accelerates it,
    // and every contact with the world pushes back through an impulse at the point
    // of contact, with friction and restitution. Toppling is no longer a rule. A
    // post standing on one corner of its foot tips because the only thing holding
    // it up is pushing on it off its centre, so the push turns it; as it goes over
    // the contact slides or holds by friction; when it comes clear it keeps the
    // spin and the speed it had, flies off in an arc and lands on whatever corner
    // reaches the ground first, which kicks it again. Straight down, the cheap
    // case, stays with falling (drop_component), which is the same motion.
    //
    // The solver is the standard one -- sequential impulses with accumulated,
    // clamped normal and friction impulses (Catto) -- in 16.16 integers, so it
    // replays exactly. Contacts come from the grid itself: a body's cell beside a
    // solid cell that is not the body is a contact, with a normal read off the
    // shape of the solid around it rather than off the cell's square edge, so a
    // body slides down a diagonal slope instead of catching on its staircase. Many
    // edge contacts on one face are reduced to the two at its ends, the way a box
    // engine keeps a two-point manifold, which is all a face needs to hold up a
    // body or to tip it.
    //
    // Poses stay what they were: three shears (Paeth) about a lattice point, a
    // permutation of the lattice, so a body never gains, loses or recolours a cell,
    // and every pose is derived from the shape it started with. The body lives in
    // the grid the whole time, which keeps rendering, the player, digging and
    // fire working on it unchanged. It moves in sub-steps of at most a cell at its
    // rim, each one checked, so a fast body cannot pass through a thin wall; a
    // sub-step into something it had not touched yet is where it stops, and the
    // next step's contacts take the impact. The body's continuous state is never
    // allowed to run ahead of where its cells are.
    //
    // Bodies push bodies, and wake pieces at rest: a body striking a standing
    // piece that is free to move (judged as balance judges one, below) turns it
    // into a body too, and the two then share every impulse between them by their
    // masses. That is what makes a row of posts go down like dominoes. A body
    // resting under a pile of sand, or a block, is pressed by its weight, and so is
    // a piece being weighed for balance: a plank across a post tips when enough
    // sand lands on one end of it. A body in liquid is lifted by the weight of the
    // liquid it displaces and slowed by it, so wood floats and stone sinks slowly.
    //
    // A body that has stopped -- slow, not speeding up, and touching something or
    // floating -- for BODY_SLEEP_STEPS goes back to being ordinary structure, with
    // a tag of its own; if it moved, the support logic asks about it again from
    // where it lies, and that is the whole hand-off in both directions.

    // Gravity on a body, in cells per step per step. The same quarter cell that a
    // falling piece gains every TICKS_PER_SPEEDUP steps, so a post toppling and one
    // dropping read as the same world.
    //
    // Nine tenths of the quarter cell a step it was first set to, which a playtest
    // found a little quick.
    static constexpr fx::v BODY_GRAVITY = fx::from_ratio(9, 40);

    // Air drag, per step, as a share of speed times speed: a falling body's speed
    // eases toward where drag balances gravity, sqrt(BODY_GRAVITY / AIR_DRAG),
    // about 7.5 cells a step, instead of climbing in a straight line and stopping
    // dead at MAX_FALL_SPEED. The straight line is most of what made a fall look
    // like arithmetic.
    static constexpr fx::v AIR_DRAG = fx::from_ratio(1, 250);

    // How much rougher or smoother than its material a surface is at any one
    // spot, in percent either way, and the most a body is nudged by as it comes
    // loose, in radians per step. Real surfaces are not uniform and nothing comes
    // loose perfectly still: without either, every topple of a given shape is the
    // same topple, and balance is a knife edge a real object never stands on.
    static constexpr int GRIP_ROUGHNESS = 20;
    static constexpr fx::v NUDGE_SPIN = fx::from_ratio(1, 1500);

    // A loose piece whose centre of mass is within this many cells of the edge of
    // its footing is handed to the solver rather than ruled standing: near the
    // edge, whether it stays is a matter of friction, rolling and the nudge, not
    // of which side of a line its centre is on.
    static constexpr fx::v BALANCE_MARGIN = fx::from_ratio(1, 2);

    // The most times its own weight a landing loads a body with, for snapping.
    // The solver delivers a landing in one step, as one spike; a real one is
    // spread over the moment the two surfaces take to give, and a wooden stick
    // falling flat does not break.
    static constexpr fx::v IMPACT_LOAD_MAX = fx::from_int(6);

    // Passes the contact solver makes over a body's contacts per step. Each pass
    // spreads the impulses a little further through the contacts that share them;
    // eight is where a slab resting on two points stops visibly sagging into one.
    static constexpr int BODY_SOLVER_PASSES = 32;

    // How much of last step's impulse a contact starts this step with (see the
    // warm start in solve_contacts). All of it converges fastest and, when the
    // contact has moved since, pushes with a step-old answer: a falling stack
    // threw its blocks across the room.
    static constexpr fx::v WARM_START = fx::from_ratio(1, 4);

    // A body counts as stopped when, held by something, no part of it has strayed
    // more than BODY_SLEEP_DRIFT cells from where it was BODY_SLEEP_STEPS steps
    // ago. Half a second: long enough that a topple starting from the very edge
    // of balance -- slow, but gaining all the time -- is past half a cell before
    // the window closes.
    static constexpr fx::v BODY_SLEEP_DRIFT = fx::from_ratio(1, 2);
    static constexpr int BODY_SLEEP_STEPS = 30;

    // A body coming to rest within this many radians (about six degrees) of a
    // quarter turn is set down square. On a grid that is how it would lie: a post
    // lying on flat ground at 92 degrees is resting on the staircase its own
    // shears made of its edge, and its cells are that staircase, which no turned
    // picture matches -- the hand-back from sprite to cells jumped by it. Square,
    // the cells are the shape turned exactly, and the sprite eases onto them.
    static constexpr fx::v REST_SQUARE_ANGLE = fx::from_ratio(1, 10);

    // Rolling resistance, as the lever it acts at, in cells: each step a body's
    // spin is opposed by at most the normal impulse holding it up times this. A
    // contact on a grid is never the knife edge the solver models -- the rim of a
    // disc is a staircase, and the corner a post tips over is a cell wide -- and
    // without it a lumpy disc rocks for ever on the energy the rounding feeds in.
    // Small, because a post past its balance point by a fraction of a cell has
    // only that much more torque than this to get going with.
    static constexpr fx::v ROLLING_ARM = fx::from_ratio(1, 16);

    // An impact slower than this, in cells per step along the contact normal,
    // gives back nothing. Without a threshold a body resting on the floor bounces
    // off the quarter cell gravity puts into it every step, and never sleeps.
    static constexpr fx::v BOUNCE_MIN_SPEED = fx::from_ratio(3, 4);

    // How fast a body has to be closing on a piece at rest, in cells per step, for
    // the piece to be woken and pushed. Above a step's worth of gravity, so that a
    // body lying on something does not wake it every step just by lying there.
    static constexpr fx::v WAKE_SPEED = fx::from_ratio(3, 8);

    // How hard a body has to lean sideways on a piece at rest to wake it: the
    // normal impulse of one contact in a step, per unit of the body's mass, in
    // cells per step. A sixteenth of a step's gravity -- any real lean.
    static constexpr fx::v PUSH_WAKE = fx::from_ratio(1, 64);

    // Share of its velocity a fully submerged body loses each step, in 1/256ths,
    // scaled by how much of it is under: the slow, viscous part of liquid drag,
    // which is what stops a floating body bobbing for ever once the waves it
    // makes have carried the rest away. Its spin has its own, LIQUID_SPIN_DRAG.
    //
    // It was 96, and all of the drag, until a playtest found sinking linear and
    // snappy and floating wood popping. A body losing over a third of its speed a
    // step is at its terminal speed within a few steps of going under, and from
    // then on falls at a constant rate -- the straight line -- and the drag
    // switches on all at once as it touches the surface. The speed-squared part,
    // LIQUID_FORM_DRAG, now does the work at speed, and this only the settling.
    static constexpr int LIQUID_DRAG = 40;
    static constexpr int LIQUID_SPIN_DRAG = 40;

    // Form drag, the part that grows with the square of speed: per step a body's
    // speed v becomes v / (1 + k v), with k this times the liquid's weight times
    // the square root of the cells under (the width it pushes through) over its
    // mass, added mass included. Written that way rather than as v - k v^2 so a
    // body slapping into a pool at speed is slowed hard but never turned back.
    // Sets the terminal speed: a 8x6 stone block sinks at about 0.6 cells a step.
    static constexpr fx::v LIQUID_FORM_DRAG = fx::from_ratio(5, 1);

    // Added mass: a body moving through liquid has to move some of the liquid
    // with it, so it is harder to speed up or slow down than its own mass says,
    // by this share of the weight of liquid it displaces (1 for a cylinder in two
    // dimensions). It is why a stone gathers speed in water instead of starting
    // at it, and why wood held under rises at a quarter of gravity rather than at
    // two thirds of it -- it was the pop a playtest saw.
    static constexpr fx::v LIQUID_ADDED_MASS = fx::from_ratio(1, 1);

    // BODY_SLEEP_DRIFT for a body held only by liquid. A body afloat drifts in
    // a gentle bob that half a cell would take for stopped, and stopping it there
    // freezes it mid-bob; the support logic then finds it floating and launches
    // it again from rest -- the pop.
    static constexpr fx::v FLOAT_SLEEP_DRIFT = fx::from_ratio(1, 8);

    // How far up a column of resting load is followed, in cells. A column that
    // is still solid this high is not a load; it is a ceiling, or the rest of the
    // level, and holds itself up. A post wedged under an overhang is not pressed
    // into the ground by the mountain above it.
    static constexpr int LOAD_SCAN = 48;

    // A body still moving after this many steps is frozen where it is. Bodies end
    // by stopping, long before; this is so that a case nobody thought of costs a
    // frozen piece rather than one simulated for ever.
    static constexpr int BODY_MAX_STEPS = 1800;

    // One contact, between body A (the one being stepped) and either the world or
    // body `other`. Everything in 16.16; impulses are per unit of A's mass, so a
    // lone body against the world never needs its own mass at all and the numbers
    // stay well inside 64 bits whatever it weighs.
    struct Contact {
        int64_t rax = 0, ray = 0;  // contact point relative to A's centre of mass
        int64_t rbx = 0, rby = 0;  // and to B's, if there is a B
        int64_t nx = 0, ny = 0;    // unit normal, pointing from the obstacle into A
        int64_t kn = 0, kt = 0;    // inverse effective mass along the normal / tangent
        int64_t rho = 0;           // A's mass over B's, 0 for the world
        int64_t target = 0;        // separating speed wanted along the normal (bounce)
        int64_t grip = 0;          // friction coefficient for the pair
        int64_t acc_n = 0, acc_t = 0;  // impulse accumulated over the passes
        int self = 0;       // body A
        int other = -1;     // body B, or -1 for the world
        int obstacle = -1;  // for the world, the structural cell touched, if any
        uint32_t id = 0;    // the same contact next step has the same id: see solve_contacts
    };

    // A loose piece. Pool entries are reused rather than freed (see tip_count),
    // so the vectors inside keep their capacity across bodies.
    struct Body {
        uint8_t tag = 0;
        int age = 0;
        int still = 0;       // stopped steps in a row
        bool moved = false;  // whether any cell has changed place since it started
        bool touching = false;  // had a contact, or was in liquid, on its last step
        bool contact = false;   // had a contact on its last step, liquid aside

        // Continuous state, 16.16. Position is the centre of mass in world cells
        // (cell x spans [x, x + 1)); angle is clockwise on screen (y points down).
        int64_t cx = 0, cy = 0;
        int64_t vx = 0, vy = 0;  // cells per step
        fx::v theta = 0;
        int64_t omega = 0;       // radians per step

        // Where it started, and where its current stopped count began: see
        // BODY_SLEEP_STEPS. Its total normal impulse this step, for rolling
        // resistance.
        int64_t start_cx = 0, start_cy = 0, rest_cx = 0, rest_cy = 0;
        int64_t prev_cx = 0, prev_cy = 0;
        fx::v prev_theta = 0;
        fx::v start_theta = 0, rest_theta = 0;
        int64_t support = 0;

        // The lattice point of the shape the pose turns about, relative to the
        // shape's origin. Moved, each step, to the contact pressing hardest on the
        // body, because the shears are exact at the point they turn about and off
        // by up to a cell at the far end: where a body is touching something is
        // where that cell matters. The hardest-pressed contact this step, as a
        // world point, and how hard, are kept for choosing it.
        int16_t pivot_x = 0, pivot_y = 0;
        int64_t pressed = 0, press_x = 0, press_y = 0;
        bool pivot_held = false;  // a contact this step was within a cell of the pivot

        // The centre of mass relative to the origin lattice point the shape is
        // stored about, 16.16. The pose puts the origin at cx,cy - R(theta)*lc.
        int64_t lcx = 0, lcy = 0;
        int64_t mass = 1;  // sum of Material::weight over its cells
        int64_t k2 = 1;    // moment of inertia over mass, about the centre: cells^2, 16.16
        int reach = 1;  // furthest cell centre from the centre of mass, whole cells, rounded up
        int64_t grip = 0, bounce = 0;  // its material's, 16.16 fractions

        std::vector<BodyCell> shape;  // as it was when it started
        std::vector<int> at;  // where each of those cells is now (grid index)
    };

    std::vector<Body> tip_bodies;
    int tip_count = 0;  // live bodies are tip_bodies[0, tip_count)

    // Which body each piece tag belongs to, or -1. A per-tag table rather than a
    // per-cell array: a body's cells all wear its fresh tag, so the tag is the
    // ownership test, at no cost per cell. Kept in step with swap-removes.
    int16_t tag_body[256];

    // What balance treats as one material. Charred is a state of Wood, not a
    // different thing: a beam that has caught in the middle is still one beam.
    static ElementType material_family(ElementType t) {
        return t == ElementType::Charred ? ElementType::Wood : t;
    }

    bool is_tipping(int idx) const {
        return is_structural(cells[idx].type) && tag_body[cells[idx].piece_tag] >= 0;
    }

    // Collects into balance_component the piece containing `seed` the way balance
    // judges one: the run of its tag and its material (or, `in_flight`, the cells
    // that were moving). False if it should not be judged at all -- it was judged
    // already this pass, it is too big, or it stands on the bottom of the world.
    bool collect_piece(int seed, bool in_flight);

    // Everything resting on the piece in balance_component, transitively, each
    // piece weighed by the share of its footing that comes down on this one. `w`
    // and `wx2` (the piece's weight and doubled-x moment) come in as the piece's
    // own and go out scaled by LOAD_WHOLE, load included.
    void add_load(int64_t& w, int64_t& wx2);

    // add_load's scratch: its own visit marks, the set of cells it has weighed so
    // far and the share each one is carried at, in 1/LOAD_WHOLE.
    static constexpr int LOAD_WHOLE = 256;
    std::vector<uint8_t> load_visit;
    std::vector<uint16_t> load_share_at;
    std::vector<int> load_cells;
    std::vector<int> load_stack;
    std::vector<int> load_pieces;  // [begin, end) in load_cells of each piece found carried
    uint8_t load_epoch = 0;

    // Called by fall_if_unsupported at the point it concludes a piece is held up.
    // Fills the piece, compares its centre of mass -- with the load resting on it
    // -- with the span of columns it stands on, and makes it a body if the centre
    // is outside. Also makes it a body if it is a loose piece lighter than the
    // liquid it is in, which floats it. `in_flight` means the piece has just
    // landed: it is then the cells that were moving (ticks > 0), because the
    // instant it touches down it is connected to the floor it landed on, and the
    // floor is not part of what is falling over. It lands with the speed it fell
    // at, which is what kicks it.
    //
    // Returns whether it started one.
    bool topple_if_unbalanced(int x, int y, bool in_flight);

    // Turns `cells_of` into a body moving at (vx, vy). Gives it a fresh tag.
    // Returns its index in the pool.
    int start_body(const std::vector<int>& cells_of, int64_t vx, int64_t vy);

    // Sums mass, centre and inertia over body `b`'s shape, from the materials its
    // cells are now, keeping its pose and the motion of every cell it still has.
    // Used when it starts and whenever fire or digging changes what it is made of.
    void weigh_body(Body& b, bool keep_motion);

    // Advances every body by one step. Runs after resolve_support() and before the
    // sweep, for the same reason that does.
    void step_tipping();

    // The parts of a step: each body's contacts (appended to `contacts`) and the
    // forces on it, then one solve over every contact, then each body moves.
    // gather_contacts may wake other pieces, which can grow the pool, so it takes
    // an index, not a reference.
    void gather_contacts(int i);
    void apply_body_forces(Body& b);
    void solve_contacts();
    // Returns whether any cell of it moved.
    bool advance_body(Body& b);

    // Where body `b`'s cells would be with its centre at (cx, cy) and angle
    // `theta`, into tip_next. False if any of them would leave the world.
    bool tip_pose(const Body& b, int64_t cx, int64_t cy, fx::v theta);

    // Tries to move body `b` to the pose in tip_next. False, with nothing
    // written, if that pose overlaps anything solid that is not the body itself.
    bool tip_move(Body& b);

    // The pose body `b` comes to rest in if it is lying within REST_SQUARE_ANGLE
    // of a quarter turn: that exact turn, with its pivot on a lattice point, so
    // its cells are the shape turned without a shear. It tries the lattice point
    // under the pivot first, since a resting body is settling downward, then the
    // one over it, and takes the first whose cells fit. False if it is at more of
    // an angle than that or neither fits, and then it rests as it is.
    bool square_pose(const Body& b, int64_t& cx, int64_t& cy, fx::v& theta) const;
    // Moves `b` into that pose, if it has one and it fits.
    void square_body(Body& b);

    // Breaks body `i` at the edge of what holds it up if its overhang bends more
    // than its section there can carry, loaded `load` times its weight (16.16).
    // Returns whether it broke; the broken-off part is queued, to come loose as a
    // body of its own next step.
    bool snap_overhang(int i, int64_t load);
    struct SnapColumn {
        int cells = 0;
        int64_t weight = 0;
        uint16_t strength = 0xFFFF;  // the weakest cell's
        bool held = false;           // something not the body directly under one of its cells
    };
    std::vector<SnapColumn> snap_cols;  // scratch, one per column a body spans

    // Ends body `i`: its cells stay where they are and become an ordinary piece.
    // With `requeue`, whatever they have come to rest on is queued for support,
    // to be weighed with them on it; the body's own cells are not, since a body
    // only stops once something holds it.
    void end_tipping(int i, bool requeue);

    std::vector<int> balance_component;  // scratch, reused across checks
    std::vector<int> balance_stack;      // scratch, reused across checks
    std::vector<int> tip_next;           // scratch: the pose being tried
    std::vector<Element> tip_carry;      // scratch: the body's cells in transit
    std::vector<Element> tip_displaced;  // scratch: fluid the body moves into
    std::vector<int> tip_displaced_at;   // scratch: and which cell each came from
    std::vector<Contact> contacts;  // scratch: every body's, this step

    // Last step's contacts, with the impulses they ended on, for warm starting;
    // prev_contacts[warm_from[tag], warm_to[tag]) are the ones whose first body
    // wore that tag. Grows to a high-water mark with `contacts`.
    std::vector<Contact> prev_contacts;
    int warm_from[256] = {};
    int warm_to[256] = {};

    // Manifold reduction for gather_contacts: per (direction, other body), the two
    // contacts furthest apart along the face. Fixed size; a body touching more
    // distinct faces than this at once keeps the first ones it found.
    static constexpr int MAX_CONTACT_FACES = 16;
    struct Face {
        int key = 0;
        int64_t lo_t = 0, hi_t = 0;
        Contact lo, hi;
    };
    Face faces[MAX_CONTACT_FACES];
    int face_count = 0;

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
