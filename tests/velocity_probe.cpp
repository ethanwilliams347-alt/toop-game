// What Element::ticks can actually represent, in numbers.
//
// A prototype rather than a feature: it ships nothing and asserts almost
// nothing, and what it produces is a number to decide a representation against.
// Not registered as a test, because every candidate below is a representation
// that does not exist in the engine -- a test asserting today's answer would pin
// a prototype rather than check the product.
//
// Three sections, answering three different halves of the question:
//
//   1. Layout. What a second byte would actually cost, measured with sizeof and
//      offsetof rather than counted by hand.
//   2. What the representation has to hold, which is two quantities per axis and
//      not one -- a velocity and a sub-cell remainder.
//   3. Trajectories. Four candidate representations flown against an fx 16.16
//      reference, which is the arithmetic Player already uses and therefore the
//      closest thing to ground truth this project has.
//
// The question section 3 exists to answer is whether a packed integer velocity
// can carry a gravity term at all.

#include "physics/element.h"
#include "physics/fixed.h"
#include "physics/random.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {

// ---------------------------------------------------------------------------
// 1. Layout
// ---------------------------------------------------------------------------
//
// Mirrors of Element with fields added in the two places they could go. Copies
// rather than edits to the real struct on purpose: the point is to price a
// change without making it, and a probe that had to modify element.h to run
// would be a fork of the engine rather than an instrument on it.
//
// `Mirror` is checked against the real Element below, so a field added to the
// real one without being added here fails loudly instead of quietly measuring
// last month's struct.

struct Mirror {
    ElementType type = ElementType::Empty;
    uint32_t color = 0;
    uint8_t updated_tag = 0;
    uint8_t ticks = 0;
    uint8_t temperature = 0;
    uint8_t piece_tag = 0;
};

// One byte appended after the last field -- the shape "add a field to Element"
// takes if it is written the way every field has been written.
struct MirrorAppended {
    ElementType type = ElementType::Empty;
    uint32_t color = 0;
    uint8_t updated_tag = 0;
    uint8_t ticks = 0;
    uint8_t temperature = 0;
    uint8_t piece_tag = 0;
    uint8_t velocity = 0;
};

// The same byte, declared between `type` and `color` instead.
struct MirrorFrontOne {
    ElementType type = ElementType::Empty;
    uint8_t velocity = 0;
    uint32_t color = 0;
    uint8_t updated_tag = 0;
    uint8_t ticks = 0;
    uint8_t temperature = 0;
    uint8_t piece_tag = 0;
};

// Three bytes there, which is what section 2 works out is actually needed.
struct MirrorFrontThree {
    ElementType type = ElementType::Empty;
    int8_t vel_x = 0;
    int8_t vel_y = 0;
    uint8_t rem = 0;
    uint32_t color = 0;
    uint8_t updated_tag = 0;
    uint8_t ticks = 0;
    uint8_t temperature = 0;
    uint8_t piece_tag = 0;
};

// Four -- one past what fits, so the output shows the cliff rather than only the
// side of it we hope to be on. A number with no failing neighbour is not
// evidence of a boundary.
struct MirrorFrontFour {
    ElementType type = ElementType::Empty;
    int8_t vel_x = 0;
    int8_t vel_y = 0;
    uint8_t rem = 0;
    uint8_t spare = 0;
    uint32_t color = 0;
    uint8_t updated_tag = 0;
    uint8_t ticks = 0;
    uint8_t temperature = 0;
    uint8_t piece_tag = 0;
};

// The played world, so the per-cell figures below are in the units the budget is
// argued in.
constexpr long long WORLD_CELLS = 1920LL * 1080LL;

