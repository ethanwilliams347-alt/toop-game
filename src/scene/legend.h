#pragma once
#include "physics/material.h"
#include <cstdint>

// The colours a material map uses to name an ElementType, and nothing else.
//
// These are authoring metadata, not the palette anything is drawn in, and
// keeping the two apart is the whole reason this table exists. Resolving a
// material-map pixel against MATERIALS[i].color -- the colour the material is
// rendered in -- makes every scene file a hostage of the art direction: a
// palette retune stops every authored pixel matching anything, and since an
// unmatched pixel and a genuinely empty pixel are the same answer, the whole
// scene loads as Empty with nothing failing.
//
// So a legend colour is an arbitrary marker with no visual meaning, and the
// values below are frozen. Changing one invalidates every material map ever
// authored, the way changing a sim_random::Stream tag invalidates every world
// its seed ever produced. The palette in MATERIALS is then free to change as
// often as the art needs.
//
// The unused entries are picked to be obviously artificial -- nobody paints
// terrain in pure magenta by accident.
struct LegendEntry {
    uint32_t rgb;  // 24-bit, no alpha: BMPs are authored opaque and alpha is not a legend
    ElementType type;
};

inline constexpr LegendEntry SCENE_LEGEND[] = {
    { 0x000000, ElementType::Empty },  // background; deliberately the artist's "nothing here"
    { 0xEEDD82, ElementType::Sand  },
    { 0x4444FF, ElementType::Water },
    { 0x888888, ElementType::Wall  },
    { 0x6B4423, ElementType::Wood  },
    { 0xFF00FF, ElementType::Oil   },
    { 0x00FFFF, ElementType::Steam },
    { 0xFF0000, ElementType::Fire  },
    // Authorable even though nobody would sensibly paint a scene that starts
    // mid-burn: the assert below is about coverage rather than usefulness. A
    // material with no legend colour cannot be authored at all, and leaving a hole
    // here would mean the next genuinely unauthorable material has no failing check
    // to hit.
    { 0xFF8800, ElementType::Charred },
};

inline constexpr int LEGEND_SIZE = sizeof(SCENE_LEGEND) / sizeof(SCENE_LEGEND[0]);

static_assert(LEGEND_SIZE == static_cast<int>(ElementType::Count),
              "SCENE_LEGEND must have exactly one row per ElementType");

// Two legend colours sharing a value would make one of the two materials
// unauthorable, silently -- the scan below returns the first match, so the loser
// never appears in any scene anyone paints. Checked at compile time for the same
// reason sim_random::SIM_STREAMS is.
namespace detail {
constexpr bool legend_distinct() {
    for (int i = 0; i < LEGEND_SIZE; ++i) {
        if (SCENE_LEGEND[i].rgb > 0xFFFFFF) return false;  // 24-bit only
        for (int j = i + 1; j < LEGEND_SIZE; ++j)
            if (SCENE_LEGEND[i].rgb == SCENE_LEGEND[j].rgb) return false;
    }
    return true;
}
constexpr bool legend_covers_every_type() {
    for (int t = 0; t < static_cast<int>(ElementType::Count); ++t) {
        bool found = false;
        for (int i = 0; i < LEGEND_SIZE; ++i)
            if (static_cast<int>(SCENE_LEGEND[i].type) == t) found = true;
        if (!found) return false;
    }
    return true;
}
} // namespace detail

static_assert(detail::legend_distinct(), "two legend colours share a value: one material is unauthorable");
static_assert(detail::legend_covers_every_type(), "a material has no legend colour and cannot be authored");

// Resolves one material-map pixel. Returns false if `rgb` is not a legend colour
// at all, which the caller must not quietly treat as Empty -- that conflation is
// the bug this file exists to prevent. Empty has its own legend entry and
// reports true.
inline constexpr bool element_from_legend(uint32_t rgb, ElementType& out) {
    for (int i = 0; i < LEGEND_SIZE; ++i) {
        if (SCENE_LEGEND[i].rgb == (rgb & 0xFFFFFF)) {
            out = SCENE_LEGEND[i].type;
            return true;
        }
    }
    return false;
}
