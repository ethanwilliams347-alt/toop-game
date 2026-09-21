#pragma once
#include "material.h"
#include <cstdint>

// A + optional-neighbour -> result, rolled once per eligible cell per step.
// Rows are checked in order; the first row whose target and catalyst condition
// are both satisfied is the only one considered that frame, win or lose the
// roll, with no falling through to a later row. That is what gives
// water-dousing priority over Fire's natural burnout without special-casing.
struct Reaction {
    ElementType catalyst; // required 8-neighbor; ElementType::Count = spontaneous, no neighbor needed
    ElementType target;   // the cell type this row may transform

    // 0-10000, rolled once per eligible cell per step.
    //
    // Per myriad rather than per cent because a spontaneous decay row is a
    // lifetime: mean steps is 10000/chance. Burn durations live here rather than as
    // a column on MATERIALS, for the same reason the temperature window below does:
    // how long a transformation takes belongs to the transformation.
    //
    // The resolution is set by the smallest adjustment anyone wants to make, not by
    // the longest lifetime expressible. See chance_per_myriad in random.h.
    uint16_t chance_per_myriad;

    ElementType result;

    // Temperature window the target cell must be inside for this row to be eligible
    // at all, inclusive at both ends. 0-255 means any temperature, which is what
    // the rows genuinely about contact rather than heat use.
    //
    // The ignition point lives here rather than as a column on MATERIALS: a
    // threshold is a property of a transformation, not of a substance. The same
    // Water row that boils at 100 would need a second number the moment anything
    // else about water were temperature-gated, and Fire is already both a target
    // and a catalyst with no threshold of its own.
    uint8_t min_temp;
    uint8_t max_temp;

    // How far below min_temp this cell's threshold sits, drawn once per coordinate
    // and never redrawn. Zero for every row but Wood's, and one-sided downwards:
    // min_temp is the hardest any cell is, not the average.
    //
    // The one-sidedness is a safety property. A cell that drew a threshold well
    // above min_temp, in a beam only a few cells thick, is a plug the front cannot
    // pass before the Charred behind it burns out -- a fire that sometimes refuses
    // to cross a stick. The dice may therefore only ever make a cell easier to
    // light.
    //
    // Jittering the threshold rather than the ignition timing is deliberate. Making
    // ignition a roll instead of a certainty was measured and is worse on both
    // axes: the front got less ragged and several times slower, and on a thin plank
    // stopped propagating, because the Charred ahead burned out before the next
    // cell won its roll. Jittering the threshold works because the temperature
    // gradient ahead of a front is shallow, so a cell needing ten degrees more than
    // its neighbour crosses its line several cells later rather than a moment
    // later. It also reads physically: a soft or dry patch of wood has a lower
    // ignition point, not a hesitation.
    //
    // No storage. It is drawn from the cell's coordinates with authored_pick, the
    // step-0 draw colour jitter uses, so it is stable for the life of the world
    // without occupying a byte of Element.
    uint8_t temp_jitter;
};

// Wood's ignition point runs from its stated value down to this much below it,
// cell by cell. See temp_jitter above for why the variation is on the threshold
// rather than the timing, and why it only ever goes downwards.
//
// Bounded below rather than above, which is the opposite of what it looks like.
// Because the jitter is one-sided no cell is ever harder to light than the
// stated point, so nothing can become a permanent hole; the binding constraint
// is instead the lowest ignition point in the table, which is what
// lowest_ignition_point() promises nothing may spawn above. Widen this far
// enough and a puff of steam could light a wall.
inline constexpr uint8_t WOOD_IGNITION_JITTER = 40;

