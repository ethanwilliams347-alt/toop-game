// How loose bodies actually move, as numbers and pictures.
//
// Not a registered test, for the reason burn_probe and fluid_probe are not: how
// far a post skids or how high a block bounces is a matter of feel, and a test
// that pinned it would be a transcription of the constants. What the suite
// asserts about bodies -- that they conserve every cell, come to rest, replay
// exactly, and that undisturbed structure never moves -- is in test_collapse.cpp.
//
// Each scene runs until everything is at rest (or a step budget runs out),
// printing every moving body every `every` steps and a picture of the world at
// the start and the end:
//
//   disc      - an asymmetric wooden disc dropped on a floor: does it roll, and stop?
//   lip       - a post landing on the lip of a ledge: it should kick over the edge.
//   plank     - a plank with only its end on a post: it swings down and leans.
//   dominoes  - a row of posts, the first one pushed: do they all go?
//   seesaw    - a plank balanced on a stone fulcrum, sand poured on one end.
//   float     - a wooden block and a stone block dropped into a pool.
//   slope     - a block set on a 45-degree stone slope: does it slide?
//   stack     - a leaning stack of blocks, built off-centre, one block at a time.
//   raft      - a wooden slab dropped into a pond from several heights and offsets.
//   pile      - pieces dropped onto each other, and a slab in a narrow pond: how
//               many steps each takes to sleep, how often they wake, and whether
//               any is only frozen at BODY_MAX_STEPS.
//
// Usage: body_probe [scene] [every]. With no scene, runs them all.
#include "physics/grid.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <string>

namespace {

void fill(Grid& g, int x0, int y0, int x1, int y1, ElementType t) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) g.set_element(x, y, t);
}

void nudge(Grid& g, int x, int y) {
    g.set_element(x, y, ElementType::Wall);
    g.set_element(x, y, ElementType::Empty);
}

char glyph(ElementType t) {
    switch (t) {
        case ElementType::Empty:   return '.';
        case ElementType::Sand:    return ':';
        case ElementType::Water:   return '~';
        case ElementType::Wall:    return '#';
        case ElementType::Wood:    return 'W';
        case ElementType::Oil:     return 'o';
        case ElementType::Steam:   return '^';
        case ElementType::Fire:    return '*';
        case ElementType::Charred: return 'C';
        default:                   return '?';
    }
}

void picture(const Grid& g, const char* label) {
    std::printf("  %s\n", label);
    for (int y = 0; y < g.get_height(); ++y) {
        std::string row = "    ";
        for (int x = 0; x < g.get_width(); ++x) row += glyph(g.get_element(x, y).type);
        std::printf("%s\n", row.c_str());
    }
}

double f(fx::v v) { return static_cast<double>(v) / fx::ONE; }

// Runs to rest or `budget` steps; returns the step it came to rest on, or -1.
int run(Grid& g, int budget, int every) {
    int most = 0;
    for (int s = 1; s <= budget; ++s) {
        g.update();
        const int n = g.tipping_count();
        if (n > most) most = n;
        if (every > 0 && s % every == 0) {
            for (int i = 0; i < n; ++i) {
                const Grid::BodyState b = g.body_state(i);
                std::printf("    step %4d  body %d  at (%6.2f, %6.2f)  v (%5.2f, %5.2f)  "
                            "angle %7.1f deg  spin %6.2f deg/step  %d cells\n",
                            s, i, f(b.x), f(b.y), f(b.vx), f(b.vy), f(b.theta) * 57.2958,
                            f(b.omega) * 57.2958, b.cells);
            }
        }
        if (!g.has_pending_support_checks() && g.active_chunk_count() == 0) {
            std::printf("  at rest after %d steps (%.2f s); at most %d bodies at once\n", s,
                        s / 60.0, most);
            return s;
        }
    }
    std::printf("  NOT at rest after %d steps; %d bodies still moving\n", budget,
                g.tipping_count());
    return -1;
}

