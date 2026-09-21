#pragma once
#include <cstdint>
#include <cstddef>

enum class ElementType : uint8_t {
    Empty = 0,
    Sand,
    Water,
    Wall,
    Wood,
    Oil,
    Steam,
    Fire,
    // Wood that has caught: a state of the fuel, not a kind of fire. This cell
    // holds the burn duration, stays structural, keeps heating its neighbours and
    // throws short-lived Fire into the air around it.
    Charred,
    Count
};

// How a material moves. The physics step is written against these four
// behaviours rather than against individual materials, so adding a material is
// a new row in MATERIALS below, not a new branch in the update loop.
enum class MoveKind : uint8_t {
    Static, // never moves (wall, wood)
    Powder, // falls straight down, piles into a slope (sand)
    Liquid, // falls, then spreads out to find its level (water, oil)
    Gas     // rises, then spreads (steam)
};

struct Material {
    const char* name;
    uint32_t color;       // ARGB8888 base colour
    uint8_t color_jitter; // per-cell brightness variation, 0 = flat
    MoveKind move;
    int16_t density;      // denser sinks through lighter; Empty is 0, gases negative
    uint8_t spread;       // cells a liquid/gas may travel sideways per step

    // Whether this material holds itself and its neighbours up, and therefore has
    // to fall as one piece when nothing holds it up.
    bool structural;

    // --- thermal ---

    // How readily heat moves through this material, 0-255. Used for conduction
    // between neighbours (a pair moves heat at the lower of the two, so an
    // insulator stops a conductor) and for the bleed back to ambient. Zero means
    // the material takes no part in heat at all, which is what Empty is: air is not
    // simulated, so heat travels through matter in contact and nowhere else.
    uint8_t conductivity;

    // Temperature a freshly placed cell of this material gets. Zero means "keep
    // whatever the spot was already at", so material dug out of a hot wall arrives
    // hot. Non-zero is for materials that are hot by definition.
    uint8_t spawn_temperature;

    // Temperature this material holds itself at every step, regardless of what it
    // is losing to its surroundings. Zero for everything merely warm. This is what
    // stops a flame being quenched by the cold wall it sits against.
    uint8_t heat_source;

    // How many steps of Fire one cell of this material is worth when it burns. Read
    // at ignition and stored on the resulting flame, so it is a property of the
    // fuel, not of the fire. Zero for anything not combustible.
    //
    // What a burning cell of this material throws into the empty space beside it,
    // and how often. `Count` means nothing, which is every row but one. Emission
    // targets Empty neighbours, so a buried cell has nowhere to emit and does not
    // visibly burn.
    ElementType emits;
    uint8_t emit_chance; // percent, per step, per burning cell
};

