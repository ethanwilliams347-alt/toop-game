#include "render/light.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace {

// How much light survives crossing one cell of each kind. Per cell, not per
// block: a block's transmission is the product of what its cells pass, which is
// also what "crossing three cells of air and one of rock" means.
//
// Stating an occlusion fraction per cell and interpolating a block's
// transmission between a clear and a solid figure instead does not work, because
// both of those figures are per block. A solid-block transmission of 0.06 is
// 0.06^(1/BLOCK) = 0.49 per cell, which quietly claims a single cell of rock
// passes half the light hitting it -- a wall you can see through.
//
// Solid terrain nearly stops light; liquids dim it; gases barely count, which is
// what lets a flame light the smoke above it instead of being smothered by it.
// Empty stops nothing -- air is not simulated here and is not rendered as though
// it were.
constexpr float CELL_SOLID  = 0.12f;
constexpr float CELL_LIQUID = 0.75f;
constexpr float CELL_GAS    = 0.97f;

// Light surviving one block of travel. The clear-air figure sets the falloff of
// an unobstructed glow, and the solid figure is what makes a wall a wall.
//
// These are placeholder values and are expected to move. What does not move is
// how violently this compounds -- it is applied once per BLOCK cells, so the
// per-cell figure is its BLOCK'th root and small edits here are large edits on
// screen:
//
//   0.86 -> visible ~60 cells out; a fire of any size washes the whole screen.
//   0.72 -> visible ~35 cells out, brightly lit within ~12.
//   0.55 -> visible ~15 cells out, brightly lit within ~8. A tight rim light.
//
// Reach in cells is roughly BLOCK * ln(255 * MAX_EMISSION) / -ln(TRANSMIT_CLEAR).
// Worth having written down before reaching for the number.
//
// Every figure in that table is a reach in cells, and a cell is not a fixed
// fraction of the screen: the look is chosen as a multiple of the body's height,
// so this has to be re-derived from the formula whenever the body is rescaled,
// or the same constant silently means a different picture.
constexpr float TRANSMIT_CLEAR = 0.77f;

// The transmission of a block that is solid all the way through, as a
// consequence of CELL_SOLID rather than a number of its own. Kept as the name
// the smoothing pass and the tests reason about.
//
// Raised to LightField::BLOCK rather than written out four times: a block is
// BLOCK cells across, and this was one of two places that quietly assumed that
// number. Turning the knob with either left alone does not produce a coarser
// field, it produces a darker one, which reads as a tuning problem rather than
// as the arithmetic error it is.
const float TRANSMIT_SOLID = std::pow(CELL_SOLID, static_cast<float>(LightField::BLOCK));

// Clear air, per cell rather than per block -- the BLOCK'th root of the figure
// above. Hoisted so the gather pays one exp per block instead of a log per cell.
//
// The second of the two places that hardcoded the block size (see
// TRANSMIT_SOLID). Written as a fixed fraction it pins the whole falloff to one
// BLOCK value, and a coarser field would then transmit TRANSMIT_CLEAR squared
// per block -- halving the reach exactly where a coarser field was asked for to
// extend it.
const float LOG_CELL_AIR = std::log(TRANSMIT_CLEAR) / static_cast<float>(LightField::BLOCK);

// Per-cell log-transmission, summed over a block during the gather. The block's
// transmission is exp(sum/BLOCK): BLOCK^2 cells sampled, BLOCK of them crossed.
//
// A table built once at static init, not a switch returning std::log(constant):
// MSVC does not fold std::log of a constant at /O2, so the switch paid a real libm
// call for every cell of every block, every frame -- ~311k per frame at 1080p,
// most of the dark-frame cost. Defined after LOG_CELL_AIR because it reads it, and
// both are in this translation unit, so initialisation order is declaration order.
const std::array<float, static_cast<size_t>(ElementType::Count)> LOG_TRANSMIT = [] {
    std::array<float, static_cast<size_t>(ElementType::Count)> t{};
    for (size_t i = 0; i < t.size(); ++i) {
        const ElementType type = static_cast<ElementType>(i);
        if (type == ElementType::Empty) { t[i] = LOG_CELL_AIR; continue; }
        switch (material_of(type).move) {
            case MoveKind::Gas:    t[i] = std::log(CELL_GAS);    break;
            case MoveKind::Liquid: t[i] = std::log(CELL_LIQUID); break;
            default:               t[i] = std::log(CELL_SOLID);  break;  // Static and Powder
        }
    }
    return t;
}();

