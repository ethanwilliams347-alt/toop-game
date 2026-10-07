#pragma once

#include <cmath>

// Wrapping backdrop layers.
//
// A non-wrapping backdrop layer must be as wide as the window plus the whole pan
// range at its own parallax factor, so the nearer the layer, the bigger its
// file -- and a factor above 1 has no finite size at all. A wrapping layer has
// no size relationship to the pan range: it is one tile, drawn as many times as
// the window needs. Every backdrop layer is drawn through wrap_axis
// (render/frame.cpp); a corner-anchored set at f <= 1 simply never needs its
// second copy.
//
// This header is arithmetic and nothing else. The draw call that uses it lives
// in render/frame.cpp and needs SDL; the question of where the copies go does
// not, so it is here, where a headless suite can reach it -- the same
// arrangement render/player_anim.cpp has with the sprite sheet.
//
// Nothing in src/physics/ may include this. It is renderer-side by the same rule
// the light field is; the ENGINE_SOURCES / RENDER_SOURCES split in
// CMakeLists.txt is what makes that a build error rather than a habit.

namespace backdrop_wrap {

// --- one authored layer cut into bands ------------------------------
//
// A receding surface has no single depth, so it has no single parallax factor.
// The rig's plane ramps its factor smoothly, one per row, which is right for art
// painted to shear (a lake). Painted art with features that must not tear has
// its factor change where the art does not: across a run of rows that is
// uniform in colour, where a discontinuity in scroll offset has nothing to
// reveal.
//
// row0/row1 are rows of the layer's texture, half-open, top-down. Vertical is
// always 1:1 -- the perspective is already in the paint, and the authored
// stack's vertical factor is locked so the composition is the painting at every
// camera height.
//
// Here rather than in frame.h because it is arithmetic about art and knows no
// SDL, which is what lets a headless suite check a band table against the BMP it
// describes.
struct Band {
    int row0 = 0, row1 = 0;
    float parallax_x = 1.0f;
};


// Where the first copy of a tile goes, and how many copies cover the window.
//
// `first` is always in (-tile, 0]: the leftmost copy starts at or before the
// window's edge, never after it, so there is no uncovered strip on that side.
struct Tiling {
    float first;
    int count;
};

// One axis of it. `origin` is the layer's unwrapped parallax origin -- what
// depth_rig::origin returns, which runs negative as the camera moves
// forward and is unbounded in both directions over a long session.
//
// The whole job is turning that unbounded number into a bounded one without ever
// letting the visible result depend on how far the camera has travelled.
// std::fmod keeps the sign of its left operand, so a negative origin gives a
// result in (-tile, 0] directly and a positive one needs a single subtraction to
// land in the same half-open interval. Both branches must produce a `first` that
// is <= 0, because a slightly positive `first` leaves a one-pixel column of
// whatever was behind the layer showing at the window edge, on one frame, at one
// camera position.
//
// The interval is half-open at 0 and not at -tile on purpose: first == 0 is a
// legitimate aligned state and must not be pushed to -tile, which would draw an
// entire redundant copy off-screen on every frame the layer happens to align.
inline Tiling wrap_axis(float origin, int tile, int window) {
    if (tile <= 0) return Tiling{0.0f, 0};  // a zero-width tile tiles nothing

    const float t = static_cast<float>(tile);
    float first = std::fmod(origin, t);
    if (first > 0.0f) first -= t;

    // Copies needed to reach the far edge. `first` is <= 0, so (window - first) is
    // the full span that has to be covered measured from the first copy's own left
    // edge, and the ceiling of that over the tile width is the count. An integer
    // ceiling rather than std::ceil, so a span that divides exactly does not gain a
    // copy from a float that landed one ulp high.
    const int span = static_cast<int>(window - first) + 1;
    const int count = (span + tile - 1) / tile;
    return Tiling{first, count};
}

} // namespace backdrop_wrap