void report_layout() {
    std::printf("1. LAYOUT - what a second byte costs, measured rather than counted\n\n");

    std::printf("   sizeof(Element)              = %d   alignof = %d\n",
                static_cast<int>(sizeof(Element)), static_cast<int>(alignof(Element)));
    std::printf("   offsets in Element:  type %d  color %d  updated_tag %d  ticks %d  temperature %d  piece_tag %d\n",
                static_cast<int>(offsetof(Element, type)),
                static_cast<int>(offsetof(Element, color)),
                static_cast<int>(offsetof(Element, updated_tag)),
                static_cast<int>(offsetof(Element, ticks)),
                static_cast<int>(offsetof(Element, temperature)),
                static_cast<int>(offsetof(Element, piece_tag)));

    // The gap the whole section is about: `type` is one byte and `color` needs
    // four-byte alignment, so offsets 1..3 are padding that no field occupies.
    const int front_gap = static_cast<int>(offsetof(Element, color)) -
                          static_cast<int>(offsetof(Element, type) + sizeof(ElementType));
    const int tail_gap = static_cast<int>(sizeof(Element)) -
                         static_cast<int>(offsetof(Element, piece_tag) + 1);
    std::printf("   unused bytes:  %d before `color`   %d after `piece_tag`\n\n", front_gap, tail_gap);

    struct Row { const char* name; size_t size; };
    const Row rows[] = {
        { "Element today            ", sizeof(Element) },
        { "Mirror (must match)      ", sizeof(Mirror) },
        { "+1 byte appended at end  ", sizeof(MirrorAppended) },
        { "+1 byte after `type`     ", sizeof(MirrorFrontOne) },
        { "+3 bytes after `type`    ", sizeof(MirrorFrontThree) },
        { "+4 bytes after `type`    ", sizeof(MirrorFrontFour) },
    };

    for (const Row& r : rows) {
        const long long extra = static_cast<long long>(r.size) - static_cast<long long>(sizeof(Element));
        const double mb = static_cast<double>(extra * WORLD_CELLS) / (1024.0 * 1024.0);
        if (extra == 0)
            std::printf("   %s %2d bytes    no growth\n", r.name, static_cast<int>(r.size));
        else
            std::printf("   %s %2d bytes    +%lld bytes/cell = %+.1f MB at 1920x1080 (%+.0f%% memory traffic)\n",
                        r.name, static_cast<int>(r.size), extra, mb,
                        100.0 * static_cast<double>(extra) / static_cast<double>(sizeof(Element)));
    }

    if (sizeof(Mirror) != sizeof(Element))
        std::printf("\n   !! Mirror does not match Element - a field was added to the real struct\n"
                    "      and not to this probe, so every row above is about the wrong type.\n");
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// 2. What the representation has to hold
// ---------------------------------------------------------------------------
//
// A cell's position is a cell index and nothing else. So a velocity with a
// fraction in it is only half of what motion needs: something has to remember
// the part of a cell the mover has crossed but not completed, or a speed of half
// a cell per step is indistinguishable from a speed of zero.
//
// Player already carries exactly this pair -- vel_x with rem_x, vel_y with
// rem_y -- and keeping the whole part an integer with the fraction separate is
// what avoids the class of float-edge bugs where a box is a fraction into a
// wall. The same split is what a moving cell needs, in a byte instead of four.

// Speeds. Ceiling is Grid::MAX_FALL_SPEED, which the two limits deliberately
// agree on.
constexpr int SPEED_CEILING_CELLS_PER_STEP = 8;

void report_requirement() {
    std::printf("2. WHAT IT HAS TO HOLD - two quantities per axis, not one\n\n");

    std::printf("   position is a cell index, so a velocity finer than 1 cell/step needs a\n"
                "   sub-cell remainder to spend it into. Player carries vel_x+rem_x and\n"
                "   vel_y+rem_y for exactly this reason. Per axis, then:\n\n");
    std::printf("     velocity   range +/-%d cells/step, some fraction\n", SPEED_CEILING_CELLS_PER_STEP);
    std::printf("     remainder  range [0,1) of a cell, same fraction\n\n");

    struct Split { const char* name; int int_bits; int frac_bits; };
    const Split splits[] = {
        { "4.0 (packed 4+4, both axes in `ticks`)     ", 4, 0 },
        { "4.4 signed, one byte per axis              ", 4, 4 },
        { "3.5 signed, one byte per axis              ", 3, 5 },
    };

    std::printf("   %-46s %-14s %-16s %s\n", "split", "top speed", "finest speed", "finest, cells/s");
    for (const Split& s : splits) {
        const double unit = 1.0 / static_cast<double>(1 << s.frac_bits);
        const double top = static_cast<double>((1 << (s.int_bits + s.frac_bits - 1)) - 1) * unit;
        std::printf("   %-46s %-14.4f %-16.4f %.1f\n", s.name, top, unit, unit * 60.0);
    }

    // The number that decides section 3 before a single trajectory is flown.
    const double g_per_step = 500.0 / (60.0 * 60.0);
    std::printf("\n   gravity, in the same units: Player::GRAVITY is 500 cells/s^2, so one step\n"
                "   of it is 500/3600 = %.5f cells/step. A representation whose finest\n"
                "   expressible speed is coarser than that cannot accumulate gravity at all -\n"
                "   the increment truncates to zero every step, forever.\n\n", g_per_step);
    for (const Split& s : splits) {
        const double unit = 1.0 / static_cast<double>(1 << s.frac_bits);
        std::printf("     %-46s gravity = %6.3f units/step  %s\n", s.name,
                    g_per_step / unit,
                    g_per_step / unit < 1.0 ? "<-- truncates to 0: no acceleration"
                                            : "accumulates");
    }
    std::printf("\n");
}

// ---------------------------------------------------------------------------
// 3. Trajectories
// ---------------------------------------------------------------------------
//
// One throw, flown under five arithmetics. Down is +y, as everywhere else in
// this engine, and the launch is up and to the right at 45 degrees.
//
// The reference is fx 16.16, not a float. A float reference would be the one
// thing fixed.h exists to keep out of this project, and it would also be
// measuring the candidates against a number that is itself machine-dependent.
// 16.16 has thousands of times the resolution of the finest candidate here,
// which is what makes it usable as ground truth.

constexpr int MAX_STEPS = 600;

// Launch: 4 cells/step on each axis, upward. Fast enough to be a thrown thing
// rather than a nudge, and comfortably inside every candidate's range so the
// comparison is about resolution rather than about clipping.
constexpr int LAUNCH_CELLS_PER_STEP = 4;

// Gravity as an exact rational, in the units each candidate works in.
//   500 cells/s^2 / 3600 = 5/36 cells/step^2.
constexpr int G_NUM = 5;
constexpr int G_DEN = 36;

struct Flight {
    const char* name;
    int steps_aloft = 0;  // until it comes back to launch height
    int apex_cells = 0;  // highest cell reached above launch, in cells
    int range_cells = 0;  // horizontal cells travelled before returning
    int max_dev = 0;  // largest distance from the reference, in cells
};

// The reference. Position and velocity both in fx cells; one step is
// `vel += g; pos += vel`, the same order Player::update integrates in.
struct Reference {
    fx::v x = 0, y = 0;
    fx::v vx = fx::from_int(LAUNCH_CELLS_PER_STEP);
    fx::v vy = fx::from_int(-LAUNCH_CELLS_PER_STEP);
    static constexpr fx::v G = fx::from_ratio(G_NUM, G_DEN);

    void step() { vy += G; x += vx; y += vy; }
    int cell_x() const { return fx::trunc(x); }
    int cell_y() const { return fx::trunc(y); }
};

// Candidate A -- a packed velocity: four bits of vx and four of vy inside
// `ticks`, both signed, whole cells per step. No fraction anywhere, so there is
// nothing for a gravity increment to land in.
struct PackedInt {
    int x = 0, y = 0;
    int vx = LAUNCH_CELLS_PER_STEP;
    int vy = -LAUNCH_CELLS_PER_STEP;

    void step() {
        // The whole of the finding, in one line: 5/36 of a cell per step, added
        // to an integer, is zero.
        vy += G_NUM / G_DEN;
        x += vx;
        y += vy;
    }
    int cell_x() const { return x; }
    int cell_y() const { return y; }
};

// Candidate D -- the same single byte, with the gravity increment applied
// stochastically instead of truncated away. "+1 cell/step with probability 5/36"
// has the right mean acceleration and costs no storage, and it is the project's
// existing idiom: a probability per step resolved by the deterministic hash.
//
// Deterministic, because the draw is a pure function of seed, step and cell
// index, so two runs of the same world agree exactly. What it is not is uniform:
// two grains launched identically from different cells get different gravity,
// which is what the spread figure below measures.
struct PackedIntStochastic {
    int x = 0, y = 0;
    int vx = LAUNCH_CELLS_PER_STEP;
    int vy = -LAUNCH_CELLS_PER_STEP;
    uint64_t seed;
    uint64_t index;
    int step_n = 0;

    void step() {
        // 5/36 as a per-myriad probability.
        if (sim_random::chance_per_myriad(G_NUM * 10000 / G_DEN, seed,
                                          static_cast<uint64_t>(step_n), index,
                                          sim_random::Stream::PowderDirection))
            vy += 1;
        x += vx;
        y += vy;
        ++step_n;
    }
    int cell_x() const { return x; }
    int cell_y() const { return y; }
};

// Candidates B and C -- a byte per axis as signed 4.4, plus a nibble per axis of
// sub-cell remainder. Position, velocity and remainder are all held in
// sixteenths of a cell, which is precisely what those bit widths mean.
//
// The two differ only in how the gravity increment is rounded, and that is the
// point of running both:
//
//   B truncates it. 5/36 of a cell is 2.222 sixteenths and an integer add can
//     only carry 2, so gravity comes out 10% light -- permanently, in one
//     direction, on every thrown thing in the world.
//
//   C spreads it across steps by differencing a running total taken from the
//     global step counter, which is Bresenham's line algorithm applied to an
//     acceleration. The increment alternates 2,2,2,2,3,... and its mean is
//     exactly 5/36. It needs no per-cell accumulator -- the step number is state
//     the engine already has -- and the error against the exact total never
//     exceeds one sixteenth of a cell per step and never accumulates.
struct Fixed44 {
    static constexpr int UNIT = 16;  // sixteenths of a cell
    static constexpr int GRAVITY_TRUNC = (G_NUM * UNIT) / G_DEN;  // = 2

    int x_u = 0, y_u = 0;  // position, in sixteenths
    int vx_u = LAUNCH_CELLS_PER_STEP * UNIT;
    int vy_u = -LAUNCH_CELLS_PER_STEP * UNIT;
    int step_n = 0;
    bool bresenham;

    // floor(n * 5/36 * 16) = floor(n * 20 / 9), differenced.
    //
    // `n` is the global step counter, not a per-cell one, which is why this
    // costs no storage -- and it means a cell launched on an arbitrary step
    // starts at an arbitrary phase of the 2,2,2,2,3 pattern. The phase sweep
    // below checks that this is a bounded error rather than a per-launch
    // lottery.
    static int gravity_at(int n) {
        return ((n + 1) * G_NUM * UNIT) / G_DEN - (n * G_NUM * UNIT) / G_DEN;
    }

    void step() {
        vy_u += bresenham ? gravity_at(step_n) : GRAVITY_TRUNC;
        x_u += vx_u;
        y_u += vy_u;
        ++step_n;
    }
    // Truncation toward zero, matching fx::trunc -- not a shift, for the reason
    // fixed.h pins with a static_assert.
    int cell_x() const { return x_u / UNIT; }
    int cell_y() const { return y_u / UNIT; }
};

template <typename Candidate>
Flight fly(const char* name, Candidate c, const int* ref_x, const int* ref_y) {
    Flight f;
    f.name = name;
    int apex = 0;

    for (int i = 0; i < MAX_STEPS; ++i) {
        c.step();
        const int cx = c.cell_x();
        const int cy = c.cell_y();

        if (cy < apex) apex = cy;

        const int dx = cx - ref_x[i];
        const int dy = cy - ref_y[i];
        const int dev = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (dev > f.max_dev) f.max_dev = dev;

        // Back to launch height, having actually gone up first.
        if (cy >= 0 && i > 0) {
            f.steps_aloft = i + 1;
            f.range_cells = cx;
            break;
        }
    }
    f.apex_cells = -apex;
    if (f.steps_aloft == 0) {
        // It never came back down inside the window, which is itself the answer
        // for one of these candidates.
        f.steps_aloft = -1;
        f.range_cells = -1;
    }
    return f;
}

void report_trajectories() {
    std::printf("3. TRAJECTORIES - one throw, five arithmetics\n\n");
    std::printf("   Launched at %d cells/step on each axis, up and to the right, under\n"
                "   Player::GRAVITY (500 cells/s^2 = 5/36 cells per step per step).\n"
                "   Reference is fx 16.16, the arithmetic Player itself uses.\n\n",
                LAUNCH_CELLS_PER_STEP);

    // The reference trace first, so every candidate is measured against the same
    // stored path rather than against a re-run of it.
    static int ref_x[MAX_STEPS];
    static int ref_y[MAX_STEPS];
    Reference ref;
    int ref_steps = -1, ref_apex = 0, ref_range = -1;
    for (int i = 0; i < MAX_STEPS; ++i) {
        ref.step();
        ref_x[i] = ref.cell_x();
        ref_y[i] = ref.cell_y();
        if (ref_y[i] < ref_apex) ref_apex = ref_y[i];
        if (ref_y[i] >= 0 && ref_steps < 0) { ref_steps = i + 1; ref_range = ref_x[i]; }
    }
    // Fill the tail so a candidate that outlives the reference still has
    // something to be compared against rather than reading past the array.
    for (int i = 0; i < MAX_STEPS; ++i)
        if (ref_steps >= 0 && i >= ref_steps) { ref_x[i] = ref_x[ref_steps - 1]; ref_y[i] = ref_y[ref_steps - 1]; }

    Flight flights[4];
    flights[0] = fly("A  packed 4+4 int, 1 byte  ", PackedInt{}, ref_x, ref_y);
    PackedIntStochastic d;
    d.seed = 12345;
    d.index = 777;
    flights[1] = fly("D  A + stochastic gravity  ", d, ref_x, ref_y);
    Fixed44 b; b.bresenham = false;
    flights[2] = fly("B  4.4 + remainder, 3 bytes", b, ref_x, ref_y);
    Fixed44 cc; cc.bresenham = true;
    flights[3] = fly("C  B + stepwise gravity    ", cc, ref_x, ref_y);

    std::printf("   %-28s %10s %8s %8s %10s\n", "candidate", "steps up", "apex", "range", "worst dev");
    std::printf("   %-28s %10d %8d %8d %10s\n", "REF  fx 16.16              ",
                ref_steps, -ref_apex, ref_range, "-");
    for (const Flight& f : flights) {
        if (f.steps_aloft < 0)
            std::printf("   %-28s %10s %8d %8s %10d   <-- never came back down in %d steps\n",
                        f.name, "never", f.apex_cells, "-", f.max_dev, MAX_STEPS);
        else
            std::printf("   %-28s %10d %8d %8d %10d\n",
                        f.name, f.steps_aloft, f.apex_cells, f.range_cells, f.max_dev);
    }

    // C's phase. The increment pattern repeats every 9 steps and the engine
    // would read it off the global step counter, so a cell launched on an
    // arbitrary step lands on an arbitrary phase of it. If that made the
    // trajectory depend on when the throw happened, C would be a worse version
    // of D rather than the answer -- so it is flown at all nine.
    int worst_dev = 0, worst_phase = 0, min_range = 1 << 30, max_range = -(1 << 30);
    for (int phase = 0; phase < 9; ++phase) {
        Fixed44 p;
        p.bresenham = true;
        p.step_n = phase;
        const Flight f = fly("", p, ref_x, ref_y);
        if (f.max_dev > worst_dev) { worst_dev = f.max_dev; worst_phase = phase; }
        if (f.range_cells < min_range) min_range = f.range_cells;
        if (f.range_cells > max_range) max_range = f.range_cells;
    }
    std::printf("\n   C, the same throw launched at each of the 9 gravity phases:\n"
                "     range %d..%d cells (spread %d), worst deviation %d cells at phase %d\n",
                min_range, max_range, max_range - min_range, worst_dev, worst_phase);

    // Candidate D, for contrast. Its error is not a bias, it is noise, and
    // mean-correct noise is exactly what a single trajectory flatters. Fire 64
    // identical grains from 64 different cell indices: if a shove or an
    // explosion hands the same impulse to 64 neighbouring cells, this is what
    // comes back.
    int d_min = 1 << 30, d_max = -(1 << 30);
    long long sum = 0;
    int counted = 0;
    for (int i = 0; i < 64; ++i) {
        PackedIntStochastic g;
        g.seed = 12345;
        g.index = static_cast<uint64_t>(1000 + i);
        const Flight f = fly("", g, ref_x, ref_y);
        if (f.range_cells < 0) continue;
        if (f.range_cells < d_min) d_min = f.range_cells;
        if (f.range_cells > d_max) d_max = f.range_cells;
        sum += f.range_cells;
        ++counted;
    }
    if (counted > 0)
        std::printf("\n   D, 64 grains given the identical impulse from 64 different cells:\n"
                    "     range %d..%d cells (mean %lld, spread %d cells, %.0f%% of the mean)\n",
                    d_min, d_max, sum / counted, d_max - d_min,
                    100.0 * static_cast<double>(d_max - d_min) /
                        (static_cast<double>(sum) / counted));

    // And the case no trajectory above reaches, which is not a throw -- it is a
    // shove and debris coming to rest. Under a whole-cell velocity a cell asked
    // to move at half a cell per step has two available speeds: none, or sixty
    // cells a second.
    std::printf("\n   the slow case:\n"
                "     a shove of 0.5 cells/step (30 cells/s) is 0 under A and D - a cell that\n"
                "     is pushed and does not move - and exact under B and C. A's slowest\n"
                "     non-zero speed is 1 cell/step = 60 cells/s, 15%% of the player's own\n"
                "     terminal velocity, so under A nothing can be nudged, only launched.\n\n");
}

} // namespace

int main() {
    std::printf("\n=== Element::ticks representation probe ===\n"
                "Evaluation of what `ticks` can represent. Prints numbers; decides nothing.\n\n");
    report_layout();
    report_requirement();
    report_trajectories();
    return 0;
}
