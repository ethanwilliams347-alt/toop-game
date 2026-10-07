#pragma once

// Integer arithmetic shared by more than one file under src/physics/, where a
// float result is not allowed to decide anything. See fixed.h for why.
namespace int_math {

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
inline long long isqrt(long long v) {
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
inline long long div_round(long long a, long long b) {
    return a >= 0 ? (2 * a + b) / (2 * b) : -((-2 * a + b) / (2 * b));
}

}  // namespace int_math