float cell_log_transmit(ElementType type) { return LOG_TRANSMIT[static_cast<size_t>(type)]; }

// Peak emission from a block that is entirely on fire, before tone mapping.
//
// Deliberate headroom past 1.0 is the wrong instinct here: this layer is
// composited additively over a scene that is already bright, so the clipping
// happens in the blend whatever this value is, and driving a signal past its
// ceiling destroys every gradient above the ceiling rather than only the peak --
// a flat white plateau where the fire is.
constexpr float MAX_EMISSION = 0.9f;

// A block emits according to how much of it is burning, not only how hot its
// hottest cell is. Taking the maximum temperature is right for deciding whether
// a block is a light source -- a single flame cell in a block of air must not be
// averaged away -- but using it for brightness too means one stray flame lights
// as hard as a solid wall of fire, which turns a ragged, mostly-empty flame
// front into a solid glowing slab.
//
// The floor keeps a lone flame clearly visible, which is the only thing it is
// for; the rest scales with coverage, which is the distinction that separates an
// ember from a blaze.
constexpr float COVERAGE_FLOOR = 0.1f;

// Light below one step of an 8-bit channel is not light, it is arithmetic. Cut
// to zero during propagation rather than at the end, so a glow has a definite
// edge instead of an exponential tail that is invisible but still costs
// iterations, and so the reach of a light is bounded by its own falloff rather
// than only by ITERATIONS.
constexpr float MIN_VISIBLE = 1.0f / 255.0f;

// Smoothing passes over the propagated field.
//
// Light here travels by orthogonal, diagonal and knight's-move steps, which
// measures distance as a chamfer metric rather than a Euclidean one, and a
// chamfer metric's iso-contours are octagons. Under max those octagon corners
// survive intact, because max never averages a neighbour into a result the way a
// sum would. The falloff is exponential, so a few percent of error in measured
// distance is a visible step in brightness, and eight of those steps arranged
// around a fire read as hard rays and shafts.
//
// The honest fix for the metric is more directions, which is quadratically more
// work per iteration. This is the cheap one: propagate as before, then low-pass
// the result. A 1-2-1 kernel run separably twice is a near-Gaussian of about one
// block -- wide enough to round the corners off, narrow enough not to move where
// the light is.
//
// The kernel is weighted by how open each neighbour is, so this does not undo
// the occlusion the propagation just paid for: a blur that averaged freely
// across a wall would pull light through it, and the wall is the one thing here
// that must stay hard.
constexpr int SMOOTH_PASSES = 2;
constexpr float SMOOTH_CENTRE = 2.0f;

// An iteration that moves no channel by more than this has converged, and the
// remaining iterations would be arithmetic nobody can see. Well below one step
// of an 8-bit channel.
constexpr float CONVERGED = 1.0f / 512.0f;

// Tone mapping. A hard clamp maps every value at or above 1.0 to the same white,
// so the whole interior of a fire -- exactly where the most light is --
// collapses into one flat shape with no structure in it.
//
// Reinhard's curve compresses instead: close to linear while dim, rolling off
// smoothly as it brightens, approaching 1 without reaching it, so there is
// always some difference between two bright values. One divide per channel per
// block.
float tone_map(float v) {
    return v <= 0.0f ? 0.0f : v / (1.0f + v);
}

} // namespace