void disc(int every) {
    std::printf("\n== disc ==\n");
    Grid g(34, 26);
    const int cx = 17, cy = 8, r = 6;
    for (int oy = -r; oy <= r; ++oy)
        for (int ox = -r; ox <= r; ++ox)
            if (ox * ox + oy * oy <= r * r) g.set_element(cx + ox, cy + oy, ElementType::Wood);
    for (int oy = -3; oy <= 3; ++oy)
        for (int ox = -3; ox <= 3; ++ox)
            if (ox * ox + oy * oy <= 9) g.set_element(12 + ox, 8 + oy, ElementType::Empty);
    picture(g, "start");
    run(g, 900, every);
    picture(g, "end");
}

void lip(int every) {
    std::printf("\n== lip ==\n");
    Grid g(80, 80);
    fill(g, 0, 60, 30, 79, ElementType::Wall);
    fill(g, 31, 75, 79, 79, ElementType::Wall);
    fill(g, 30, 30, 32, 44, ElementType::Wood);
    nudge(g, 31, 29);
    run(g, 900, every);
    picture(g, "end");
}

void plank(int every) {
    std::printf("\n== plank ==\n");
    Grid g(80, 40);
    fill(g, 60, 25, 62, 39, ElementType::Wall);
    fill(g, 10, 22, 62, 24, ElementType::Wood);
    nudge(g, 30, 21);
    run(g, 900, every);
    picture(g, "end");
}

void dominoes(int every) {
    std::printf("\n== dominoes ==\n");
    Grid g(120, 40);
    fill(g, 0, 34, 119, 39, ElementType::Wall);
    for (int i = 0; i < 8; ++i) fill(g, 10 + i * 10, 18, 11 + i * 10, 33, ElementType::Wood);
    // Knock the first one: a head on its right-hand side puts its centre past its
    // foot, and the nudge has it judged.
    fill(g, 12, 18, 17, 20, ElementType::Wood);
    nudge(g, 11, 17);
    picture(g, "start");
    run(g, 1500, every);
    picture(g, "end");
}

void seesaw(int every) {
    std::printf("\n== seesaw ==\n");
    Grid g(80, 40);
    fill(g, 0, 36, 79, 39, ElementType::Wall);
    fill(g, 38, 30, 41, 35, ElementType::Wall);  // fulcrum
    fill(g, 15, 28, 64, 29, ElementType::Wood);  // plank across it, balanced
    nudge(g, 40, 27);
    for (int s = 0; s < 60; ++s) g.update();
    std::printf("  balanced before the sand: %s\n", g.tipping_count() == 0 ? "yes" : "NO");
    // Pour sand onto the right end for a while.
    for (int s = 0; s < 240; ++s) {
        if (s % 2 == 0)
            for (int x = 56; x <= 62; ++x) g.set_element(x, 2, ElementType::Sand);
        g.update();
        if (s % every == 0 && g.tipping_count() > 0) {
            const Grid::BodyState b = g.body_state(0);
            std::printf("    pour %3d  angle %6.1f deg  spin %6.2f deg/step\n", s,
                        f(b.theta) * 57.2958, f(b.omega) * 57.2958);
        }
    }
    run(g, 900, every);
    picture(g, "end");
}

void floating(int every) {
    std::printf("\n== float ==\n");
    Grid g(80, 50);
    fill(g, 0, 45, 79, 49, ElementType::Wall);
    fill(g, 0, 20, 0, 44, ElementType::Wall);
    fill(g, 79, 20, 79, 44, ElementType::Wall);
    fill(g, 1, 25, 78, 44, ElementType::Water);
    fill(g, 15, 5, 26, 10, ElementType::Wood);  // 12x6 wooden block
    fill(g, 50, 5, 57, 10, ElementType::Wall);  // 8x6 stone block
    nudge(g, 20, 4);
    nudge(g, 53, 4);
    run(g, 1500, every);
    picture(g, "end");
    int top = -1;
    for (int y = 0; y < 50 && top < 0; ++y)
        for (int x = 1; x < 79; ++x)
            if (g.get_element(x, y).type == ElementType::Water) {
                top = y;
                break;
            }
    std::printf("  water surface row %d\n", top);
}

