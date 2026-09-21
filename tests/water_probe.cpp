// The instrument for the water/sand brush defect.
//
// Two claims to measure:
//
//  1. Water spawns on top of the sand column the moment the sand touches the
//     water -- so: how far above the original waterline does water get, and does
//     that height depend on how long the brush is held?
//  2. It flows in a staggered, tree-like pattern -- so: how wide is the water
//     that is up there, and is it connected or scattered?
//
// A pool, a Sand brush held stationary in open air well above the surface, and
// nothing else. The brush never touches water at any point, which is the whole
// reason this is a probe and not a test. If water still ends up at the cursor,
// it climbed.

#include "game/run.h"
#include <cstdio>
#include <string>

namespace {

constexpr int W = 120;
constexpr int H = 90;
constexpr int FLOOR_Y = 80;
constexpr int SURFACE_Y = 60;  // top row of water
constexpr int CURSOR_Y = 35;  // open air above the surface
constexpr int CURSOR_X = 60;

void build(Run& run) {
    for (int x = 0; x < W; ++x) run.grid.set_element(x, FLOOR_Y, ElementType::Wall);
    for (int y = SURFACE_Y; y < FLOOR_Y; ++y)
        for (int x = 10; x < W - 10; ++x) run.grid.set_element(x, y, ElementType::Water);
}

int count_of(const Grid& g, ElementType t) {
    int n = 0;
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < g.get_width(); ++x)
            if (g.get_element(x, y).type == t) n++;
    return n;
}

// Measured against the cursor and not against the old waterline. Pouring sand
// into a pool genuinely raises its level -- the sand takes up room -- so "water
// above where the surface used to be" counts the fix working and the defect
// happening as the same number, and a long enough run reports a large one either
// way.
//
// Water above the cursor has no such excuse. The brush is the highest sand in
// the world and it is well above the water; nothing can put water up there
// except a lift.
int water_above_cursor(const Grid& g) {
    int n = 0;
    for (int y = 0; y < CURSOR_Y; ++y)
        for (int x = 0; x < W; ++x)
            if (g.get_element(x, y).type == ElementType::Water) n++;
    return n;
}

// Topmost water in the world, as a row. Reported alongside the count because a
// single cell riding to the ceiling and a broad sheet of it look identical in a
// count and completely different on screen.
int highest_water_row(const Grid& g) {
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < W; ++x)
            if (g.get_element(x, y).type == ElementType::Water) return y;
    return -1;
}

// An ASCII slice around the column, which is the only way to see the shape the
// report is about -- a height and a count cannot tell "staggered tree" from "a
// neat sheath".
void dump(const Grid& g, int x0, int x1, int y0, int y1) {
    for (int y = y0; y <= y1; ++y) {
        std::printf("%3d |", y);
        for (int x = x0; x <= x1; ++x) {
            switch (g.get_element(x, y).type) {
                case ElementType::Empty: std::putchar('.'); break;
                case ElementType::Sand:  std::putchar('s'); break;
                case ElementType::Water: std::putchar('~'); break;
                case ElementType::Wall:  std::putchar('#'); break;
                default:                 std::putchar('?'); break;
            }
        }
        std::putchar('\n');
    }
}

} // namespace

int main() {
    Input paint;
    paint.brush_active = true;
    paint.brush_type = ElementType::Sand;
    paint.brush_size = 3;
    paint.cursor_x = CURSOR_X;
    paint.cursor_y = CURSOR_Y;

    std::printf("Sand brush held at (%d,%d); water surface at y=%d; floor at y=%d\n",
                CURSOR_X, CURSOR_Y, SURFACE_Y, FLOOR_Y);
    std::printf("The brush is %d cells of open air above the water and never touches it.\n\n",
                SURFACE_Y - CURSOR_Y);

    std::printf("%8s %16s %14s %10s\n", "steps", "water>cursor", "highest row", "water");
    Run run(W, H, 4242);
    build(run);
    const int start_water = count_of(run.grid, ElementType::Water);

    // Reported densely and early. The report is about the moment the sand touches
    // the water, so the interesting window is the first few dozen steps, not the
    // steady state of a pile big enough to fill the pool.
    for (int step = 1; step <= 500; ++step) {
        run.step(paint);
        if (step <= 100 ? step % 10 == 0 : step % 50 == 0) {
            std::printf("%8d %16d %14d %10d\n", step, water_above_cursor(run.grid),
                        highest_water_row(run.grid), count_of(run.grid, ElementType::Water));
        }
    }

    std::printf("\nwater at start %d, at end %d\n", start_water,
                count_of(run.grid, ElementType::Water));
    std::printf("\nColumn at the cursor (row %d), rows %d-%d:\n", CURSOR_Y, CURSOR_Y - 6, SURFACE_Y + 2);
    dump(run.grid, CURSOR_X - 34, CURSOR_X + 34, CURSOR_Y - 6, SURFACE_Y + 2);

    // Released, then left alone. If the water up there is only held by the stroke,
    // it comes down; if it is stuck inside the column, it stays.
    for (int i = 0; i < 400; ++i) run.step(Input{});
    std::printf("\n400 steps after release: water above cursor %d, highest row %d\n",
                water_above_cursor(run.grid), highest_water_row(run.grid));
    dump(run.grid, CURSOR_X - 34, CURSOR_X + 34, CURSOR_Y - 6, SURFACE_Y + 2);
    return 0;
}
