#pragma once
#include <cstdint>

// The GL frame under the multiply light model, as an ordered table of passes and
// the targets each reads and writes. Header-only and constexpr, so its order
// rules are static_asserts: a pass moved, added or rewired so that it reads a
// target nothing has written yet fails the build, not the frame.
//
//   backdrop  -> albedo                     frame::compose, the backdrop layers
//   world     -> albedo, emissive           frame::compose, cells to foreground
//   light     pack -> blocks, sun           GPU port of LightField + sun march
//   resolve   albedo, emissive, blocks, sun -> hdr   albedo x light + emissive
//   weather   hdr -> hdr                    R6; draws nothing yet
//   post      hdr -> frame                  linear back to display; R5 adds
//                                           tonemap, bloom, grade, fog, grain
//   ui        frame -> frame                overlay::draw, after the world
//
// The first two and the last are drawn by the CPU-side frame code through
// gfx::Canvas (GlCanvas routes those draws to the albedo/emissive pair while
// a lit frame is open); gpu::Pipeline runs the four in between, in this
// table's order (pipeline.cpp walks it). The legacy light model never opens a
// lit frame and draws exactly what the SDL path does, so none of this exists
// for it.
//
// The rules, each asserted below:
//   - nothing is read before something writes it (the pack is uploaded before
//     the frame, so it starts written);
//   - exactly one Resolve: it is the boundary between albedo space and linear
//     light, and two would light the frame twice;
//   - UI comes after Post: the HUD is not in the world and is drawn over the
//     finished image, never lit, graded or fogged (frame::LAYERS' rule, one
//     level up);
//   - every pass writes something.
namespace gpu::pass_graph {

enum Target : uint32_t {
    PACK = 1u << 0,      // the cell pack (RGBA8UI, one texel per cell)
    ALBEDO = 1u << 1,    // the world's paint, display-encoded (attachment 0)
    EMISSIVE = 1u << 2,  // linear light the world gives off (attachment 1)
    BLOCKS = 1u << 3,    // propagated fire light, one texel per LightField block
    SUN = 1u << 4,       // sun visibility, one texel per cell
    HDR = 1u << 5,       // the lit frame, linear, RGBA16F
    FRAME = 1u << 6,     // what is presented (shares the albedo texture)
};

enum class Kind : uint8_t { Canvas, Gpu, Resolve, Post, Ui };

enum class Id : uint8_t { Backdrop, World, Light, Resolve, Weather, Post, Ui };

// Pointer and masks first, the two bytes last: no padding between them.
struct Pass {
    const char* name;
    uint32_t reads;
    uint32_t writes;
    Id id;
    Kind kind;
};

inline constexpr Pass PASSES[] = {
    // name       reads                                writes              id            kind
    {"backdrop", 0, ALBEDO, Id::Backdrop, Kind::Canvas},
    {"world", PACK, ALBEDO | EMISSIVE, Id::World, Kind::Canvas},
    {"light", PACK, BLOCKS | SUN, Id::Light, Kind::Gpu},
    {"resolve", ALBEDO | EMISSIVE | BLOCKS | SUN, HDR, Id::Resolve, Kind::Resolve},
    {"weather", HDR, HDR, Id::Weather, Kind::Gpu},
    {"post", HDR, FRAME, Id::Post, Kind::Post},
    {"ui", FRAME, FRAME, Id::Ui, Kind::Ui},
};
inline constexpr int PASS_COUNT = static_cast<int>(sizeof(PASSES) / sizeof(PASSES[0]));

// Uploaded before the frame starts.
inline constexpr uint32_t EXTERNAL = PACK;

constexpr bool reads_only_written() {
    uint32_t written = EXTERNAL;
    for (const Pass& p : PASSES) {
        if ((p.reads & ~written) != 0) return false;
        written |= p.writes;
    }
    return true;
}

constexpr int count_of(Kind k) {
    int n = 0;
    for (const Pass& p : PASSES) n += p.kind == k ? 1 : 0;
    return n;
}

constexpr int index_of(Kind k) {
    for (int i = 0; i < PASS_COUNT; ++i)
        if (PASSES[i].kind == k) return i;
    return -1;
}

constexpr bool every_pass_writes() {
    for (const Pass& p : PASSES)
        if (p.writes == 0) return false;
    return true;
}

// The table's position is the pass's identity, so Id can be used as an index.
constexpr bool ids_in_order() {
    for (int i = 0; i < PASS_COUNT; ++i)
        if (static_cast<int>(PASSES[i].id) != i) return false;
    return true;
}

static_assert(reads_only_written(), "a pass reads a target no earlier pass has written");
static_assert(count_of(Kind::Resolve) == 1, "the frame needs exactly one Resolve pass");
static_assert(count_of(Kind::Post) == 1 && count_of(Kind::Ui) == 1,
              "the frame needs exactly one Post and one UI pass");
static_assert(index_of(Kind::Ui) > index_of(Kind::Post), "UI is drawn after Post, unlit");
static_assert(index_of(Kind::Post) > index_of(Kind::Resolve), "Post reads the resolved frame");
static_assert(every_pass_writes(), "a pass that writes nothing is not a pass");
static_assert(ids_in_order(), "PASSES is indexed by Id; keep them in the same order");

}  // namespace gpu::pass_graph
