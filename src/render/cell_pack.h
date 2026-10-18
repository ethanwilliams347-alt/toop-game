#pragma once
#include <bit>
#include <cstdint>
#include <span>
#include <vector>
#include "physics/element.h"
#include "physics/grid.h"
#include "physics/material.h"
#include "render/hidden_cell.h"

// The visible window of cells as per-cell material data, one 32-bit texel per
// cell, for a cell shader that draws from what a cell *is* rather than from its
// colour. The colour texture (Grid::get_pixels) stays as it is and keeps drawing
// the SDL path; this is the second texture beside it.
//
// Why a pack and not an upload of the Element array itself: Element is 12 bytes
// and most of it is simulation bookkeeping (colour, the visit tag, the piece
// tag) a shader has no use for. Four bytes is a quarter of the upload, one
// RGBA8UI texel, and a layout this file owns, so the simulation can rearrange
// its struct without a shader noticing.
//
// SDL-free and GL-free like the rest of slop_render: what this produces is a
// plain buffer, and handing it to the GPU is the shell's job. Reads the grid
// const and writes only its own buffer -- rendering never feeds simulation.
//
// Element::color is deliberately not read. It is simulation state (hashed into
// recorded sessions), and every look a shader gives a material is built from
// the type and the numbers below instead.
//
// ---------------------------------------------------------------------------
// Texel layout. A shader mirrors this exactly; change both or neither.
//
// One uint32_t per cell, row-major, width() wide, top row first. Stored so that
// its bytes in memory are R, G, B, A in that order (R is the low byte on a
// little-endian machine, asserted below), which is what GL_RGBA8UI uploaded as
// GL_RGBA_INTEGER / GL_UNSIGNED_BYTE expects; a usampler2D reads it back as
// uvec4(r, g, b, a).
//
//   R  ElementType, as its enum value (Empty = 0).
//   G  A liquid (is_liquid: water, oil): how much it holds,
//        min(mass >> 4, 255)
//      so a full cell (Grid::LIQUID_FULL = 4096) reads 255 -- 256 saturates --
//      and the pressed excess a sinking grain leaves for a few steps saturates
//      with it. Anything else: Element::ticks verbatim -- a structural cell's
//      free-fall clock, a Fire or Steam cell's steps left, 0 for the rest.
//      Steam carries mass too, but its lifetime is the number worth drawing (a
//      puff thins as it runs out), so it takes the ticks branch.
//   B  Element::temperature verbatim (AMBIENT_TEMPERATURE = 20 at rest).
//   A  Flags, bit 0 the lowest:
//        bit 0     HIDDEN    a loose body's cell, drawn by its sprite; R/G are
//                            the fill shown around the sprite, not the body
//        bit 1     OCCLUDER  OCCLUDES[R]: the cell is a solid or a powder
//        bit 2     FLOW_LEFT Element::flow < 0 (the liquid moves left)
//        bits 3-7  FLOW_MAG  min(|Element::flow| >> 2, 31)
//      The signed flow a shader recovers is (FLOW_LEFT ? -1 : 1) * FLOW_MAG, in
//      units of 4 * Grid::LIQUID_FLOW_SCALE = 64 mass a step (1/64 of a cell a
//      step), so 31 is just under half a cell a step, which is the byte's own
//      ceiling. Zero for everything that is not a liquid, as flow is.
//
// GLSL, for reference:
//   uvec4 c = texelFetch(cells, p, 0);
//   bool hidden = (c.a & 1u) != 0u, occluder = (c.a & 2u) != 0u;
//   int flow = int(c.a >> 3) * (((c.a & 4u) != 0u) ? -1 : 1);
//
// Cells outside the grid (a window hanging past an edge) are PAD_TEXEL: an
// Empty cell as Element{} holds it -- R = 0, G = 0, B = 20, A = 0. Not Wall,
// which is what Grid::get_element answers off the edge: that is a collision
// rule, and drawn, it would put a wall of rock past the edge of every scene.
//
// A hidden cell (frame::HiddenCell) packs as its `fill_type` with HIDDEN set:
// G is 255 if that fill is a liquid (a neighbour's fill reads as a full cell of
// it, as its colour does on the SDL path) and 0 otherwise, B is the hidden
// cell's own temperature (the body is still there, and still as hot), the
// OCCLUDER bit is the fill's, and there is no flow. A light pass that wants the
// body to keep casting its shadow tests HIDDEN as well as OCCLUDER.
// ---------------------------------------------------------------------------
namespace cell_pack {

static_assert(std::endian::native == std::endian::little,
              "the texel's byte order is written for a little-endian machine");

inline constexpr uint32_t FLAG_HIDDEN = 1u << 0;
inline constexpr uint32_t FLAG_OCCLUDER = 1u << 1;
inline constexpr uint32_t FLAG_FLOW_LEFT = 1u << 2;
inline constexpr int FLOW_MAG_SHIFT = 3;  // bits 3-7 of A
inline constexpr int FLOW_MAG_DROP = 2;   // |flow| >> 2
inline constexpr uint32_t FLOW_MAG_MAX = 31;
inline constexpr int MASS_DROP = 4;  // mass >> 4

// Which materials stop light and take a rim: what holds its shape. Today this
// is exactly is_solid (Static or Powder, not Empty), written out as a table
// rather than derived because a shader mirrors it as one, and because the two
// may part -- a translucent solid like ice would be solid and not an occluder.
// Indexed by ElementType; the size assert makes a new material choose.
// clang-format off
inline constexpr bool OCCLUDES[] = {
    false, // Empty
    true,  // Sand
    false, // Water
    true,  // Wall
    true,  // Wood
    false, // Oil
    false, // Steam
    false, // Fire
    true,  // Charred
};
// clang-format on
static_assert(sizeof(OCCLUDES) / sizeof(OCCLUDES[0]) == static_cast<size_t>(ElementType::Count),
              "OCCLUDES needs exactly one entry per ElementType");

inline constexpr bool occludes(ElementType t) { return OCCLUDES[static_cast<size_t>(t)]; }

inline constexpr uint32_t texel(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

// The flag byte's flow bits for one signed flow value.
inline constexpr uint32_t flow_bits(int flow) {
    const uint32_t mag = static_cast<uint32_t>(flow < 0 ? -flow : flow) >> FLOW_MAG_DROP;
    return (flow < 0 ? FLAG_FLOW_LEFT : 0u) |
           ((mag < FLOW_MAG_MAX ? mag : FLOW_MAG_MAX) << FLOW_MAG_SHIFT);
}

// One cell as it is in the grid.
inline constexpr uint32_t texel_of(const Element& e) {
    const uint32_t amount = static_cast<uint32_t>(e.mass) >> MASS_DROP;
    const uint32_t g = is_liquid(e.type) ? (amount < 255u ? amount : 255u) : e.ticks;
    const uint32_t a = (occludes(e.type) ? FLAG_OCCLUDER : 0u) | flow_bits(e.flow);
    return texel(static_cast<uint32_t>(e.type), g, e.temperature, a);
}

// One hidden cell: its fill, flagged. See the layout notes above.
inline constexpr uint32_t hidden_texel(ElementType fill, uint8_t temperature) {
    return texel(static_cast<uint32_t>(fill), is_liquid(fill) ? 255u : 0u, temperature,
                 FLAG_HIDDEN | (occludes(fill) ? FLAG_OCCLUDER : 0u));
}

inline constexpr uint32_t PAD_TEXEL = texel_of(Element{});

// The buffer, kept between frames. One per renderer.
//
// No allocation once warm: the buffer is resized to w*h on each pack, which
// allocates only when the window grows past the largest it has been -- a display
// mode change, not a frame. A window that shrinks keeps its capacity.
class CellPack {
public:
    // Packs the window whose top-left cell is (x0, y0), w by h cells, from the
    // grid. The window may hang off any edge (or lie wholly outside); cells off
    // the grid are PAD_TEXEL. `hidden` is this frame's present::paint_bodies
    // output; entries outside the window are skipped. w or h <= 0 packs nothing.
    void pack(const Grid& grid, int x0, int y0, int w, int h,
              std::span<const frame::HiddenCell> hidden = {});

    // The same over a bare row-major cell array grid_w by grid_h -- what the
    // overload above calls with Grid::cell_data(), and what a test calls with
    // cells it has written field by field, which Grid has no setter for.
    void pack(const Element* cells, int grid_w, int grid_h, int x0, int y0, int w, int h,
              std::span<const frame::HiddenCell> hidden = {});

    // width() * height() texels, row-major. See the layout notes above.
    const std::vector<uint32_t>& texels() const { return buf; }
    int width() const { return buf_w; }
    int height() const { return buf_h; }

private:
    std::vector<uint32_t> buf;
    int buf_w = 0, buf_h = 0;
};

} // namespace cell_pack