// Heat, not luck, spreads fire. Wood and Oil ignite when they get hot enough;
// conduction from a neighbouring flame is what gets them there, and how long
// that takes is set by their conductivity in MATERIALS. The dice are left on
// one row, Fire's own burnout, which is a lifetime rather than a threshold and
// has nothing to be gated on.
inline constexpr Reaction REACTIONS[] = {
    // Dousing keeps its place at the top and is deliberately not temperature-gated:
    // water hitting a flame puts it out because it is water, and gating it on heat
    // would mean a cold splash did nothing.
    { ElementType::Water, ElementType::Fire,   9000, ElementType::Steam,   0, 255, 0 },
    // Wood catches into Charred, not into Fire. The cell that was fuel stays where
    // it was, keeps holding up whatever it was holding up, and burns; the flame is
    // thrown off it by the `emits` column and is a separate, shorter-lived thing.
    //
    // The ignition point here, together with Wood's conductivity, sets how fast
    // fire travels. The hard bound is Charred's heat_source: a burning cell holds
    // itself at that temperature, so an ignition point at or above it means a fire
    // can never light its neighbour and will not propagate at all. Approaching it
    // makes propagation very slow before it stops.
    { ElementType::Count, ElementType::Wood, 10000, ElementType::Charred, 150, 255, WOOD_IGNITION_JITTER },
    // Oil has no smouldering state; it is a Liquid, so it flashes. See its row in
    // material.h for why that boundary is where it is.
    { ElementType::Count, ElementType::Oil,  10000, ElementType::Fire,     90, 255, 0 },
    { ElementType::Count, ElementType::Water,10000, ElementType::Steam,   100, 255, 0 },
    // Wood's burn duration, expressed as a lifetime. Untemperatured on purpose: a
    // cell that is already burning is not waiting on a threshold.
    //
    // The closed-form mean, 10000/chance steps, is a lower bound on what a body
    // actually burns for. Decay is only rolled for awake cells, and the interior of
    // a uniform body of Charred reaches thermal equilibrium with itself, stops
    // marking itself dirty, and sleeps through rolls it would otherwise have lost.
    // The gap widens the longer the lifetime is; a thin burn front sleeps less and
    // sits nearer the closed-form figure.
    //
    // This knob has no cliff near it and does not affect how fast fire travels --
    // the spread probe reads the same to within noise across the range tried. How
    // long wood resists and how fast fire crosses it are genuinely two dials.
    { ElementType::Count, ElementType::Charred, 34, ElementType::Empty,     0, 255, 0 },
    // Flame's own burnout is a countdown on Element::ticks, in step_fire, rather
    // than a row here, because the colour ramp has to read the flame's age and a
    // dice roll has no age to read.
};

// The coldest temperature at which anything in the table catches fire.
//
// Exists to be asserted against rather than read: a material that spawns hotter
// than this is an ignition source whether or not anything calls it one, because
// heat_flow has a floor of one unit per step, so any difference of two or more
// eventually transfers in full. Conductivity only changes how long that takes.
inline constexpr uint8_t lowest_ignition_point() {
    uint8_t lowest = 255;
    for (const Reaction& r : REACTIONS)
        if (r.result == ElementType::Fire && r.min_temp < lowest) lowest = r.min_temp;
    return lowest;
}

// Nothing may spawn hot enough to light something unless it is declared a heat
// source. Fire is the one row allowed to, and says so with a non-zero
// heat_source; everything else has to arrive cool enough that it can only warm
// its surroundings.
//
// Checked here rather than trusted because the bug is invisible at the call
// site: spawn_temperature is a column about how a material is created, and
// ignition thresholds are rows in a different table about how one is destroyed.
// Nothing about editing either suggests reading the other.
//
// Conduction cannot overshoot -- each exchange is capped at half the difference
// and stops inside the dead band -- so a neighbour converges towards a spawn
// temperature and never past it. Strictly below the threshold is therefore
// genuinely safe.
namespace detail {
constexpr bool spawn_temperatures_cannot_ignite() {
    for (int i = 0; i < static_cast<int>(ElementType::Count); ++i) {
        const Material& m = material_of(static_cast<ElementType>(i));
        if (m.heat_source != 0) continue;  // a declared heat source is meant to
        if (m.spawn_temperature >= lowest_ignition_point()) return false;
    }
    return true;
}
} // namespace detail

static_assert(detail::spawn_temperatures_cannot_ignite(),
              "a material that is not a declared heat source spawns hot enough to ignite something");

// The same rule applied to the bottom of the jitter's range, which is where the
// softest cell in the world sits. Checked separately because the function above
// reads min_temp and knows nothing about temp_jitter, so a jitter widened past
// the margin would slip under the ignition floor without disturbing a single
// assertion.
namespace detail {
constexpr bool jittered_thresholds_stay_above_spawn_temperatures() {
    for (const Reaction& r : REACTIONS) {
        if (r.temp_jitter == 0) continue;
        if (static_cast<int>(r.min_temp) - static_cast<int>(r.temp_jitter) <
            static_cast<int>(lowest_ignition_point()))
            return false;
    }
    return true;
}
} // namespace detail

static_assert(detail::jittered_thresholds_stay_above_spawn_temperatures(),
              "a jittered ignition threshold reaches below the coldest ignition point in the table");