LightField::LightField(int region_cells_w, int region_cells_h)
    : block_cols(std::max(1, (region_cells_w + BLOCK - 1) / BLOCK)),
      block_rows(std::max(1, (region_cells_h + BLOCK - 1) / BLOCK)) {
    const size_t count = static_cast<size_t>(block_cols) * block_rows;
    emission.assign(count, Rgb{});
    front.assign(count, Rgb{});
    back.assign(count, Rgb{});
    transmit.assign(count, TRANSMIT_CLEAR);
    transmit_diag.assign(count, TRANSMIT_CLEAR);
    transmit_knight.assign(count, TRANSMIT_CLEAR);
    texels.assign(count, 0xFF000000u);
}

void LightField::update(const Grid& grid, int origin_x, int origin_y) {
    const size_t count = static_cast<size_t>(block_cols) * block_rows;
    constexpr float CELLS_PER_BLOCK = static_cast<float>(BLOCK * BLOCK);

    // --- gather ---
    //
    // One pass over the region's cells, accumulating two things per block: the
    // hottest cell in it, and how much of it is solid. The hottest and not the
    // average, because a single flame cell in a block of air is a light source and
    // averaging it against its empty neighbours would divide it away.
    lit = false;
    for (int by = 0; by < block_rows; ++by) {
        for (int bx = 0; bx < block_cols; ++bx) {
            const size_t i = static_cast<size_t>(by) * block_cols + bx;

            uint8_t hottest = 0;
            float log_transmit = 0.0f;
            int glowing = 0;
            for (int cy = 0; cy < BLOCK; ++cy) {
                for (int cx = 0; cx < BLOCK; ++cx) {
                    const Element cell = grid.get_element(origin_x + bx * BLOCK + cx,
                                                          origin_y + by * BLOCK + cy);
                    hottest = std::max(hottest, cell.temperature);
                    log_transmit += cell_log_transmit(cell.type);
                    if (cell.temperature > GLOW_THRESHOLD) ++glowing;
                }
            }

            // The product of what the block's cells pass.
            //
            // Averaging the cells' occlusion and interpolating the block's
            // transmission linearly between a clear figure and a solid one averages
            // two numbers that only mean anything as exponents. A one-cell-thick
            // wall -- the commonest wall a player draws -- comes out as a
            // partly-occluded block that still passes most of its light.
            //
            // BLOCK^2 cells are sampled and BLOCK are crossed, so the mean
            // log-transmission times BLOCK is the exponent, which for a block
            // holding one cell of rock is exactly "three cells of air and one of
            // rock" and nothing else.
            const float k = std::exp(log_transmit / (CELLS_PER_BLOCK / BLOCK));
            transmit[i] = k;
            // k raised to the diagonal's length, sqrt(2).
            //
            // The exponent has to be exact, not approximated: the error compounds
            // once per block, so an approximation that is within a couple of percent
            // per block arrives roughly half as bright ten blocks out. Per unit of
            // distance that makes the diagonal cost more than the orthogonal, and
            // the field bulges along the axes -- which is one of the two things that
            // reads as shafts. A pow over the block grid is nothing next to
            // ITERATIONS sweeps of it.
            transmit_diag[i] = std::pow(k, 1.41421356f);
            // sqrt(5), the length of a (1,2) step.
            transmit_knight[i] = std::pow(k, 2.23606798f);

            if (hottest <= GLOW_THRESHOLD) {
                emission[i] = Rgb{};
                continue;
            }
            lit = true;

            // Temperature to colour. The ramp is the one a fire actually walks -- dull
            // red where something has only just caught, orange through the body of the
            // flame, pale towards white at the top of the byte -- and it is computed
            // from temperature rather than read off the cell's own colour on purpose.
            // A Charred cell is nearly black by design, so its rendered colour is the
            // worst possible guide to what it emits; heat is the thing both the dark
            // ember and the bright flame agree on.
            const float t = static_cast<float>(hottest - GLOW_THRESHOLD) /
                            static_cast<float>(255 - GLOW_THRESHOLD);
            const float coverage = static_cast<float>(glowing) / CELLS_PER_BLOCK;
            const float strength = MAX_EMISSION * t * std::sqrt(t) *
                                   (COVERAGE_FLOOR + (1.0f - COVERAGE_FLOOR) * coverage);
            emission[i] = Rgb{
                strength,
                strength * (0.34f + 0.46f * t),
                strength * (0.08f + 0.26f * t * t),
            };
        }
    }

    if (!lit) {
        std::fill(texels.begin(), texels.end(), 0xFF000000u);
        return;
    }

    // --- propagate ---
    //
    // Light spreads by taking the brightest of its neighbours and paying the
    // crossing cost, with its own emission as a floor. Max rather than a sum or an
    // average, which is what keeps this stable at any iteration count: a diffusion
    // that adds neighbours together grows without bound if it is run longer, so the
    // look would be a function of ITERATIONS rather than of the scene, and tuning
    // either one would move the other.
    //
    // Double-buffered because an in-place sweep propagates further in the direction
    // it happens to walk, and a fire lighting further to its right than its left is
    // obvious on screen and invisible in the code that caused it.
    front = emission;
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        float largest_change = 0.0f;
        for (int by = 0; by < block_rows; ++by) {
            for (int bx = 0; bx < block_cols; ++bx) {
                const size_t i = static_cast<size_t>(by) * block_cols + bx;

                // Outside the region contributes nothing. The grid reads as Wall past
                // its own edge, so the world's border already stops light on its own;
                // this only matters at the edges of the view, where light from
                // off-screen is light the player cannot see the source of anyway.
                //
                // Orthogonal and diagonal neighbours are gathered separately because
                // they are paid for at different rates -- see transmit_diag. Folded in
                // as two maxima rather than eight so the two costs stay
                // distinguishable.
                const bool l = bx > 0, r = bx < block_cols - 1;
                const bool u = by > 0, d = by < block_rows - 1;
                const int W = block_cols;

                Rgb best{};
                const auto fold = [&best, this](size_t j) {
                    best.r = std::max(best.r, front[j].r);
                    best.g = std::max(best.g, front[j].g);
                    best.b = std::max(best.b, front[j].b);
                };
                if (l) fold(i - 1);
                if (r) fold(i + 1);
                if (u) fold(i - W);
                if (d) fold(i + W);

                Rgb diag{};
                const auto fold_diag = [&diag, this](size_t j) {
                    diag.r = std::max(diag.r, front[j].r);
                    diag.g = std::max(diag.g, front[j].g);
                    diag.b = std::max(diag.b, front[j].b);
                };
                if (l && u) fold_diag(i - W - 1);
                if (r && u) fold_diag(i - W + 1);
                if (l && d) fold_diag(i + W - 1);
                if (r && d) fold_diag(i + W + 1);

                // The eight knight's moves, at sqrt(5). See transmit_knight.
                //
                // A (1,2) step steps over a block, so it is the one direction here that
                // could jump a wall, and a one-block wall is thin enough to be common.
                // The source is therefore taken as the lesser of the far block and the
                // block the step passes through: light can only arrive by a route it has
                // already lit. A wall in the way is dark, so the knight step carries the
                // wall's darkness rather than routing around it, and no occlusion test
                // is needed to say so.
                Rgb knight{};
                const auto fold_knight = [&knight, this](size_t j, size_t mid) {
                    knight.r = std::max(knight.r, std::min(front[j].r, front[mid].r));
                    knight.g = std::max(knight.g, std::min(front[j].g, front[mid].g));
                    knight.b = std::max(knight.b, std::min(front[j].b, front[mid].b));
                };
                const bool l2 = bx > 1, r2 = bx < block_cols - 2;
                const bool u2 = by > 1, d2 = by < block_rows - 2;
                if (l && u2) fold_knight(i - 2 * W - 1, i - W);
                if (r && u2) fold_knight(i - 2 * W + 1, i - W);
                if (l && d2) fold_knight(i + 2 * W - 1, i + W);
                if (r && d2) fold_knight(i + 2 * W + 1, i + W);
                if (l2 && u) fold_knight(i - W - 2, i - 1);
                if (r2 && u) fold_knight(i - W + 2, i + 1);
                if (l2 && d) fold_knight(i + W - 2, i - 1);
                if (r2 && d) fold_knight(i + W + 2, i + 1);

                const float k = transmit[i];
                const float kd = transmit_diag[i];
                const float kk = transmit_knight[i];
                const auto carry = [k, kd, kk](float e, float n, float nd, float nk) {
                    const float v = std::max(std::max(e, n * k),
                                             std::max(nd * kd, nk * kk));
                    return v < MIN_VISIBLE ? 0.0f : v;
                };
                const Rgb next{
                    carry(emission[i].r, best.r, diag.r, knight.r),
                    carry(emission[i].g, best.g, diag.g, knight.g),
                    carry(emission[i].b, best.b, diag.b, knight.b),
                };
                largest_change = std::max(largest_change, std::abs(next.r - front[i].r));
                largest_change = std::max(largest_change, std::abs(next.g - front[i].g));
                largest_change = std::max(largest_change, std::abs(next.b - front[i].b));
                back[i] = next;
            }
        }
        front.swap(back);
        if (largest_change < CONVERGED) break;
    }

    // --- smooth ---
    //
    // Rounds the chamfer metric's octagon off. See SMOOTH_PASSES. Separable, so
    // each pass is two sweeps of three taps rather than one of nine, and the taps
    // are weighted by how open the neighbour is so a wall stays a wall.
    //
    // Emission is re-imposed as a floor exactly as it is during propagation: a
    // source averaged against its dark neighbours would dim itself, and the one
    // block that must not move is the one the fire is in. Removing the floor here
    // does not round the shape off either -- by the time this pass runs the
    // emitting block has already been copied outward by every propagation
    // iteration, each of which re-imposes the same floor.
    {
        const auto openness = [this](size_t j) {
            return std::clamp((transmit[j] - TRANSMIT_SOLID) /
                                  (TRANSMIT_CLEAR - TRANSMIT_SOLID),
                              0.0f, 1.0f);
        };
        const auto sweep = [&](int step, bool horizontal) {
            for (int by = 0; by < block_rows; ++by) {
                for (int bx = 0; bx < block_cols; ++bx) {
                    const size_t i = static_cast<size_t>(by) * block_cols + bx;
                    const bool lo = horizontal ? bx > 0 : by > 0;
                    const bool hi = horizontal ? bx < block_cols - 1
                                              : by < block_rows - 1;

                    float weight = SMOOTH_CENTRE;
                    Rgb acc{front[i].r * SMOOTH_CENTRE, front[i].g * SMOOTH_CENTRE,
                            front[i].b * SMOOTH_CENTRE};
                    const auto tap = [&](size_t j) {
                        const float w = openness(j);
                        acc.r += front[j].r * w;
                        acc.g += front[j].g * w;
                        acc.b += front[j].b * w;
                        weight += w;
                    };
                    if (lo) tap(i - step);
                    if (hi) tap(i + step);

                    const auto settle = [&](float sum, float floor_value) {
                        const float v = std::max(sum / weight, floor_value);
                        return v < MIN_VISIBLE ? 0.0f : v;
                    };
                    back[i] = Rgb{
                        settle(acc.r, emission[i].r),
                        settle(acc.g, emission[i].g),
                        settle(acc.b, emission[i].b),
                    };
                }
            }
            front.swap(back);
        };
        for (int pass = 0; pass < SMOOTH_PASSES; ++pass) {
            sweep(1, true);
            sweep(block_cols, false);
        }
    }

    // --- pack ---
    for (size_t i = 0; i < count; ++i) {
        const auto channel = [](float v) -> uint32_t {
            return static_cast<uint32_t>(std::clamp(tone_map(v), 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        texels[i] = 0xFF000000u | (channel(front[i].r) << 16) |
                    (channel(front[i].g) << 8) | channel(front[i].b);
    }
}
