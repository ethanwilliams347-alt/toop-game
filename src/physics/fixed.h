#pragma once
#include <cstdint>

// Signed 16.16 fixed point.
//
// Float results are not reproducible across compilers, optimisation levels or
// architectures, and the engine's determinism guarantee depends on every
// number in the simulation being reproducible. Integer arithmetic has no such
// freedom.
//
// 16 fractional bits resolves a sub-cell remainder far below one screen pixel;
// 16 integer bits holds a velocity in cells per second (~500 at its largest)
// against a range of +/-32767. Neither half is close to its limit.
namespace fx {

// int32_t rather than int: the width is part of the contract, and a 16-bit int
// would overflow at four cells.
using v = int32_t;

inline constexpr int FRAC_BITS = 16;
inline constexpr v ONE = 1 << FRAC_BITS;

// The fixed simulation rate. `Run::FIXED_DT` is the same number written as a
// duration for the frame pacer; the static_assert in run.h keeps them in step.
inline constexpr int STEPS_PER_SECOND = 60;

// Build a value from an exact rational at compile time. Constants are written
// `from_ratio(225, 2)` rather than as a float literal cast, so no float appears
// anywhere in the chain that produces them.
constexpr v from_ratio(int64_t num, int64_t den) {
    return static_cast<v>((num * ONE) / den);
}

constexpr v from_int(int32_t n) { return n * ONE; }

// Whole part, truncated toward zero rather than floored. The sub-cell remainder
// scheme needs the same rounding in both directions: a body moving left by 0.6
// of a cell must take zero whole steps, exactly as one moving right by 0.6 does.
// Flooring would give a step of -1 and a remainder of +0.4, drifting the body
// left while it stands still.
constexpr int32_t trunc(v a) { return a / ONE; }

// The fractional part left over after `trunc`, carrying the sign of `a`.
constexpr v frac(v a) { return a - trunc(a) * ONE; }

// For rendering and test output only. Nothing in src/physics/ may use this.
constexpr float to_float(v a) { return static_cast<float>(a) / static_cast<float>(ONE); }

// A per-second rate as a per-step amount. One integer division, so it is exact
// on every machine, and it truncates toward zero, symmetric about zero.
//
// The error is under 1/65536 of a unit per step and does not accumulate: the
// truncated value is a constant, so adding it a thousand times is a thousand
// times one exact number rather than a thousand roundings.
constexpr v per_step(v per_second) { return per_second / STEPS_PER_SECOND; }

// `a >> 16` is faster than `a / 65536` and is not the same function: it floors.
// A body drifting left at half a cell per step would then take a whole-cell step
// every step and grow a positive remainder. Asserted on the pair, because what
// matters is that both directions agree.
static_assert(trunc(from_ratio(3, 2)) == 1 && trunc(from_ratio(-3, 2)) == -1,
              "fx::trunc must round toward zero in both directions, not floor");
static_assert(frac(from_ratio(3, 2)) == ONE / 2 && frac(from_ratio(-3, 2)) == -(ONE / 2),
              "fx::frac must carry the sign of its argument, or a body moving left "
              "accumulates a remainder pointing right");
static_assert(from_ratio(225, 2) == 112 * ONE + ONE / 2, "from_ratio is not exact");

} // namespace fx
