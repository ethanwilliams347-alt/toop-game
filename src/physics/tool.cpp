#include "tool.h"
#include <algorithm>
#include <cstdlib>

namespace {

// floor(sqrt(v)) for v >= 0, digit by digit in binary. Exact: it settles on the
// largest root with root*root <= v.
//
// Not std::sqrt cast to int. A library sqrt is a float result, and a float
// result one bit low turns an exact square into the integer below it on one
// toolchain and not on another -- and this picks which cells a dig deletes.
//
// Binary rather than Newton because Newton runs an idiv per iteration and this
// runs none: shifts, adds and compares only. That makes its cost flat and its
// result obviously the same everywhere, which matters more here than speed does.
long long isqrt(long long v) {
    if (v <= 0) return 0;
    unsigned long long n = static_cast<unsigned long long>(v);
    unsigned long long root = 0;
    unsigned long long bit = 1ULL << 62;  // highest even power of four in 64 bits

    while (bit > n) bit >>= 2;
    while (bit != 0) {
        if (n >= root + bit) {
            n -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return static_cast<long long>(root);
}

// a/b rounded to nearest with halves away from zero, for b > 0. The integer
// replacement for std::lround(float(a) / b), and it agrees with it exactly:
// doubling both sides is what lets the half be compared without a fraction.
long long div_round(long long a, long long b) {
    return a >= 0 ? (2 * a + b) / (2 * b) : -((-2 * a + b) / (2 * b));
}

} // namespace

void DigTool::march(const Grid& grid, int from_x, int from_y, int aim_x, int aim_y,
                    int& out_x, int& out_y, bool& out_hit) const {
    out_hit = false;
    out_x = from_x;
    out_y = from_y;

    const int dx = aim_x - from_x;
    const int dy = aim_y - from_y;

    const int span = std::max(std::abs(dx), std::abs(dy));
    if (span == 0) return;  // aiming at your own feet

    // Range is a real distance, not a cell count along the dominant axis --
    // otherwise a diagonal dig would reach ~1.4x as far as a straight one.
    //
    // Integer-only, which is why this reads more awkwardly than sqrt would. This
    // picks which cells a dig deletes, and digging writes to the grid, so a last-bit
    // difference from excess precision, an FMA contraction or a fast-math flag is a
    // different world on another toolchain, not a different pixel. (swing_progress()
    // below is still a float and is meant to be: only the animation reads it.)
    //
    // The comparison is squared so no length is ever taken:
    //   len <= RANGE   <=>   dx*dx + dy*dy <= RANGE*RANGE
    // and when the aim is out of range, the truncation a float cast would do is kept
    // exactly, without dividing by an irrational length:
    //   floor(span * RANGE / len) = floor(sqrt(span*span * RANGE*RANGE / dist2))
    // which is isqrt of an integer quotient, because floor(sqrt(x)) is
    // floor(sqrt(floor(x))) for x >= 0.
    //
    // 64-bit throughout. span*span * RANGE*RANGE overflows a 32-bit int for a cursor
    // a couple of thousand cells away, which is an ordinary aim in a big world.
    const long long dist2 = static_cast<long long>(dx) * dx + static_cast<long long>(dy) * dy;
    const long long range2 = static_cast<long long>(RANGE) * RANGE;

    const long long span2 = static_cast<long long>(span) * span;
    const int steps = (dist2 <= range2)
                          ? span
                          : static_cast<int>(isqrt(span2 * range2 / dist2));

    for (int i = 1; i <= steps; ++i) {
        // One cell per iteration along the dominant axis, so the path is
        // connected and nothing is skipped over.
        const int cx = from_x + static_cast<int>(div_round(static_cast<long long>(dx) * i, span));
        const int cy = from_y + static_cast<int>(div_round(static_cast<long long>(dy) * i, span));

        out_x = cx;
        out_y = cy;

        // The same is_solid() the player collides against, so what blocks a dig is
        // exactly what blocks a body -- terrain and powders stop the ray, water and
        // fire do not. One definition, used twice.
        //
        // get_element() reads out-of-bounds as Wall, so the ray stops at the world
        // border rather than running off forever. The border still cannot be dug
        // through: set_element() bounds-checks, so the part of the hole that lies
        // outside the world is silently dropped.
        if (is_solid(grid.get_element(cx, cy).type)) {
            out_hit = true;
            return;
        }
    }
}

void DigTool::aim_point(const Grid& grid, int from_x, int from_y, int aim_x, int aim_y,
                        int& out_x, int& out_y) const {
    bool hit = false;
    march(grid, from_x, from_y, aim_x, aim_y, out_x, out_y, hit);
}

bool DigTool::update(Grid& grid, bool held, int from_x, int from_y, int aim_x, int aim_y) {
    // A swing that has run its length is over before this step is considered, so a
    // held button starts the next one on the same step the last finished and the
    // cycle has no seam in it. A one-step gap at the top of every swing is a stutter
    // at 60 Hz.
    if (swing >= 0 && ++swing >= SWING_STEPS) swing = -1;

    // Mid-swing is follow-through: the blow already landed on the step the swing
    // opened, and these steps exist so the animation has somewhere to play out and
    // so the next blow cannot arrive immediately.
    if (swing >= 0) return false;
    if (!held) return false;

    int hit_x = 0;
    int hit_y = 0;
    bool hit = false;
    march(grid, from_x, from_y, aim_x, aim_y, hit_x, hit_y, hit);

    // Swinging at thin air costs nothing and starts nothing. Only a connecting shot
    // commits the body to a swing, which makes the rate feel like a tool speed
    // rather than an input lockout, and means the player is not locked into a full
    // swing's animation for a shot that was never going to hit anything.
    if (!hit) return false;

    swing = 0;

    // This deletes matter outright, which is correct for a tool and would be a bug
    // anywhere inside the simulation step -- see the conservation-of-matter test,
    // which covers Grid::update() and is testing a different thing.
    //
    // Going through set_element() rather than touching cells[] is what wakes the 3x3
    // neighbourhood of every removed cell, and that is why digging under a settled
    // pile makes the pile fall instead of leaving it hanging over the hole.
    for (int oy = -RADIUS; oy <= RADIUS; ++oy) {
        for (int ox = -RADIUS; ox <= RADIUS; ++ox) {
            if (ox * ox + oy * oy > RADIUS * RADIUS) continue;
            grid.set_element(hit_x + ox, hit_y + oy, ElementType::Empty);
        }
    }

    return true;
}
