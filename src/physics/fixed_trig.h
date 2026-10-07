#pragma once
#include <cstdint>
#include "fixed.h"

// Sine and cosine in 16.16, for the one part of the simulation that turns
// things: a structure tipping over its own edge (see "toppling" in grid.h).
//
// Its own header rather than more of fixed.h because nothing else needs it, and
// fixed.h is included by every file that touches a number.
//
// Integer-only for the reason fixed.h gives: std::sin is allowed to differ in the
// last bit between compilers, libms and /fp modes, and a rotation that differs by
// one bit eventually puts one cell in a different place and the replay forks.
// A polynomial evaluated with integer multiplies and truncating divides has no
// such freedom. Angles are radians in fx.
namespace fx {

// pi/2 in 16.16, rounded to nearest (102943.7). The quarter turn is the unit the
// reductions below work in, so it is the constant that is written down; a whole
// turn is four of these rather than a separately rounded value, so reducing by
// quarters and by whole turns can never disagree.
inline constexpr v HALF_PI = 102944;
inline constexpr v QUARTER_PI = HALF_PI / 2;

// a * b in 16.16, truncated toward zero. Division rather than >> for the reason
// fx::trunc gives: flooring would make mul(-a, b) != -mul(a, b), and sin is odd.
constexpr v mul(v a, v b) {
    return static_cast<v>((static_cast<int64_t>(a) * b) / ONE);
}

namespace trig_detail {
    // The series run in 2.30 rather than 16.16: five multiplies each lose up to
    // one unit in the last place, and spending those losses fourteen bits below
    // the result's resolution means none of them reaches it.
    inline constexpr int64_t Q = int64_t{1} << 30;
    constexpr int64_t mulq(int64_t a, int64_t b) { return (a * b) / Q; }

    // Taylor series, Horner form, valid for |x| <= pi/4 (in 2.30). The first
    // omitted terms are x^9/9! and x^10/10!, both under 4e-7 at pi/4 -- forty
    // times finer than one 16.16 unit -- so stopping here costs nothing visible.
    constexpr int64_t sin_q(int64_t x) {
        const int64_t x2 = mulq(x, x);
        int64_t t = Q - x2 / 72;
        t = Q - mulq(x2, t) / 42;
        t = Q - mulq(x2, t) / 20;
        t = Q - mulq(x2, t) / 6;
        return mulq(x, t);
    }
    constexpr int64_t cos_q(int64_t x) {
        const int64_t x2 = mulq(x, x);
        int64_t t = Q - x2 / 56;
        t = Q - mulq(x2, t) / 30;
        t = Q - mulq(x2, t) / 12;
        return Q - mulq(x2, t) / 2;
    }
} // namespace trig_detail

// Quarter turns in `a`, rounded to nearest, so that `a - quarters(a) * HALF_PI`
// lies within +/-pi/4 -- the range the series above are good for. Written out
// rather than as a / HALF_PI because division truncates toward zero and this
// has to round in both directions alike.
constexpr int32_t quarters(v a) {
    return a >= 0 ? (a + HALF_PI / 2) / HALF_PI : -((-a + HALF_PI / 2) / HALF_PI);
}

struct SinCos { v s; v c; };

// sin and cos of any angle. Reduced to within pi/4 of a quarter turn, evaluated
// there, then turned back by exact quarter-turn identities -- which only swap
// and negate, so they add no error.
constexpr SinCos sincos(v a) {
    const int32_t k = quarters(a);
    const v r = a - k * HALF_PI;
    const int64_t rq = static_cast<int64_t>(r) * (trig_detail::Q / ONE);
    const v s = static_cast<v>(trig_detail::sin_q(rq) / (trig_detail::Q / ONE));
    const v c = static_cast<v>(trig_detail::cos_q(rq) / (trig_detail::Q / ONE));
    switch (((k % 4) + 4) % 4) {
        case 0:  return {s, c};
        case 1:  return {c, -s};
        case 2:  return {-s, -c};
        default: return {-c, s};
    }
}

constexpr v sin(v a) { return sincos(a).s; }
constexpr v cos(v a) { return sincos(a).c; }

// Pinned the way fixed.h pins trunc. Two units of tolerance is the honest error
// of 16.16 here; anything wider means the series or the reduction is broken.
namespace trig_detail {
    constexpr bool near(v got, v want) { return got - want <= 2 && want - got <= 2; }
}
static_assert(sin(0) == 0 && cos(0) == ONE, "sin/cos are wrong at zero");
static_assert(trig_detail::near(sin(HALF_PI), ONE) && trig_detail::near(cos(HALF_PI), 0),
              "the quarter-turn reduction is wrong");
static_assert(trig_detail::near(sin(34315), ONE / 2),  // pi/6
              "the sine series is wrong inside its own range");
static_assert(trig_detail::near(cos(68629), ONE / 2),  // pi/3, which reduces across a quarter
              "cos is wrong after reduction");
static_assert(trig_detail::near(sin(QUARTER_PI), cos(QUARTER_PI)),
              "the two series disagree where they meet");
static_assert(sin(-12345) == -sin(12345) && cos(-12345) == cos(12345),
              "sin must be exactly odd and cos exactly even, or a piece tipping left "
              "and its mirror image tipping right take different paths");
static_assert(trig_detail::near(sin(2 * HALF_PI), 0) && trig_detail::near(cos(2 * HALF_PI), -ONE),
              "a half turn is wrong");

} // namespace fx
