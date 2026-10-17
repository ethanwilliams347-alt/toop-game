// What the liquids do, as numbers, for judging a fluid model against the last
// one.
//
// Not a registered test: these are qualities -- how fast a U-tube levels, how
// long a released wall of water sloshes, how flat a pool ends -- and the right
// answers are a matter of feel. The properties that must always hold (water is
// conserved, a level pool sleeps, sand does not carry water up its own column)
// are asserted in test_grid.cpp; this reports how well they are met. It replaces
// water_probe, which measured only the third scene below.
//
// Every amount goes through tests/liquid.h, so the probe reads the same whether
// a cell is one unit of water or holds an amount of it.

#include <cmath>
#include <cstdio>
#include "game/run.h"
#include "liquid.h"

namespace {

// Steps until the world has been asleep for 30 in a row, or -1 within `limit`.
int settle(Grid& g, int limit) {
    int quiet = 0;
    for (int s = 0; s < limit; ++s) {
        g.update();
        quiet = g.active_chunk_count() == 0 && !g.has_pending_support_checks() ? quiet + 1 : 0;
        if (quiet >= 30) return s + 1 - 30;
    }
    return -1;
}

// The spread of liquid depth across columns [x0, x1), in cells: 0 is dead level.
double unevenness(const Grid& g, int x0, int x1, ElementType t) {
    double lo = 1e9, hi = -1e9;
    for (int x = x0; x < x1; ++x) {
        const double d = liquid::depth(g, x, t);
        lo = d < lo ? d : lo;
        hi = d > hi ? d : hi;
    }
    return hi - lo;
}

void u_tube() {
    Grid g(40, 40);
    for (int x = 9; x <= 21; ++x) {
        g.set_element(x, 18, ElementType::Wall);
        g.set_element(x, 39, ElementType::Wall);
    }
    for (int y = 18; y <= 39; ++y) {
        g.set_element(9, y, ElementType::Wall);
        g.set_element(21, y, ElementType::Wall);
    }
    for (int y = 19; y <= 37; ++y)
        for (int x = 11; x <= 19; ++x) g.set_element(x, y, ElementType::Wall);
    for (int x = 10; x <= 20; ++x) g.set_element(x, 38, ElementType::Water);
    for (int y = 24; y <= 37; ++y) g.set_element(10, y, ElementType::Water);
    const int64_t placed = liquid::total(g, ElementType::Water);
    // How far apart the arms still are a few seconds in: the pace of the levelling,
    // where the settle time is mostly its last fraction of a cell.
    for (int s = 0; s < 200; ++s) g.update();
    const double apart =
        liquid::depth(g, 10, ElementType::Water) - liquid::depth(g, 20, ElementType::Water);
    const int steps = settle(g, 4000);
    std::printf("u-tube:      %.2f cells apart at step 200; settled in %5d steps; arms %.2f and "
                "%.2f cells deep; water %lld -> %lld\n",
                apart, steps < 0 ? steps : steps + 200, liquid::depth(g, 10, ElementType::Water),
                liquid::depth(g, 20, ElementType::Water), static_cast<long long>(placed),
                static_cast<long long>(liquid::total(g, ElementType::Water)));
}

// A wall of water held at one end of a tank, released all at once. The swing of
// the far end's depth over time is the slosh; the settle time is how long it
// takes to stop.
void dam_break() {
    constexpr int W = 160, H = 60, FLOOR = 55;
    Grid g(W, H, 7);
    for (int x = 0; x < W; ++x) g.set_element(x, FLOOR, ElementType::Wall);
    for (int y = 20; y < FLOOR; ++y) {
        g.set_element(0, y, ElementType::Wall);
        g.set_element(W - 1, y, ElementType::Wall);
    }
    for (int y = FLOOR - 30; y < FLOOR; ++y)
        for (int x = 1; x < 30; ++x) g.set_element(x, y, ElementType::Water);
    const int64_t placed = liquid::total(g, ElementType::Water);

    double far_peak = 0.0;
    int far_rises = 0;
    double prev_far = 0.0;
    bool rising = false;
    int quiet = 0, settled = -1;
    // The step the front first reaches the far wall: how fast released water runs.
    // And how many cell-steps ran at the fastest speed seen, which says whether the
    // speed ceiling or the spreading is what holds it back.
    int arrived = -1;
    int64_t at_speed[128] = {};
    for (int s = 0; s < 6000 && settled < 0; ++s) {
        g.update();
        for (int y = 0; y < FLOOR; ++y)
            for (int x = 1; x < W - 1; ++x) {
                const int f = g.get_element(x, y).flow;
                ++at_speed[f < 0 ? (f < -127 ? 127 : -f) : f];
            }
        const double far = liquid::depth(g, W - 3, ElementType::Water);
        if (arrived < 0 && far > 0.0) arrived = s + 1;
        if (far > far_peak) far_peak = far;
        // Each time the far end turns from falling to rising is one wave arriving.
        if (far > prev_far + 0.25 && !rising) {
            rising = true;
            ++far_rises;
        } else if (far < prev_far - 0.25) {
            rising = false;
        }
        prev_far = far;
        quiet = g.active_chunk_count() == 0 ? quiet + 1 : 0;
        if (quiet >= 30) settled = s + 1 - 30;
    }
    int top = 127;
    while (top > 0 && at_speed[top] == 0) --top;
    std::printf("dam break:   front at far wall in %4d steps; top speed %.3f cells/step for "
                "%lld cell-steps; settled in %5d steps; far wall peak %.2f cells over %d waves; "
                "unevenness %.2f; water %lld -> %lld\n",
                arrived, top * Grid::LIQUID_FLOW_SCALE / static_cast<double>(Grid::LIQUID_FULL),
                static_cast<long long>(at_speed[top]), settled, far_peak, far_rises,
                unevenness(g, 1, W - 1, ElementType::Water), static_cast<long long>(placed),
                static_cast<long long>(liquid::total(g, ElementType::Water)));
}

// Sand poured into a pool from a brush held in open air above it. Water above
// the brush can only have got there by riding up the sand.
void sand_into_pool() {
    constexpr int W = 120, H = 90, FLOOR = 80, SURFACE = 60, CX = 60, CY = 35;
    Run run(W, H, 4242);
    for (int x = 0; x < W; ++x) run.grid.set_element(x, FLOOR, ElementType::Wall);
    for (int y = SURFACE; y < FLOOR; ++y)
        for (int x = 10; x < W - 10; ++x) run.grid.set_element(x, y, ElementType::Water);
    const int64_t placed = liquid::total(run.grid, ElementType::Water);

    Input paint;
    paint.brush_active = true;
    paint.brush_type = ElementType::Sand;
    paint.brush_size = 3;
    paint.cursor_x = CX;
    paint.cursor_y = CY;
    int64_t worst_above = 0;
    for (int s = 0; s < 500; ++s) {
        run.step(paint);
        int64_t above = 0;
        for (int y = 0; y < CY; ++y)
            for (int x = 0; x < W; ++x) {
                const Element e = run.grid.get_element(x, y);
                if (e.type == ElementType::Water) above += liquid::units_of(e);
            }
        worst_above = above > worst_above ? above : worst_above;
    }
    const int steps = settle(run.grid, 6000);
    std::printf("sand->pool:  worst water above the brush %lld units; settled in %5d steps after; "
                "water %lld -> %lld\n",
                static_cast<long long>(worst_above), steps, static_cast<long long>(placed),
                static_cast<long long>(liquid::total(run.grid, ElementType::Water)));
}

// A pool whose top row is partial: how level it ends, and whether it sleeps.
void partial_pool() {
    Grid g(40, 40);
    for (int y = 30; y < 40; ++y)
        for (int x = 0; x < 40; ++x) g.set_element(x, y, ElementType::Water);
    for (int x = 0; x < 17; ++x) g.set_element(x, 29, ElementType::Water);
    const int steps = settle(g, 4000);
    std::printf("partial row: settled in %5d steps; unevenness %.2f cells\n", steps,
                unevenness(g, 0, 40, ElementType::Water));
}

}  // namespace

int main() {
    u_tube();
    dam_break();
    sand_into_pool();
    partial_pool();
    return 0;
}