void slope(int every) {
    std::printf("\n== slope ==\n");
    Grid g(80, 60);
    for (int x = 0; x < 80; ++x) fill(g, x, 59 - (x < 50 ? 50 - x : 0), x, 59, ElementType::Wall);
    // A 6x4 wooden block dropped onto the slope. The slope's top is row 9 + x,
    // so it lands on its lower-left corner and has the whole slope below it.
    fill(g, 12, 10, 17, 13, ElementType::Wood);
    nudge(g, 14, 9);
    run(g, 900, every);
    picture(g, "end");
}

void stack(int every) {
    std::printf("\n== stack ==\n");
    Grid g(60, 50);
    fill(g, 0, 45, 59, 49, ElementType::Wall);
    // Blocks of alternating material, so each is its own piece, each set two
    // cells further right than the one under it.
    for (int i = 0; i < 6; ++i) {
        const ElementType t = i % 2 == 0 ? ElementType::Wood : ElementType::Wall;
        fill(g, 20 + 2 * i, 40 - 5 * i, 27 + 2 * i, 44 - 5 * i, t);
    }
    picture(g, "start");
    nudge(g, 31, 14);
    run(g, 1200, every);
    picture(g, "end");
}

// A dam break with a raft downstream: a reservoir on the left held back by a
// wall, a shallow channel with a wooden raft afloat in it. The wall is removed
// once the raft has settled, and the raft's left edge and the water's flow
// beside it are printed -- how far, and how soon, the surge carries it.
void current(int every) {
    std::printf("\n== current ==\n");
    Grid g(240, 60);
    fill(g, 0, 55, 239, 59, ElementType::Wall);
    fill(g, 0, 10, 0, 54, ElementType::Wall);
    fill(g, 239, 30, 239, 54, ElementType::Wall);
    fill(g, 1, 15, 40, 54, ElementType::Water);   // the reservoir
    fill(g, 41, 15, 41, 54, ElementType::Wall);   // the dam
    fill(g, 42, 48, 238, 54, ElementType::Water); // the channel
    fill(g, 80, 45, 99, 47, ElementType::Wood);
    nudge(g, 85, 44);
    for (int s = 0; s < 300; ++s) g.update();
    const auto left = [&] {
        for (int x = 0; x < 240; ++x)
            for (int y = 0; y < 60; ++y)
                if (g.get_element(x, y).type == ElementType::Wood) return x;
        return -1;
    };
    const int start = left();
    fill(g, 41, 15, 41, 54, ElementType::Empty);
    for (int s = 0; s <= 600; ++s) {
        if (s % every == 0) {
            // The liquid's sideways flow (raw Element::flow units) summed over
            // the rows the raft floats in, from 20 cells behind it to its far end.
            int flow = 0;
            const int l = left();
            for (int y = 44; y <= 50; ++y)
                for (int x = l - 20; x < l + 20; ++x)
                    if (x >= 0 && x < 240) flow += g.get_element(x, y).flow;
            std::printf("  step %4d  raft left %3d (moved %+d)  bodies %d  flow %d\n", s, l,
                        l - start, g.tipping_count(), flow);
        }
        g.update();
    }
    picture(g, "end");
}

