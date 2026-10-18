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
//
// Usage: body_probe [scene] [every]. With no scene, runs them all.
#include "physics/grid.h"
#include <cstdio>
#include <cstdlib>
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
    return 0;
}
