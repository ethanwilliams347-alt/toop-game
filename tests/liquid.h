#pragma once
#include <cstdint>
#include "physics/grid.h"

// How much liquid a world holds, and where it sits -- for the suites that check
// conservation and level.
//
// In units rather than cells, and that is the whole reason this header exists.
// The suites state "water is conserved" and "the pool is level" as properties of
// the liquid, not of how a cell happens to store it, so the same assertions hold
// whether a cell is one unit of water or holds an amount of it. UNITS_PER_CELL is
// how many units a full cell holds; units_of is how many a given cell holds.
// Every count in the suites goes through these two, and nothing else in a test
// may assume one cell is one unit.
//
// Steam counts too: it is water in another state, and condensing and boiling are
// conserving only when the two are added up.
namespace liquid {

inline constexpr int64_t UNITS_PER_CELL = 1;

// The units of liquid (or steam) cell `e` holds, 0 for anything else. The type is
// checked by the caller; this is only "how much".
inline int64_t units_of(const Element& e) {
    (void)e;
    return 1;
}

inline int64_t total(const Grid& g, ElementType t) {
    int64_t n = 0;
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < g.get_width(); ++x) {
            const Element e = g.get_element(x, y);
            if (e.type == t) n += units_of(e);
        }
    return n;
}

// How deep the liquid in column x is, in full cells.
inline double depth(const Grid& g, int x, ElementType t) {
    int64_t n = 0;
    for (int y = 0; y < g.get_height(); ++y) {
        const Element e = g.get_element(x, y);
        if (e.type == t) n += units_of(e);
    }
    return static_cast<double>(n) / static_cast<double>(UNITS_PER_CELL);
}

// Mean row of the liquid, weighted by how much each cell holds; lower is higher
// on screen. -1 when there is none.
inline double mean_row(const Grid& g, ElementType t) {
    double sum = 0.0;
    int64_t n = 0;
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < g.get_width(); ++x) {
            const Element e = g.get_element(x, y);
            if (e.type != t) continue;
            const int64_t u = units_of(e);
            sum += static_cast<double>(y) * static_cast<double>(u);
            n += u;
        }
    return n ? sum / static_cast<double>(n) : -1.0;
}

}  // namespace liquid