// A painted wooden slab dropped into a pond from several heights and x
// offsets, its rows against the pond's still surface printed every `every`
// steps: it should sink in a little, bob, and settle with its top above the
// water and its bottom below it -- never come to rest clear of the water.
void raft(int every) {
    std::printf("\n== raft ==\n");
    const int SURFACE = 60;
    for (const int drop : {2, 10, 30, 45}) {
        for (const int dx : {0, 37, 71}) {
            for (const int narrow : {0, 1}) {
                // narrow: a pond only a few cells wider than the slab, where the
                // splash has nowhere to go but up its sides.
                const int lo = narrow ? 55 + dx : 1, hi = narrow ? 94 + dx : 238;
                Grid g(240, 100);
                fill(g, 0, 90, 239, 99, ElementType::Wall);
                fill(g, 0, 40, lo - 1, 89, ElementType::Wall);
                fill(g, hi + 1, 40, 239, 89, ElementType::Wall);
                fill(g, lo, SURFACE, hi, 89, ElementType::Water);
                const int x0 = 60 + dx, y1 = SURFACE - 1 - drop;
                fill(g, x0, y1 - 3, x0 + 29, y1, ElementType::Wood);
                nudge(g, x0 + 10, y1 - 4);
                int top = 0, bottom = 0, worst = 99, rest = -1;
                for (int s = 1; s <= 1200; ++s) {
                    g.update();
                    top = 99;
                    bottom = -1;
                    for (int y = 0; y < 100; ++y)
                        for (int x = 1; x < 239; ++x)
                            if (g.get_element(x, y).type == ElementType::Wood) {
                                top = std::min(top, y);
                                bottom = std::max(bottom, y);
                            }
                    if (s > 120) worst = std::min(worst, bottom - SURFACE);
                    if (every > 0 && s % every == 0)
                        std::printf("    drop %2d dx %2d step %4d  top %+d bottom %+d  bodies %d\n",
                                    drop, dx, s, top - SURFACE, bottom - SURFACE,
                                    g.tipping_count());
                    if (rest < 0 && g.tipping_count() == 0) rest = s;
                }
                if (narrow && drop == 30 && dx == 0) {
                    for (int y = 50; y < 66; ++y) {
                        std::string row = "    ";
                        for (int x = lo - 2; x <= hi + 2; ++x)
                            row += glyph(g.get_element(x, y).type);
                        std::printf("%s  %d\n", row.c_str(), y - SURFACE);
                    }
                }
                std::printf("  %s drop %2d dx %2d: end top %+d bottom %+d, highest bottom after "
                            "step 120 %+d, first at rest step %d\n",
                            narrow ? "narrow" : "wide  ", drop, dx, top - SURFACE, bottom - SURFACE,
                            worst, rest);
            }
        }
    }
    // A droplet stuck beside the slab's top row, after it has settled: does
    // the slab count as submerged and rise out of the pond?
    for (const bool kept : {false, true}) {
        Grid g(240, 100);
        fill(g, 0, 90, 239, 99, ElementType::Wall);
        fill(g, 0, 40, 0, 89, ElementType::Wall);
        fill(g, 239, 40, 239, 89, ElementType::Wall);
        fill(g, 1, SURFACE, 238, 89, ElementType::Water);
        fill(g, 60, SURFACE - 12, 89, SURFACE - 3, ElementType::Wood);
        nudge(g, 70, SURFACE - 13);
        const auto rows = [&](int& top, int& bottom) {
            top = 99;
            bottom = -1;
            for (int y = 0; y < 100; ++y)
                for (int x = 1; x < 239; ++x)
                    if (g.get_element(x, y).type == ElementType::Wood) {
                        top = std::min(top, y);
                        bottom = std::max(bottom, y);
                    }
        };
        for (int s = 0; s < 600; ++s) g.update();
        int top = 0, bottom = 0;
        rows(top, bottom);
        std::printf("  droplet (%s): settled top %+d bottom %+d\n", kept ? "kept" : "once",
                    top - SURFACE, bottom - SURFACE);
        for (int s = 1; s <= 300; ++s) {
            // A water cell on the slab's top-left corner's outside, once, or
            // kept there every step as a film clinging to it would be.
            int l = 239;
            for (int x = 1; x < 239; ++x)
                if (g.get_element(x, top).type == ElementType::Wood) {
                    l = x;
                    break;
                }
            if ((kept || s == 1) && l > 1 && g.get_element(l - 1, top).type == ElementType::Empty)
                g.set_element(l - 1, top, ElementType::Water);
            g.update();
            rows(top, bottom);
            if (s % 30 == 0)
                std::printf("    droplet step %3d top %+d bottom %+d\n", s, top - SURFACE,
                            bottom - SURFACE);
        }
    }
}

