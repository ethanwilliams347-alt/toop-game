#pragma once
#include <cstdint>

// Deterministic randomness for the simulation.
//
// There is no generator and no state. A random value is a pure function of
// where and when it is asked for, so a run reproduces from nothing but a seed
// and a step count and a save file carries no generator state.
//
// It also means randomness does not depend on how many times it has been asked
// for. Adding a roll anywhere would, with a stateful generator, shift every
// subsequent value in the world; here each decision draws from its own
// coordinates, so behaviour changes stay local.
namespace sim_random {

// Every distinct decision draws from its own stream.
//
// Two decisions taken about the same cell on the same step would otherwise be
// the same number and correlate permanently. One tag per decision costs one xor
// and removes the problem.
//
// The values are arbitrary but must never change: changing one changes every
// world its seed ever produced. They are large odd constants sharing no
// structure, so the tag mixes rather than merely offsets.
enum class Stream : uint64_t {
    ColorJitter     = 0x9E3779B97F4A7C15ull,
    SweepDirection  = 0xD1B54A32D192ED03ull,
    PowderDirection = 0xA0761D6478BD642Full,
    FluidDirection  = 0xE7037ED1A0B428DBull,
    Reaction        = 0x8EBC6AF09C88C6E3ull,
    Fracture        = 0xC2B2AE3D27D4EB4Full,
    // Whether a burning cell throws a flame this step, and which empty neighbour it
    // goes to. Two decisions on one stream, separated by index rather than by tag;
    // see emit_flame().
    Emission        = 0xF1BBCDCBB3D935A7ull,
    // How long one flame lives. Its own stream because it is drawn about the new
    // cell at the moment of emission; sharing would correlate a flame's lifetime
    // with whether its parent chose to emit at all.
    FlameLifetime   = 0xB5026F5AA96619E9ull,
    // Whether a flame rises this step. Per cell per step, so it must not share with
    // anything else asked about the same cell on the same step.
    FlameRise       = 0xCA9E6D9C1B5D4C77ull,
    // How far this spot's ignition point sits from its material's stated figure.
    // Drawn with authored_spread, pinned at step 0, because it is a property of the
    // material at that coordinate rather than a decision retaken every step.
    IgnitionPoint   = 0xD6E8FEB86659FD93ull,
    // How long one steam cell lasts before it condenses. Its own stream for the
    // same reason FlameLifetime has one.
    SteamLifetime   = 0xA3B195354A39B70Dull,
};

// splitmix64's finalizer. Not cryptographic, and does not need to be: the
// requirement is that inputs one apart give unrelated outputs. The inputs here
// are consecutive cell indices and step numbers, so a weak mix shows up as
// diagonal banding in falling powder.
inline constexpr uint64_t mix(uint64_t x) {
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// The one place the four inputs are combined. Each varying input is multiplied
// by a large odd constant before being folded in, spreading its low bits -- the
// ones that change from cell to cell -- across the whole word before the mix.
inline constexpr uint64_t bits(uint64_t seed, uint64_t step, uint64_t index, Stream stream) {
    uint64_t x = seed ^ static_cast<uint64_t>(stream);
    x = mix(x ^ (step * 0x9E3779B97F4A7C15ull));
    x = mix(x ^ (index * 0xD1B54A32D192ED03ull));
    return x;
}

// A 50/50 decision. Reads the low bit, which is safe because the mixer's last
// step folds the high half down into it.
inline constexpr bool coin(uint64_t seed, uint64_t step, uint64_t index, Stream stream) {
    return (bits(seed, step, index, stream) & 1ull) != 0;
}

// True with probability pct/100. The modulo bias is on the order of 1 in 2^57.
inline constexpr bool chance(int pct, uint64_t seed, uint64_t step, uint64_t index, Stream stream) {
    if (pct <= 0) return false;
    if (pct >= 100) return true;
    return static_cast<int>(bits(seed, step, index, stream) % 100ull) < pct;
}

// True with probability per_myriad/10000, for decisions too rare to express in
// whole percents. Mean lifetime of a spontaneous decay is 10000/per_myriad
// steps. The resolution a tuning column needs is set by the smallest change
// anyone will want to make to it, not by the largest value it has to hold.
inline constexpr bool chance_per_myriad(int per_myriad, uint64_t seed, uint64_t step, uint64_t index, Stream stream) {
    if (per_myriad <= 0) return false;
    if (per_myriad >= 10000) return true;
    return static_cast<int>(bits(seed, step, index, stream) % 10000ull) < per_myriad;
}

// A value in [0, n), for choosing one of several neighbours.
inline constexpr int pick(int n, uint64_t seed, uint64_t step, uint64_t index, Stream stream) {
    if (n <= 1) return 0;
    return static_cast<int>(bits(seed, step, index, stream) % static_cast<uint64_t>(n));
}

// A value in [-range, range], for symmetric offsets such as colour jitter.
inline constexpr int spread(int range, uint64_t seed, uint64_t step, uint64_t index, Stream stream) {
    if (range <= 0) return 0;
    const uint64_t span = static_cast<uint64_t>(2 * range + 1);
    return static_cast<int>(bits(seed, step, index, stream) % span) - range;
}

// ---------------------------------------------------------------------------
// Reserved for world generation. Nothing draws from this yet.
//
// World generation draws only from streams minted by worldgen(), and the
// simulation only from the tags declared above. Neither borrows the other's: a
// generator sharing simulation streams would mean that generating one extra
// cave changes the values the simulation reads for those cells, so a terrain
// tweak would silently alter how sand falls elsewhere.
//
// Two notes for whoever writes the generator:
//
//   - Generation is authored, not per-step. Pass step 0, as colour jitter does,
//     so a feature's shape does not depend on which step generated it.
//   - `index` means "which thing am I asking about", not necessarily a cell. A
//     draw about the seventh cave passes 7.
inline constexpr Stream worldgen(uint64_t n) {
    return static_cast<Stream>(mix(0x5851F42D4C957F2Dull ^ (n * 0x2545F4914F6CDD1Dull)));
}

// Every simulation stream, listed once so the check below can see them all. A
// new tag has to be added here as well as to the enum.
inline constexpr Stream SIM_STREAMS[] = {
    Stream::ColorJitter,
    Stream::SweepDirection,
    Stream::PowderDirection,
    Stream::FluidDirection,
    Stream::Reaction,
    Stream::Fracture,
    Stream::Emission,
    Stream::FlameLifetime,
    Stream::FlameRise,
    Stream::IgnitionPoint,
    Stream::SteamLifetime,
};

// Two streams sharing a value are one stream, silently, with no assertion that
// would catch it at runtime -- so it is checked at compile time. Covers the
// declared tags against each other and against the first sixteen minted
// generation streams.
namespace detail {
constexpr bool streams_distinct() {
    constexpr int sim_n = sizeof(SIM_STREAMS) / sizeof(SIM_STREAMS[0]);
    constexpr int gen_n = 16;
    uint64_t all[sim_n + gen_n] = {};
    for (int i = 0; i < sim_n; ++i) all[i] = static_cast<uint64_t>(SIM_STREAMS[i]);
    for (int i = 0; i < gen_n; ++i)
        all[sim_n + i] = static_cast<uint64_t>(worldgen(static_cast<uint64_t>(i)));

    for (int i = 0; i < sim_n + gen_n; ++i)
        for (int j = i + 1; j < sim_n + gen_n; ++j)
            if (all[i] == all[j]) return false;
    return true;
}
} // namespace detail

static_assert(detail::streams_distinct(), "two streams share a value and are therefore one stream");

} // namespace sim_random
