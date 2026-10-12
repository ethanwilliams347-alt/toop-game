#pragma once
#include "physics/grid.h"

// Whether two grids hold the same world, for the suites that check determinism.
//
// One definition, shared, because the copies drifted: one compared every field
// of Element and another skipped temperature and piece_tag, so a replay that
// diverged in heat or in where a slab had cracked passed until the divergence
// happened to change a cell's type. A field added to Element belongs here, and
// in input_log::fingerprint(Grid), which answers the same question for a
// recorded session.
//
// Compared field by field rather than with memcmp: Element carries padding, and
// a difference in padding bytes is not a difference in the world.
//
// A header of its own rather than more of test_util.h, because that one is
// included by suites that link no engine and have no business seeing grid.h.
inline bool worlds_match(const Grid& a, const Grid& b) {
    if (a.get_width() != b.get_width() || a.get_height() != b.get_height()) return false;
    if (a.get_pixels() != b.get_pixels()) return false;
    for (int y = 0; y < a.get_height(); ++y) {
        for (int x = 0; x < a.get_width(); ++x) {
            const Element ea = a.get_element(x, y);
            const Element eb = b.get_element(x, y);
            if (ea.type != eb.type || ea.color != eb.color || ea.updated_tag != eb.updated_tag ||
                ea.ticks != eb.ticks || ea.temperature != eb.temperature ||
                ea.piece_tag != eb.piece_tag || ea.mass != eb.mass || ea.flow != eb.flow)
                return false;
        }
    }
    return true;
}