// Runs until nothing moves or `budget` steps, counting each body that starts
// (age 1: dropped, struck or pushed awake) and the oldest any body got. A body
// frozen at BODY_MAX_STEPS shows as an oldest age at the cap.
void settle(Grid& g, const char* name, int budget) {
    int wakes = 0, oldest = 0, rest = -1;
    for (int s = 1; s <= budget; ++s) {
        g.update();
        for (int i = 0; i < g.tipping_count(); ++i) {
            const Grid::BodyState b = g.body_state(i);
            if (b.age == 1) ++wakes;
            oldest = std::max(oldest, b.age);
        }
        if (!g.has_pending_support_checks() && s > 5) {
            rest = s;
            break;
        }
    }
    std::printf("  %-28s at rest step %5d  wakes %3d  oldest body %4d steps\n", name, rest, wakes,
                oldest);
}

void pile(int) {
    std::printf("\n== pile ==\n");
    for (int n = 3; n <= 5; ++n) {
        for (const int off : {0, 3, 6}) {
            Grid g(80, 80);
            fill(g, 0, 75, 79, 79, ElementType::Wall);
            // Blocks of alternating material, so each is its own piece, stacked
            // in the air with gaps and offsets so they land on one another.
            for (int i = 0; i < n; ++i) {
                const ElementType t = i % 2 == 0 ? ElementType::Wood : ElementType::Wall;
                const int x0 = 30 + (i % 2 ? off : -off / 2), y1 = 70 - 9 * i;
                fill(g, x0, y1 - 3, x0 + 7 + i, y1, t);
            }
            for (int i = 0; i < n; ++i) nudge(g, 31, 70 - 9 * i - 4);
            char name[64];
            std::snprintf(name, sizeof name, "pile n=%d off=%d", n, off);
            settle(g, name, 2400);
        }
    }
    // A slab 30 wide in a pond `gap` cells wider on each side.
    for (const int gap : {3, 5, 8}) {
        for (const int drop : {2, 10, 20, 30}) {
            Grid g(240, 100);
            fill(g, 0, 90, 239, 99, ElementType::Wall);
            fill(g, 0, 40, 59 - gap, 89, ElementType::Wall);
            fill(g, 90 + gap, 40, 239, 89, ElementType::Wall);
            fill(g, 60 - gap, 60, 89 + gap, 89, ElementType::Water);
            const int y1 = 59 - drop;
            fill(g, 60, y1 - 3, 89, y1, ElementType::Wood);
            nudge(g, 70, y1 - 4);
            char name[64];
            std::snprintf(name, sizeof name, "pond gap %d drop %d", gap, drop);
            settle(g, name, 2400);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* only = argc > 1 ? argv[1] : nullptr;
    const int every = argc > 2 ? static_cast<int>(std::strtol(argv[2], nullptr, 10)) : 20;
    const auto want = [&](const char* name) { return !only || std::strcmp(only, name) == 0; };
    if (want("disc")) disc(every);
    if (want("lip")) lip(every);
    if (want("plank")) plank(every);
    if (want("dominoes")) dominoes(every);
    if (want("seesaw")) seesaw(every);
    if (want("float")) floating(every);
    if (want("slope")) slope(every);
    if (want("stack")) stack(every);
    if (want("current")) current(every);
    if (want("raft")) raft(every);
    if (want("pile")) pile(every);
    return 0;
}
