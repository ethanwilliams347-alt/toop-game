#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <string>
#include "physics/grid.h"
#include "render/light.h"
#include "test_util.h"

// The light field, headless: LightField is deliberately SDL-free, so the thing
// worth testing is testable the same way the simulation is.
//
// Every claim here that light reaches somewhere is paired with a control
// asserting it does not reach somewhere else. A test that only checks for
// brightness is passed by a field that lights the entire world, and by one that
// lights only the source block -- which is the most likely way this file breaks.

namespace {

// Channel accessors on the packed ARGB texels, so the tests read the same bytes
// the GPU will.
int red(uint32_t argb)   { return (argb >> 16) & 0xFF; }
int green(uint32_t argb) { return (argb >> 8) & 0xFF; }
int blue(uint32_t argb)  { return argb & 0xFF; }

// Rough perceptual brightness is not the point here -- the tests compare light
// to light, so the sum of the channels is enough and is monotonic in all three.
int brightness(uint32_t argb) { return red(argb) + green(argb) + blue(argb); }

// Bounds-checked on purpose. The obvious indexing expression wraps a past-the-end
// column onto the next row's first block, which reads as a plausible brightness
// rather than as an error. Anything out of range is a broken test, so it aborts
// rather than returning a number.
uint32_t at(const LightField& light, int bx, int by) {
    if (bx < 0 || by < 0 || bx >= light.cols() || by >= light.rows()) {
        std::printf("[FAIL] probe (%d,%d) is outside a %dx%d field\n",
                    bx, by, light.cols(), light.rows());
        std::exit(1);
    }
    return light.pixels()[static_cast<size_t>(by) * light.cols() + bx];
}

// A world with nothing in it but air, so a test can place exactly what it means
// to and nothing else is emitting or occluding.
Grid empty_world(int w, int h) {
    return Grid(w, h, 1234);
}

// Every scene below is authored in cells, and almost every assertion is in
// blocks -- reach, beam width, the round-glow contour, the lit percentage. The
// two are connected only by LightField::BLOCK, so a change to BLOCK silently
// rescales every scene in this file relative to what is measuring it: a gap
// authored as 12 cells is three blocks wide at BLOCK=4 and one and a half at
// BLOCK=8, and "the beam's edge is at least 1.5 blocks" stops meaning what it
// says.
//
// So cell coordinates go through cells(), which holds each scene's footprint in
// blocks fixed at whatever BLOCK is. The literals stay the numbers these tests
// were written and debugged with, and the suite stops depending on a constant it
// is not testing.
constexpr int c(int reference_cells) {
    return reference_cells * LightField::BLOCK / 4;
}

} // namespace