// Indexed by ElementType. Keep rows in the same order as the enum.
//
// The colours are chosen as a set, not row by row. Reaction products take their
// colour from this table, so rows picked independently show a seam where one
// material becomes another. Every material is composited over the backdrop
// rather than over black, and the backdrop is a cool dark blue, so the world
// sits warm and desaturated against it; Fire and Water are the only rows that
// keep real saturation, because they are the two that must never be missed.
//
// Jitter is small wherever it is not zero: per-cell random noise fights the
// ordered dithering in the art rather than combining with it. What is left is
// enough to stop a flat fill looking flat. Fire keeps the widest range, because
// there the variation is the thing being drawn.
inline constexpr Material MATERIALS[] = {
    // name      colour       jitter  move              density  spread  structural  cond  spawn  source  emits                 emit%
    // Empty is the one row whose colour is fully transparent rather than a colour:
    // nothing is there, so whatever is drawn behind the cell shows through. Every
    // other row is opaque -- alpha marks absence here, not a per-material effect.
    {  "Empty",  0x00000000,      0,  MoveKind::Static,       0,      0,  false,        0,    20,      0,  ElementType::Count,      0 },
    {  "Sand",   0xFFC6A970,      6,  MoveKind::Powder,     150,      0,  false,       30,     0,      0,  ElementType::Count,      0 },
    // Water is teal rather than blue for compositing reasons: the backdrop behind
    // it is a cool blue, and a blue liquid seen through a gap read as a hole in the
    // world.
    {  "Water",  0xFF2E7F96,      5,  MoveKind::Liquid,     100,      5,  false,      120,     0,      0,  ElementType::Count,      0 },
    // The two structural materials. Density is what lets an unsupported slab sink
    // through a fluid it lands in rather than perch on top of it. Wall conducts
    // poorly on purpose: a wall is usually the biggest connected body in a scene,
    // and a good conductor there would quench any fire touching it.
    {  "Wall",   0xFF6F6A63,      5,  MoveKind::Static,   32000,      0,  true,        30,     0,      0,  ElementType::Count,      0 },
    // Wood is fuel. What it turns into when lit is Charred rather than Fire -- the
    // burning is a state of this cell, and the flame is thrown off it. How long it
    // burns is the decay chance on Charred's row in REACTIONS.
    //
    // Conductivity sets how fast fire spreads: it governs how quickly an unlit cell
    // climbs to the ignition point on Wood's row in REACTIONS. It has a floor, but
    // a cell also loses heat to ambient every step, so below a certain conductivity
    // the front sheds heat faster than it is fed and stops rather than slows. That
    // cliff is close below 40. The ignition point cannot be raised to compensate:
    // Charred holds itself at 200, so anything above 150 eats a thin margin.
    {  "Wood",   0xFF6B4E33,      5,  MoveKind::Static,   32000,      0,  true,        40,     0,      0,  ElementType::Count,      0 },
    // Oil flashes straight to Fire and does not smoulder. A burning cell holds its
    // state in the cell, and Oil is a Liquid, so a smouldering oil cell would carry
    // that state through swap_elements and every fluid rule. Only Static materials
    // get a burning state; movable fuels ignite and are gone.
    {  "Oil",    0xFF2C2620,      3,  MoveKind::Liquid,      60,      3,  false,       70,     0,      0,  ElementType::Count,      0 },
    // Spawns below the coldest ignition point in REACTIONS, and that bound is
    // enforced at the bottom of reaction.h rather than remembered. A steam spawn
    // above an ignition point makes steam a fire-starter -- both ways of making it,
    // boiling and dousing a flame, would then start fires. Conductivity cannot fix
    // that, only temperature can: heat_flow has a floor of one unit per step, so
    // any gap of two or more transfers in full eventually.
    //
    // The cost is a shorter puff: steam's life is the span between this number and
    // its condensing point. The gain beyond the bug is that water boiling into
    // cooler steam absorbs heat rather than creating it out of nothing.
    {  "Steam",  0xFFC4D2D8,      4,  MoveKind::Gas,        -20,      3,  false,       40,    88,      0,  ElementType::Count,      0 },
    // Denser (less negative) than Steam so flame stays under a steam layer instead
    // of punching through it. The only heat source in the table.
    {  "Fire",   0xFFF07A22,     16,  MoveKind::Gas,        -10,      4,  false,      200,   250,    250,  ElementType::Count,      0 },
    // The cell that is actually on fire. Structural, because a burning ceiling that
    // drops the instant it catches is worse than one that burns through first.
    // That `true` is what forces its lifetime to be a decay chance rather than a
    // countdown: a structural cell already spends Element::ticks on the free-fall
    // clock, and the static_assert at the bottom of element.h turns that collision
    // into a compile error. The variance a chance produces is a gain -- a plank's
    // cells stop winking out in lockstep.
    //
    // Heat source at 200 rather than a conductor, which is what spreads fire: it
    // sits above Wood's ignition point, so a charred cell brings its neighbours to
    // catching regardless of what is cooling them. Flame is thrown off and dies
    // within a dozen steps, far too briefly to heat anything through.
    //
    // Colour is charcoal -- a warm dark grey with visible mottling -- not soot. A
    // near-black cell against the dark blue backdrop reads as a hole rather than a
    // material, and mottling is most of what separates charcoal from a flat fill.
    // It stays far enough under the flame it emits to keep the two shapes distinct.
    {  "Charred",0xFF3A3431,     10,  MoveKind::Static,   32000,      0,  true,        90,     0,    200,  ElementType::Fire,      25 },
};

static_assert(sizeof(MATERIALS) / sizeof(MATERIALS[0]) == static_cast<size_t>(ElementType::Count),
              "MATERIALS must have exactly one row per ElementType");

inline constexpr const Material& material_of(ElementType type) {
    return MATERIALS[static_cast<size_t>(type)];
}

// True if this material is part of a structure: it holds its neighbours up, and
// when nothing holds it up in turn, the whole connected piece falls together
// rather than each cell falling on its own.
inline constexpr bool is_structural(ElementType type) {
    return material_of(type).structural;
}

// What the player character collides with. Derived from the movement
// behaviour rather than listed per material, so a new MATERIALS row gets
// correct collision without a second table to keep in sync. Anything that
// holds its shape is solid: static terrain, and powders, which pile up and
// can be stood on. Empty is Static in the table only because it never moves,
// so it is excluded explicitly.
inline constexpr bool is_solid(ElementType type) {
    if (type == ElementType::Empty) return false;
    const MoveKind move = material_of(type).move;
    return move == MoveKind::Static || move == MoveKind::Powder;
}
