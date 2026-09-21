// What a fire's timing and shape actually are, as numbers.
//
// Slower spread, longer consumption, a longer-lived Charred state and a less
// linear burn front are four requests and three of them are the same complaint
// said at different volumes. Tuning four constants against "still a little too
// fast" is how this goes wrong, so this exists to say which of them moved and by
// how much.
//
// Not a registered test. These are measurements, not assertions: the right burn
// duration is a matter of taste, and a test that pinned it would be a
// transcription of the constants rather than a check on them. What the suite
// does assert about fire lives in test_grid.cpp and is about behaviour that must
// never change -- that fire spreads at all, that water puts it out.
//
// Three probes, one per complaint:
//
//   spread      - how long a long plank takes to burn end to end.
//   consumption - how long a lit body of wood takes to stop existing, and how
//                 much of that time it spends as Charred rather than gone.
//   front       - how ragged the leading edge is, in cells of scatter across the
//                 plank's thickness. The only one that is about shape rather
//                 than duration.
#include "physics/grid.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>

namespace {

uint64_t WORLD_SEED = 4242;

constexpr int FPS = 60;  // the fixed step rate; steps are reported as seconds too

bool burnt(ElementType t) { return t != ElementType::Wood; }

// Where the burn front is in one row: the first cell along it that is still
// Wood. Deliberately the contiguous front rather than the furthest burnt cell.
// Flames are gas, they drift along an open surface, and one that lands well
// ahead and lights a spot there is a real thing a fire does -- but it is not the
// front, and letting a single spark define the front means the probe reports the
// fastest thing in the scene rather than the thing being tuned.
int front_of(const Grid& g, int x0, int y, int len) {
    for (int i = 0; i < len; ++i)
        if (!burnt(g.get_element(x0 + i, y).type)) return i;
    return len;
}

// A horizontal plank, lit at its left end by seeding the first column as Charred
// -- which is what ignition produces, so the probe starts where a real ignition
// leaves off rather than by dropping a flame and hoping it catches.
//
// It is a beam on two posts, and both alternatives are wrong. Floated, the
// collapse system drops it out from under the probe within a couple of steps and
// the reading is gravity called fire. Rested on a Wall floor, a wall is a heat
// sink big enough to quench any fire touching it, so a plank lying along one is
// a fire fighting the largest cold body in the scene for its whole length -- it
// cannot propagate at all once ignition points vary, and the reading looks like
// a defect in the jitter.
//
// Two posts hold it up and touch a few cells of it. The beam itself burns in
// air, which is both the honest test and the thing a tester is looking at.
Grid plank(int len, int thick, int& out_x, int& out_y) {
    const int GW = len + 20, GH = thick + 40;
    Grid g(GW, GH, WORLD_SEED);
    out_x = 10;
    out_y = 10;
    for (int i = 0; i < 3; ++i)
        for (int t = out_y + thick; t < GH; ++t) {
            g.set_element(out_x + i, t, ElementType::Wall);
            g.set_element(out_x + len - 1 - i, t, ElementType::Wall);
        }
    for (int i = 0; i < len; ++i)
        for (int t = 0; t < thick; ++t) g.set_element(out_x + i, out_y + t, ElementType::Wood);
    for (int t = 0; t < thick; ++t) g.set_element(out_x, out_y + t, ElementType::Charred);
    return g;
}

void probe_spread() {
    constexpr int LEN = 150, THICK = 3;
    int x0, y0;
    Grid g = plank(LEN, THICK, x0, y0);

    // The far end is what burn-through means. Waiting for the whole plank to be gone
    // would measure consumption as well as travel, and those are two different
    // constants.
    int steps = 0;
    const int LIMIT = 20000;
    while (steps < LIMIT) {
        int furthest = 0;
        for (int t = 0; t < THICK; ++t) {
            const int f = front_of(g, x0, y0 + t, LEN);
            if (f > furthest) furthest = f;
        }
        if (furthest >= LEN) break;
        g.update();
        ++steps;
    }
    std::printf("spread:      %d cells in %d steps (%.2f s) = %.2f cells/s\n",
                LEN, steps, static_cast<double>(steps) / FPS,
                steps ? static_cast<double>(LEN) * FPS / steps : 0.0);
}

void probe_consumption() {
    // Short and thick, so travel time is a small part of the answer and what is left
    // is how long the material resists being consumed.
    constexpr int LEN = 24, THICK = 24;
    int x0, y0;
    Grid g = plank(LEN, THICK, x0, y0);

    int steps = 0, peak_charred = 0, charred_area = 0;
    const int LIMIT = 20000;
    while (steps < LIMIT) {
        int wood = 0, charred = 0;
        for (int i = 0; i < LEN; ++i)
            for (int t = 0; t < THICK; ++t) {
                const ElementType e = g.get_element(x0 + i, y0 + t).type;
                if (e == ElementType::Wood) ++wood;
                else if (e == ElementType::Charred) ++charred;
            }
        if (charred > peak_charred) peak_charred = charred;
        charred_area += charred;
        if (wood == 0 && charred == 0) break;
        g.update();
        ++steps;
    }

    // Mean Charred lifetime, recovered from the area under the count rather than by
    // tracking individual cells: every cell that ever charred contributes its own
    // lifetime to that area exactly once, and the body starts as LEN*THICK cells all
    // of which pass through Charred on their way out.
    const double cells = static_cast<double>(LEN) * THICK;
    std::printf("consumption: %dx%d body gone in %d steps (%.2f s); "
                "peak charred %d cells, mean charred life %.0f steps (%.2f s)\n",
                LEN, THICK, steps, static_cast<double>(steps) / FPS, peak_charred,
                charred_area / cells, charred_area / cells / FPS);
}

void probe_front() {
    // Not the scatter of the per-row fronts. A body of wood burns its exposed
    // surface far faster than its core -- flame is a gas, it drifts along an open
    // face and lights what it touches, while the interior waits on conduction -- so
    // on a thick plank the top rows are through the whole length before the bottom
    // rows have moved. Nearly all the scatter such a probe reports is that
    // stratification. It is real, it is not a defect, and it is not what "the front
    // could be less linear" is about.
    //
    // What the complaint is about is whether cells in comparable positions ignite at
    // noticeably different times. So: record the step each cell stops being Wood,
    // fit a straight line to each row, and report how far the cells sit off their
    // own row's line. A front that sweeps along at a fixed speed fits perfectly and
    // scores zero however fast it is going; a front that gnaws unevenly does not.
    // Reported in cells as well as steps, because a scatter of ten steps means
    // nothing without knowing how far the front moves in ten steps.
    constexpr int LEN = 150, THICK = 6;
    int x0, y0;
    Grid g = plank(LEN, THICK, x0, y0);

    constexpr int NEVER = -1;
    std::vector<int> lit(static_cast<size_t>(LEN) * THICK, NEVER);

    int steps = 0;
    const int LIMIT = 20000;
    while (steps < LIMIT) {
        int remaining = 0;
        for (int t = 0; t < THICK; ++t)
            for (int i = 0; i < LEN; ++i) {
                const size_t k = static_cast<size_t>(t) * LEN + i;
                if (lit[k] != NEVER) continue;
                if (burnt(g.get_element(x0 + i, y0 + t).type)) lit[k] = steps;
                else ++remaining;
            }
        if (remaining == 0) break;
        g.update();
        ++steps;
    }

    // Least squares per row, over the first cell onward -- the seeded column is not
    // an ignition and would drag every fit.
    double pooled = 0, pooled_slope = 0;
    int rows = 0, n_total = 0;
    for (int t = 0; t < THICK; ++t) {
        double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;
        for (int i = 1; i < LEN; ++i) {
            const int s = lit[static_cast<size_t>(t) * LEN + i];
            if (s == NEVER) continue;
            n += 1; sx += i; sy += s; sxx += double(i) * i; sxy += double(i) * s;
        }
        if (n < 3) continue;
        const double denom = n * sxx - sx * sx;
        if (denom == 0) continue;
        const double b = (n * sxy - sx * sy) / denom;  // steps per cell
        const double a = (sy - b * sx) / n;

        double resid = 0;
        for (int i = 1; i < LEN; ++i) {
            const int s = lit[static_cast<size_t>(t) * LEN + i];
            if (s == NEVER) continue;
            const double d = s - (a + b * i);
            resid += d * d;
        }
        pooled += resid;
        pooled_slope += b * n;
        n_total += static_cast<int>(n);
        ++rows;
    }

    const double rms = n_total ? std::sqrt(pooled / n_total) : 0.0;
    const double slope = n_total ? pooled_slope / n_total : 0.0;  // steps per cell
    std::printf("front:       whole plank burnt by step %d; front takes %.2f steps/cell, "
                "cells ignite %.1f steps off their row's line (%.2f cells of raggedness, %d rows)\n",
                steps, slope, rms, slope > 0 ? rms / slope : 0.0, rows);
}

} // namespace

int main(int argc, char** argv) {
    // Seeded from the command line because every constant tuned against this probe
    // sits near a cliff: below a certain conductivity, or above a certain ignition
    // point, a fire does not merely slow down, it goes out. Close to one of those
    // edges a single world is not evidence -- two neighbouring settings on one seed
    // can disagree about which is faster -- so the finalists get run across several.
    if (argc > 1) WORLD_SEED = std::strtoull(argv[1], nullptr, 10);
    std::printf("seed %llu\n", static_cast<unsigned long long>(WORLD_SEED));
    probe_spread();
    probe_consumption();
    probe_front();
    return 0;
}
