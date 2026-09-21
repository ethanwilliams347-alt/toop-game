#pragma once
#include <cstdint>
#include <vector>
#include "physics/grid.h"

// Per-cell emissive lighting: a downsampled light grid, propagated over a
// handful of iterations, uploaded as a small texture and stretched over the
// world with linear filtering. One extra SDL_RenderCopy, no shader path, no new
// dependency, and a resolution knob (BLOCK) to trade quality against cost.
//
// This lives in src/render/ and not in src/physics/ because rendering must not
// feed the simulation: two players on the same seed and the same input log must
// not diverge because of what was on screen. The dependency runs one way only --
// this file includes grid.h, and nothing under src/physics/ may ever include
// this one. Everything here reads the grid const and writes only its own
// buffers, which is what makes that rule checkable by reading a single #include.
//
// Deliberately SDL-free so it can be tested headlessly like the simulation is,
// even though it is rendering code. The texture upload is main.cpp's job; what
// this produces is a plain ARGB buffer.
class LightField {
public:
    // Cells per light block on each axis. Small enough that a single flame cell
    // lands in a block of its own rather than being averaged into a wall, and large
    // enough that the propagation below is a rounding error against the physics
    // step.
    //
    // Raising it is the obvious way to pay for a longer reach or a larger viewport,
    // and it is the wrong trade: occlusion is averaged over a block, so a one-cell
    // wall reads as 1/BLOCK opaque, and light leaking through a player-drawn
    // one-cell wall is exactly that number being too small already. Doubling BLOCK
    // halves it again. Reach is bought below instead, where it costs iterations
    // rather than the sharpness of a wall.
    static constexpr int BLOCK = 4;

    // How far light carries, in propagation steps. Each step moves light one block --
    // BLOCK cells -- so this is a ceiling of ITERATIONS*BLOCK cells. The true reach
    // is shorter and is set by TRANSMIT_CLEAR compounding per block, which light.cpp
    // gives as BLOCK * ln(255*MAX_EMISSION) / -ln(TRANSMIT_CLEAR). This bound only
    // has to sit above that, or the iterations become the reach and the tuning stops
    // meaning anything.
    //
    // The attenuation-limited reach is tuned as a multiple of the body's height --
    // light carries about four body-heights -- so this has to move with the body
    // size rather than stay at a fixed cell count.
    static constexpr int ITERATIONS = 24;

    // Below this a cell contributes nothing. It sits under the coldest ignition
    // point and above anything a world that is merely warm will reach: a cell this
    // hot has either caught or is about to, and both should glow. Without a floor
    // every conducted degree would be a light source and the whole scene would haze.
    static constexpr uint8_t GLOW_THRESHOLD = 100;

    // Sized in cells; the block dimensions are derived and rounded up, so the last
    // block may hang off the edge of the region. That is intentional and is what
    // keeps the stretch aligned -- see pixels().
    LightField(int region_cells_w, int region_cells_h);

    // Recomputes the whole field from the grid region whose top-left cell is
    // (origin_x, origin_y). Reads the grid and mutates nothing outside this object.
    void update(const Grid& grid, int origin_x, int origin_y);

    // ARGB8888, one texel per block, row-major, cols() wide. Alpha is 255 throughout
    // and carries no meaning: this texture is composited additively, so black is "no
    // light" and the alpha channel is never read.
    //
    // The texture covers cols()*BLOCK by rows()*BLOCK cells, not the region it was
    // asked for, and the caller must stretch it over exactly that larger area. That
    // is what puts each texel's centre on the centre of the cells it was computed
    // from, which is the whole of getting linear filtering to land where the light
    // actually is instead of half a block off it.
    const std::vector<uint32_t>& pixels() const { return texels; }

    int cols() const { return block_cols; }
    int rows() const { return block_rows; }

    // Whether anything at all is emitting. Zero light is the common case -- a world
    // nobody has set fire to -- and the caller skips the upload and the draw
    // entirely when this is false, so the cost of the feature in an unlit scene is
    // the scan and nothing else.
    bool any_light() const { return lit; }

private:
    int block_cols;
    int block_rows;

    // Floats rather than bytes because many multiplications by an attenuation under
    // 1 quantise to nothing in 8 bits: the far half of every falloff would be a hard
    // edge at the point the integer hit zero.
    struct Rgb { float r = 0.0f, g = 0.0f, b = 0.0f; };

    // What the hot cells themselves put out, kept separate from the propagated
    // result because every iteration re-imposes it as a floor. A source that could
    // be dimmed by its own falloff would fade out over the iterations instead of
    // settling.
    std::vector<Rgb> emission;
    std::vector<Rgb> front;
    std::vector<Rgb> back;

    // How much light survives crossing each block, 0-1. Derived from how much of the
    // block is solid, so terrain shadows itself without anything tracing a ray.
    //
    // Three of them, because the step lengths differ. Propagating to four neighbours
    // only makes distance Manhattan rather than Euclidean, and a glow whose falloff
    // is measured in city blocks is a diamond -- on screen, vertical and horizontal
    // shafts radiating from every fire. Adding the diagonals fixes the shape only if
    // they cost more to cross; at equal cost the artefact rotates 45 degrees and
    // becomes a square.
    //
    // Orthogonal and diagonal steps alone still only measure distance to about 8%,
    // and the error is worst at 22.5 degrees -- halfway between the two directions
    // the steps point in, which reads as an octagon bulging at eight points. Adding
    // (1,2) steps at their own cost interleaves eight more directions exactly where
    // the error was, and takes it under 2%.
    std::vector<float> transmit;
    std::vector<float> transmit_diag;
    std::vector<float> transmit_knight;

    std::vector<uint32_t> texels;
    bool lit = false;
};