int main() {
    constexpr int W = c(128), H = c(128);
    constexpr int B = LightField::BLOCK;

    // --- the region-to-block derivation ---
    {
        LightField light(10, 10);
        // Rounded up, so the field always covers at least the region asked for.
        // Rounding down would leave an unlit strip along the right and bottom of
        // the view, which is the edge the camera is usually panning towards.
        check("blocks cover the whole region",
              light.cols() * B >= 10 && light.rows() * B >= 10,
              std::to_string(light.cols()) + "x" + std::to_string(light.rows()) + " blocks");
        check("blocks do not overshoot by a whole block",
              (light.cols() - 1) * B < 10 && (light.rows() - 1) * B < 10);
    }

    // --- a world nobody has set fire to costs nothing and shows nothing ---
    {
        Grid grid = empty_world(W, H);
        for (int y = 64; y < H; ++y)
            for (int x = 0; x < W; ++x) grid.set_element(x, y, ElementType::Wall);

        LightField light(64, 64);
        light.update(grid, 0, 0);

        bool all_black = true;
        for (uint32_t t : light.pixels()) if (brightness(t) != 0) all_black = false;

        check("an unlit world reports no light", !light.any_light());
        check("an unlit world produces a black field", all_black);
    }

    // --- a hot cell lights its own block ---
    {
        Grid grid = empty_world(W, H);
        grid.set_element(32, 32, ElementType::Fire); // spawns at 250

        LightField light(64, 64);
        light.update(grid, 0, 0);

        check("a fire cell reports light", light.any_light());
        check("the block containing the fire is lit", brightness(at(light, 32 / B, 32 / B)) > 0);
    }

    // --- propagation reaches, and stops ---
    //
    // The paired assertion. `near` proves light leaves the block it was emitted in;
    // `far` proves it does not simply fill the buffer. A field that lit everything,
    // or one that lit only the source block, fails exactly one of these.
    {
        // Sized from ITERATIONS rather than a fixed number of cells: the `far`
        // probe deliberately sits past the reach bound, so a field that does not
        // extend past that bound has nowhere to put it. Raising ITERATIONS is what
        // walks the probe off the edge of the field, and `at` aborting is the
        // check that catches it.
        const int reach_w = (32 / B + LightField::ITERATIONS + 4) * B;
        Grid grid = empty_world(reach_w, reach_w);
        grid.set_element(32, 32, ElementType::Fire);

        LightField light(reach_w, reach_w);
        light.update(grid, 0, 0);

        const int src_bx = 32 / B, src_by = 32 / B;
        const int source = brightness(at(light, src_bx, src_by));
        const int near   = brightness(at(light, src_bx + 2, src_by));
        // Past ITERATIONS blocks of reach, and still inside the field -- a probe
        // outside it wraps onto the next row, which is why `at` refuses to index
        // out of range.
        const int far    = brightness(at(light, src_bx + LightField::ITERATIONS + 2, src_by));

        check("light carries into open air beyond its own block", near > 0,
              "near=" + std::to_string(near));
        check("light is dimmer further from the source", near < source,
              "source=" + std::to_string(source) + " near=" + std::to_string(near));
        check("light does not fill the whole field", far == 0,
              "far=" + std::to_string(far));
    }

    // --- the falloff is not lopsided ---
    //
    // What this catches is an asymmetric neighbour gather -- a dropped or mistyped
    // branch in the max, which is easy to write and produces a glow visibly
    // off-centre from the flame casting it.
    //
    // What it does not catch, checked rather than assumed, is the in-place sweep the
    // double buffering in update() exists to prevent. An in-place sweep carries much
    // further in the direction it walks, but both directions still converge on the
    // same value within a few blocks of the source, so every probe here agrees. The
    // assertion that fails on that mutation is the reach bound above, because the
    // extra distance is the whole of what an in-place sweep changes near the source.
    //
    // Recorded because a test whose stated purpose is not what it detects retires a
    // worry that is still live.
    {
        Grid grid = empty_world(W, H);
        grid.set_element(64, 64, ElementType::Fire);

        LightField light(128, 128);
        light.update(grid, 0, 0);

        const int bx = 64 / B, by = 64 / B;
        bool symmetric = true;
        for (int d = 1; d <= 6; ++d) {
            if (at(light, bx - d, by) != at(light, bx + d, by)) symmetric = false;
            if (at(light, bx, by - d) != at(light, bx, by + d)) symmetric = false;
        }
        check("the falloff is symmetric about the source", symmetric);
    }

    // --- terrain shadows itself ---
    //
    // A flame in a pit lights nothing behind the pit's wall. Two probes the same
    // distance from the same source, one across open air and one across rock, so
    // distance is held constant and the only difference is what is in the way.
    {
        Grid grid = empty_world(W, H);
        grid.set_element(64, 64, ElementType::Fire);
        // A wall to the right of the fire, spanning the probe's path. Nothing to
        // the left, which is the control.
        for (int x = 68; x < 80; ++x)
            for (int y = 40; y < 90; ++y) grid.set_element(x, y, ElementType::Wall);

        LightField light(128, 128);
        light.update(grid, 0, 0);

        // The distance is free to follow the reach -- what this test asserts is the
        // difference between two probes at equal distance. What it must never do is
        // compare a lit probe against one that is dark for the ordinary reason that
        // nothing was ever going to reach it.
        const int bx = 64 / B, by = 64 / B;
        const int through_rock = brightness(at(light, bx + 3, by));
        const int through_air  = brightness(at(light, bx - 3, by));

        check("light is stopped by solid terrain", through_rock < through_air,
              "rock=" + std::to_string(through_rock) + " air=" + std::to_string(through_air));
        check("the open side is genuinely lit", through_air > 0);
    }

    // --- a one-cell wall is a wall ---
    //
    // The test above uses a slab many cells thick, which every version of this code
    // has stopped easily, and it is not the wall a player draws. Occlusion is
    // averaged over a block, so a single-cell wall reads as a fraction opaque, and a
    // linear blend between the clear and solid transmission figures hands that block
    // most of its light through.
    //
    // Paired as everything in this file is: the same probe distance across open air
    // is the control, so this measures the wall and not the falloff.
    {
        Grid grid = empty_world(W, H);
        grid.set_element(64, 64, ElementType::Fire);
        for (int y = 40; y < 90; ++y) grid.set_element(72, y, ElementType::Wall);

        LightField light(128, 128);
        light.update(grid, 0, 0);

        const int bx = 64 / B, by = 64 / B;
        const int behind = brightness(at(light, bx + 3, by));  // past the wall
        const int open   = brightness(at(light, bx - 3, by));  // same distance, no wall

        check("the control across open air is lit", open > 0,
              "open=" + std::to_string(open));
        // Not "is zero": one cell of rock is not twelve, and something getting
        // through is correct. What matters is how much -- a third is the line
        // between a wall that shadows and a wall you can see through.
        check("a wall one cell thick casts a real shadow", behind * 3 < open,
              "behind=" + std::to_string(behind) + " open=" + std::to_string(open));
    }

    // --- what glows is heat, not the material ---
    //
    // In a grid of its own, with no fire anywhere. Sharing one grid with the flame
    // above puts this probe inside that flame's reach, so it measures propagated
    // light and calls it emission -- the two have to be separated by the scene,
    // because the field itself cannot tell them apart.
    {
        Grid grid = empty_world(W, H);
        grid.set_element(32, 32, ElementType::Charred);  // burnable, but nothing has lit it

        LightField cold(64, 64);
        cold.update(grid, 0, 0);
        check("a cold cell of a burnable material does not glow", !cold.any_light());
    }

    // --- the ramp stays warm ---
    {
        Grid grid = empty_world(W, H);
        grid.set_element(32, 32, ElementType::Fire); // 250

        LightField hot(64, 64);
        hot.update(grid, 0, 0);

        const uint32_t flame = at(hot, 32 / B, 32 / B);
        check("emission is warm - red leads, blue trails",
              red(flame) >= green(flame) && green(flame) >= blue(flame),
              std::to_string(red(flame)) + "," + std::to_string(green(flame)) + "," +
                  std::to_string(blue(flame)));
    }

    // --- a big fire does not blow the frame out ---
    //
    // A light field that covers most of the viewport at peak brightness composites
    // additively over the scene as a flat white plateau with the terrain invisible
    // inside it. Three things prevent it and all three are asserted here rather than
    // described: emission is tone-mapped so it cannot reach the top of the channel,
    // it scales with how much of a block is burning, and the falloff is steep enough
    // that a fire does not light everything in the world.
    //
    // Deliberately a large fire, because every earlier test in this file uses a
    // single cell -- and a single cell is exactly the case that looks fine while the
    // real thing is unusable.
    {
        Grid grid = empty_world(W, H);
        for (int y = 60; y < 70; ++y)
            for (int x = 40; x < 90; ++x) grid.set_element(x, y, ElementType::Fire);

        LightField light(128, 128);
        light.update(grid, 0, 0);

        int peak = 0;
        for (uint32_t t : light.pixels())
            peak = std::max(peak, std::max(red(t), std::max(green(t), blue(t))));

        // Counted at a third of peak, not at non-zero. Counting any non-zero block
        // calls a channel value of 1/255 lit, which is invisible on screen -- by
        // that measure this field covers almost the whole view and always will,
        // since the falloff has an exponential tail. What washed out means is a
        // large area at a brightness that competes with the scene.
        int bright_blocks = 0;
        for (uint32_t t : light.pixels())
            if (std::max(red(t), std::max(green(t), blue(t))) > peak / 3) ++bright_blocks;
        const int lit_pct = bright_blocks * 100 / static_cast<int>(light.pixels().size());

        // Headroom below 255 is the whole point: this layer is added to a scene
        // that already has brightness of its own, so a light field that reaches the
        // top of the channel on its own guarantees clipping in the blend.
        check("a large fire leaves headroom in the channel", peak < 200,
              "peak=" + std::to_string(peak));
        // And it must still be bright enough to be worth having.
        check("a large fire is still clearly bright", peak > 60,
              "peak=" + std::to_string(peak));
        check("a large fire does not wash out the whole view", lit_pct < 45,
              "brightly lit=" + std::to_string(lit_pct) + "%");
    }

    // --- the falloff is round, not diamond-shaped ---
    //
    // Propagating to four neighbours only makes distance Manhattan, so a glow
    // reaches furthest along the axes and appears on screen as vertical and
    // horizontal shafts radiating out of every fire. The diagonal neighbours cost
    // more to cross than the orthogonal ones; if they ever cost the same, the
    // artefact rotates 45 degrees rather than going away, so this compares the two
    // directions rather than merely checking the diagonals are gathered.
    {
        Grid grid = empty_world(W, H);
        grid.set_element(64, 64, ElementType::Fire);

        LightField light(128, 128);
        light.update(grid, 0, 0);

        const int bx = 64 / B, by = 64 / B;
        const int along_axis = brightness(at(light, bx + 4, by));
        const int diagonal   = brightness(at(light, bx + 3, by + 3));

        // Four blocks straight out is nearer than three-and-three diagonally (4 vs
        // 4.24), so it must be brighter -- but not by the margin a Manhattan metric
        // would give, where three-and-three costs six steps.
        check("the falloff reaches further along an axis than diagonally",
              along_axis > diagonal,
              "axis=" + std::to_string(along_axis) + " diag=" + std::to_string(diagonal));
        check("the diagonal is not starved the way a 4-neighbour walk starves it",
              diagonal * 2 > along_axis,
              "axis=" + std::to_string(along_axis) + " diag=" + std::to_string(diagonal));
    }

    // --- the falloff is round in every direction, not just two ---
    //
    // The test above compares one axis against one diagonal and bounds their ratio,
    // and it cannot catch an octagonal glow: light here travels by orthogonal,
    // diagonal and knight's-move steps, which measures distance as a chamfer metric,
    // and a chamfer metric's worst error is at 22.5 degrees -- exactly halfway
    // between the two directions that test samples. A two-sample test of an
    // eight-lobed artefact looks where the artefact is not.
    //
    // So this sweeps the whole circle and measures what the eye judges: how far the
    // glow gets before it dies, as a function of angle. A round glow reaches the
    // same distance whichever way you look; an octagonal one reaches further at the
    // eight points than between them, and the ratio of the two is the number the
    // shafts are made of.
    //
    // Measuring reach rather than brightness-at-a-radius is deliberate. It needs to
    // know nothing about TRANSMIT_CLEAR, MAX_EMISSION or the tone curve, so retuning
    // any of those moves this test's inputs without moving what it asserts.
    {
        Grid grid = empty_world(W, H);
        for (int y = 63; y <= 65; ++y)
            for (int x = 63; x <= 65; ++x) grid.set_element(x, y, ElementType::Fire);

        LightField light(128, 128);
        light.update(grid, 0, 0);

        const double cx = 64.0 / B, cy = 64.0 / B;

        // Bilinear, and a nearest-block version of this test is wrong. Sampling
        // the nearest block quantises each reach by up to half a block, which at
        // these distances is larger than the artefact being measured and is
        // angle-dependent, because an axis ray lands on block centres and a
        // 30-degree one does not -- so the ratio does not move when the metric
        // does. Interpolating is also what the GPU does to this texture, so it is
        // the shape the player sees rather than the one in the array.
        const auto sample = [&](double fx, double fy) -> double {
            const double gx = std::clamp(fx, 0.0, light.cols() - 1.0001);
            const double gy = std::clamp(fy, 0.0, light.rows() - 1.0001);
            const int x0 = static_cast<int>(gx), y0 = static_cast<int>(gy);
            const double tx = gx - x0, ty = gy - y0;
            const double a = brightness(at(light, x0, y0));
            const double b = brightness(at(light, x0 + 1, y0));
            const double c = brightness(at(light, x0, y0 + 1));
            const double d = brightness(at(light, x0 + 1, y0 + 1));
            return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty;
        };

        // A contour well inside the glow rather than at its dying edge. The
        // outermost ring is where propagation hit MIN_VISIBLE and cut to zero, so
        // it is a step and not a gradient -- there is no sub-block crossing there
        // to find, and interpolating across it would put the quantisation back in.
        constexpr double CONTOUR = 24.0;
        constexpr double STEP = 0.05;
        double nearest = 1e9, furthest = 0.0;
        int nearest_deg = 0, furthest_deg = 0;
        for (int deg = 0; deg < 360; deg += 3) {
            const double rad = deg * 3.14159265358979 / 180.0;
            const double ux = std::cos(rad), uy = std::sin(rad);
            double reach = 0.0, prev = sample(cx, cy), prev_t = 0.0;
            for (double t = STEP; t < 24.0; t += STEP) {
                const double v = sample(cx + ux * t, cy + uy * t);
                if (prev >= CONTOUR && v < CONTOUR) {
                    // Linear crossing between the two samples.
                    reach = prev_t + STEP * (prev - CONTOUR) / (prev - v);
                }
                prev = v;
                prev_t = t;
            }
            if (reach < nearest)  { nearest = reach;  nearest_deg = deg; }
            if (reach > furthest) { furthest = reach; furthest_deg = deg; }
        }

        const double ratio = nearest > 0.0 ? furthest / nearest : 1e9;
        const std::string detail =
            "furthest " + std::to_string(furthest) + " blocks at " +
            std::to_string(furthest_deg) + " deg, nearest " +
            std::to_string(nearest) + " at " + std::to_string(nearest_deg) +
            " deg, ratio " + std::to_string(ratio);

        // The glow must exist before its shape is worth asserting -- a field that
        // reached nowhere would have a perfect ratio of 1.
        check("a lone flame's glow carries a measurable distance", nearest > 2.0,
              detail);
        // The threshold is not a taste: bilinear sampling still carries a few
        // percent of quantisation at these distances, and the chamfer metric's own
        // worst case is about 8%. A field that is round within its own measurement
        // noise lands well under this; an octagonal one does not.
        check("the glow is round at every angle, not only on the axes and diagonals",
              ratio < 1.25, detail);
    }

    // --- light through a gap has a soft edge, not a cut one ---
    //
    // The other half of hard rays and shafts, and the half the round-glow test above
    // cannot see, because it looks at a flame alone in open air.
    //
    // Max-propagation has no penumbra of any kind. A block either has a route to the
    // light or it does not, and the brightest route wins outright, so the boundary
    // between lit-through-the-gap and shadow is a step one block wide. Real light
    // through an opening has a soft edge whose width grows with distance from the
    // opening; this has a straight-sided beam with a hard edge at any distance,
    // which is exactly what a ray or a shaft looks like. Nothing about it is fixed
    // by the metric being round.
    //
    // So the assertion is on the width of the transition, measured across the beam
    // well behind the wall. A step is under a block; anything the smoothing pass has
    // softened is wider.
    {
        Grid grid = empty_world(W, H);
        for (int x = 0; x < W; ++x) {
            if (x < 56 || x > 67) grid.set_element(x, 64, ElementType::Wall);
        }
        for (int y = 50; y <= 60; ++y)
            for (int x = 56; x <= 67; ++x) grid.set_element(x, y, ElementType::Fire);

        LightField light(128, 128);
        light.update(grid, 0, 0);

        // Six blocks past the wall, scan sideways out of the beam.
        const int row = 64 / B + 2;
        double peak = 0.0;
        for (int bx = 0; bx < light.cols(); ++bx)
            peak = std::max(peak, static_cast<double>(brightness(at(light, bx, row))));

        check("light gets through the gap at all", peak > 8.0,
              "peak across the beam=" + std::to_string(peak));

        // Walk out from the beam's centre and find where the profile crosses 80%
        // and 20% of its peak. The distance between them is the edge's width.
        const int centre = 64 / B;
        double at80 = -1.0, at20 = -1.0;
        for (int bx = centre; bx < light.cols(); ++bx) {
            const double v = brightness(at(light, bx, row));
            if (at80 < 0.0 && v < peak * 0.8) at80 = bx;
            if (at20 < 0.0 && v < peak * 0.2) { at20 = bx; break; }
        }
        const double width = (at80 >= 0.0 && at20 >= 0.0) ? at20 - at80 : -1.0;
        check("the beam's edge is a gradient rather than a cut", width >= 1.5,
              "80% at block " + std::to_string(at80) + ", 20% at " +
              std::to_string(at20) + ", edge width " + std::to_string(width) +
              " blocks");
    }

    // --- the same input gives the same field ---
    //
    // Not determinism for the simulation's sake -- this never feeds the simulation,
    // which is the point of it living in src/render/ -- but because a field that
    // drifted between identical frames would flicker on a still scene.
    {
        Grid grid = empty_world(W, H);
        grid.set_element(50, 50, ElementType::Fire);

        LightField a(64, 64), b(64, 64);
        a.update(grid, 0, 0);
        b.update(grid, 0, 0);
        // And the same object updated twice, which is the case that actually runs
        // every frame: update reuses its buffers, so state left over from the
        // previous frame is the plausible way this breaks.
        a.update(grid, 0, 0);

        check("the field is a pure function of the grid and the origin",
              a.pixels() == b.pixels());
    }

    // --- a field re-aimed away from the fire goes dark ---
    //
    // The origin argument is what makes this camera-relative, and getting it wrong
    // would light the wrong part of the world.
    {
        Grid grid = empty_world(W, H);
        grid.set_element(10, 10, ElementType::Fire);

        LightField light(32, 32);
        light.update(grid, 0, 0);
        check("the view containing the fire is lit", light.any_light());

        light.update(grid, 90, 90);
        check("a view far from the fire is dark", !light.any_light());
    }

    return report();
}
