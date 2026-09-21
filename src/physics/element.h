#pragma once
#include <cstdint>
#include "material.h"

// What an undisturbed cell sits at, and what the world starts at. The scale
// reads as degrees Celsius so the numbers in MATERIALS and REACTIONS mean
// something to a person: 20 is room temperature, water boils at 100, and 255 is
// a little above what an open flame holds. That is a naming convenience, not a
// physical claim -- nothing here models energy, only a number that flows
// downhill between neighbours.
inline constexpr uint8_t AMBIENT_TEMPERATURE = 20;

struct Element {
    ElementType type = ElementType::Empty;

    // ARGB8888, base colour plus per-cell jitter.
    //
    // Transparent, because the default type is Empty and the two have to agree.
    // swap_elements moves whole Elements, so a grain leaving a cell swaps a
    // default-constructed Empty into its place; an opaque default would paint every
    // cell anything ever moved through. The colour a cell renders as belongs to
    // MATERIALS, and the static_assert below stops the two drifting apart.
    uint32_t color = 0x00000000;

    // The frame tag of the step this cell was last visited in. A cell is skipped
    // when this equals the grid's current tag, which stops it moving twice in one
    // sweep. Storing the tag rather than a bool means there is no per-step pass to
    // reset it, which matters once most of the world is asleep.
    uint8_t updated_tag = 0;

    // A per-cell step counter whose meaning is set by the cell's role. Two things
    // need one and neither can afford a byte of its own:
    //
    //  - For a structural cell it is steps spent in unbroken free fall, which is
    //    what a falling piece's speed and its fracture threshold are read from. It
    //    lives on the cell rather than on the piece because a piece has no identity
    //    between steps -- it is re-discovered by flood fill every time.
    //  - For a Fire cell it is fuel remaining: steps of burning still owed by
    //    whatever this flame came from, seeded at ignition out of the burning
    //    material's burn_duration.
    //  - For a Steam cell it is steps until it condenses. The same role as Fire's
    //    and deliberately not a third one: both are gases whose whole existence is
    //    a countdown, and neither can ever be structural.
    //
    // Sharing is safe because the roles cannot occur in the same cell: a lifetime
    // belongs to a Gas and a Gas is never structural, which TickRole below asserts
    // rather than trusts. Every transition between roles goes through place(),
    // which builds a fresh Element and so zeroes this -- a Wood cell that has been
    // falling does not carry its fall ticks over as fuel when it ignites.
    //
    // Zero for everything else. Powders and fluids move one cell per step by their
    // own rules and have no use for a clock.
    uint8_t ticks = 0;

    // How hot this cell is. Ambient unless something has heated it, and it decays
    // back to ambient on its own, so a world nobody has set fire to holds one value
    // everywhere and generates no thermal work at all.
    //
    // On the cell rather than the material because it is the one thermal quantity
    // that varies per cell, and it moves with the cell for free since swap_elements
    // moves whole Elements: a grain pulled out of a fire carries its heat with it.
    uint8_t temperature = AMBIENT_TEMPERATURE;

    // Which broken-off piece of structure this cell belongs to. Zero for everything
    // that has never been fractured, which is almost everything.
    //
    // The support flood fill only crosses between two structural cells whose tags
    // match, so a crack is stored as a disagreement between two cells rather than
    // as a line between them. That is why this is a per-cell field and not a side
    // table of edges: cracks have to survive the piece moving, and a whole Element
    // is what swap_elements carries.
    //
    // Fracture is the only thing that writes a non-zero value here, and it never
    // writes one back to zero: two pieces that have come apart do not re-weld by
    // touching. The counter is a byte and wraps, so two unrelated pieces can end up
    // sharing a tag and welding if they meet. That is a missed fracture, which is
    // the harmless direction.
    uint8_t piece_tag = 0;
};

// ticks and temperature are free: they sit in padding the struct already had
// after updated_tag. Worth pinning down, because they would stop being free the
// moment they pushed the struct over an alignment boundary -- this array is one
// entry per cell of the world and every step walks all of the awake part of it,
// so a byte here is 500 KB and a slower sweep at the target resolution.
//
// type(1) + 3 pad + color(4) + updated_tag(1) + ticks(1) + temperature(1) +
// piece_tag(1) is 12 bytes. The 3 pad is real space: `type` is one byte and
// `color` needs four-byte alignment, so offsets 1-3 are a hole that a field
// declared between them occupies for free. Three bytes remain there. Appending
// after piece_tag is not a one-byte option -- alignment rounds 13 up to 16, so
// it costs four bytes per cell.
//
// The assertion below is what makes the front hole safe to use on a machine
// whose ABI packs this struct differently: a layout with no hole fails the
// build here instead of silently growing.
static_assert(sizeof(Element) <= 12, "Element grew - price the extra memory traffic before accepting it");

// A default-constructed cell must render as what its default type renders as.
//
// The two are written in different files by different kinds of edit -- a member
// initialiser here, a row in MATERIALS there -- and nothing about touching
// either suggests reading the other. Data-driven design moves the danger into
// the relationships between rows, and those relationships have no compiler
// behind them unless one is written. This is that compiler, and it is the same
// shape as the spawn-temperature check at the bottom of reaction.h.
static_assert(Element{}.color == material_of(ElementType::Empty).color,
              "a default-constructed Element must carry Empty's colour from MATERIALS");

// What Element::ticks means, per material, as something a compiler can read.
//
// "No cell is ever both at once" is a rule about the table rather than a fact
// about one row, so it wants a lookup and an assertion over every row.
//
// Deliberately a function over ElementType rather than a column on MATERIALS: a
// column would be authored per row and could disagree with `structural` on the
// same line, where this cannot, because `structural` is where it reads the
// answer from. The cost is that a material with a lifetime is named here, which
// is why Lifetime is written as an exception list rather than as the default.
enum class TickRole : uint8_t {
    None,      // powders and fluids: no clock, and the byte is unused
    FallClock, // structural cells: steps of unbroken free fall
    Lifetime,  // gases that expire: steps left before they are gone
};

inline constexpr TickRole tick_role(ElementType type) {
    if (material_of(type).structural) return TickRole::FallClock;
    if (type == ElementType::Fire || type == ElementType::Steam) return TickRole::Lifetime;
    return TickRole::None;
}

// A cell with a lifetime may not be structural, because `ticks` means two
// things: fall time for a structural cell, a countdown for a gas. The whole
// argument that one byte serves both is that no cell is ever both at once, and
// that is a single word in a MATERIALS row away from stopping being true. If it
// did, a burning cell in free fall would spend its fuel on the fracture
// threshold and its fall time on burning out, and neither system would report
// anything wrong.
//
// Checked over every row rather than over one, so the next material given a
// lifetime is covered without anyone remembering to widen it.
namespace detail {
constexpr bool lifetimes_are_never_structural() {
    for (int i = 0; i < static_cast<int>(ElementType::Count); ++i) {
        const ElementType t = static_cast<ElementType>(i);
        if (tick_role(t) == TickRole::Lifetime && material_of(t).structural) return false;
    }
    return true;
}
} // namespace detail

static_assert(detail::lifetimes_are_never_structural(),
              "a material with a ticks lifetime is structural and would collide with the fall clock");
