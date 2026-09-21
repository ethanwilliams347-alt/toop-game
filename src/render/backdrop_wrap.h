#pragma once

#include <cmath>

// Wrapping backdrop layers.
//
// A non-wrapping backdrop layer must be as wide as the window plus the whole pan
// range at its own parallax factor -- see layer_size() in
// tools/generate_backdrop.py -- so the nearer the band, the bigger its file. A
// ground plane sits nearer than the sky or the mountains, and priced flat a set
// of near bands costs several times the whole of assets/ for art that is, by
// construction, one repeating texture. A wrapping layer has no size relationship
// to the pan range at all: it is one tile, drawn as many times as the window
// needs.
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
// A generated plane is a repeating noise tile with no feature wide enough to
// shear, so it can ramp its factor smoothly, one strip per row-range. Painted
// art has features, so its factor has to change where the art does not: across a
// run of rows that is uniform in colour, where a discontinuity in scroll offset
// has nothing to reveal.
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
// Camera::parallax_origin_x/y returns, which runs negative as the camera moves
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

// --- the ground plane: one band drawn as N strips ------------------------
//
// A receding plane has no single depth, so it has no single parallax factor.
// Drawn flat at one factor it reads as a wall standing behind the world rather
// than as ground going away. The plane is cut into horizontal strips, each strip
// is a depth, and each therefore scrolls at its own rate and samples its own
// rows of the tile.
//
// One relation rather than two. For a plane seen edge-on, screen distance below
// the horizon goes as inverse world distance, so the scroll factor is linear in
// that distance:
//
//     f(t) = f_far + (f_near - f_far) * t,   t = 0 at the horizon, 1 at the near edge
//
// and world distance is d(t) = 1/f(t). The texture is laid out in world distance
// -- its top row is the far edge and its bottom row the near one -- so the source
// row for a strip falls out of the same d(t) with no second curve to author and
// nothing to keep in step:
//
//     v(t) = (d(t) - d_near) / (d_far - d_near),  src_y = (1 - v) * tile_h
//
// This is the texture-gradient mechanism, and it comes out of the geometry
// rather than out of the art: marks near the viewer are magnified and marks near
// the horizon are compressed, because a far strip spans a huge range of world
// distance in a handful of screen rows. Note that src_h therefore grows with
// distance rather than shrinking.
//
// Only the vertical is scaled, and the horizontal tile width is constant across
// every strip. A true plane shrinks a mark in both axes, so this is a stated
// cheapness. Scaling the tile's width per strip costs nothing arithmetically and
// looks worse: adjacent strips would tile at different widths, their phases
// would diverge, and the mark pattern would stair-step at every strip boundary --
// a visible seam per strip bought to fix a foreshortening nobody can see on a
// night-dark texture.
struct Plane {
    float horizon_y;  // screen y of the far edge
    float bottom_y;  // screen y of the near edge - normally the window's bottom
    float far_factor;  // parallax at the horizon
    float near_factor;  // parallax at the near edge
    int tile_h;  // the tile's height in pixels; its rows are the plane's depth
};

// How many screen pixels one row of the tile occupies, and therefore how deep
// the plane is. The plane's whole geometry is this one number plus wherever the
// horizon landed.
//
// This is what replaces a near edge nailed to the bottom of the window, and the
// window is deliberately not a parameter of this function. A constant near edge
// against a parallaxed far edge gives the near end an effective vertical
// parallax of zero and the far end a non-zero one -- the nearer end of one layer
// less parallaxed than its further end, which is the one thing a parallax stack
// may never do. The visible result is a tile that never leaves the frame at any
// camera height and squeezes as the camera climbs.
//
// The plane now translates and clips, the way every other layer does. The
// alternative was to give the near edge its own vertical factor and let the band
// grow as the camera climbs, which is honest perspective for a surface seen from
// higher up and magnifies the tile's marks substantially over the world's
// height. A constant scale was chosen: the complaint is that the layer resizes,
// and the depth-honest fix resizes it too, just for a better reason.
//
// Stated per texel and not as a fraction of the window, because that is what
// makes it constant across the three DISPLAY_MODES -- a fraction of the window
// would put the same tile on screen at two sizes at 1080 and 1440.
inline constexpr float PLANE_TEXEL_SCALE = 2.5f;

// Where the plane's two edges go, given where its far edge landed.
//
// The far edge comes from the art (the mountains' skyline row, at the mountains'
// vertical factor -- see ground_horizon_y in render/frame.cpp); the near edge is
// the far edge plus the plane's own depth. Nothing here knows the window size,
// which is the point: a layer's geometry is a fact about the layer, and the
// window only decides how much of it you can see.
//
// The near edge is free to land off the bottom of the window -- it usually does
// -- and free to land above it when the camera is low, which is what the caller's
// fill below the plane is for.
inline Plane plane_geometry(float horizon_y, int tile_h,
                            float far_factor, float near_factor) {
    return Plane{horizon_y,
                 horizon_y + static_cast<float>(tile_h) * PLANE_TEXEL_SCALE,
                 far_factor, near_factor, tile_h};
}

// One strip: where it lands, what it samples, and how fast it scrolls.
struct Strip {
    float factor;  // this strip's parallax factor, for wrap_axis
    float dst_y, dst_h;  // destination, in screen pixels
    float src_y, src_h;  // source, in tile rows
};

// Strip `i` of `n`, counted from the horizon down.
//
// The factor is taken at the strip's midpoint rather than at its top edge: a
// strip is one depth standing in for a range of them, and its middle is the
// depth it is least wrong about at both of its own edges. Taking the top edge
// biases every strip toward the horizon and leaves the near edge of the plane
// scrolling slower than near_factor says it does.
//
// The source rows come from the strip's actual edges and not from its midpoint,
// because those have to meet: strip i's bottom source row is strip i+1's top
// source row, or the texture repeats or skips a row at every boundary.
//
// v(t), written out. d_far and d_near are the reciprocals of the two factors;
// the subtraction below is (1/f - 1/f_near) / (1/f_far - 1/f_near), which is v
// with both reciprocals left in place rather than expanded -- this runs n times
// a frame, not per pixel.
//
// A free function rather than a lambda inside plane_strip, because
// plane_src_row below has to evaluate it at exactly the same t values. That is
// the whole of how adjacent strips are made to meet in integer rows.
inline float plane_src_at(const Plane& p, float t) {
    const float th = static_cast<float>(p.tile_h);
    const float d_far = 1.0f / p.far_factor;
    const float d_near = 1.0f / p.near_factor;
    const float span = d_far - d_near;
    if (span <= 0.0f) return t * th;  // degenerate: far and near are one depth
    const float d = 1.0f / (p.far_factor + (p.near_factor - p.far_factor) * t);
    const float v = (d - d_near) / span;
    return (1.0f - v) * th;
}

inline Strip plane_strip(const Plane& p, int i, int n) {
    if (n <= 0 || p.tile_h <= 0 || p.far_factor <= 0.0f || p.near_factor <= 0.0f)
        return Strip{0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

    const float t0 = static_cast<float>(i) / static_cast<float>(n);
    const float t1 = static_cast<float>(i + 1) / static_cast<float>(n);
    const float band = p.bottom_y - p.horizon_y;

    const float f_mid = p.far_factor +
                        (p.near_factor - p.far_factor) * 0.5f * (t0 + t1);

    const float s0 = plane_src_at(p, t0);
    const float s1 = plane_src_at(p, t1);

    return Strip{f_mid,
                 p.horizon_y + band * t0, band * (t1 - t0),
                 s0, s1 - s0};
}

// The integer source row of boundary `i` of `n`, for i in [0, n].
//
// SDL_Rect is integer, so the float rows above have to be rounded somewhere.
// Rounding each strip's start and its height independently means strip i's last
// sampled row and strip i+1's first are unrelated numbers -- the texture repeats
// a row at some boundaries and skips one at others, which shows as black bands
// between the strips.
//
// Rounding the boundaries instead makes them meet by construction: strip i ends
// at boundary i+1 and strip i+1 begins at boundary i+1, which is one number
// evaluated once.
//
// That is only half of the banding. The other half is the art: a tile dithered
// evenly between two tones, point-sampled at up to ten source rows per screen
// row near the horizon, leaves which rows a strip lands on deciding whether it
// comes out dark. That half is fixed in generate_backdrop.py's banded_ramp, and
// no arithmetic here could have.
inline int plane_src_row(const Plane& p, int i, int n) {
    if (n <= 0 || p.tile_h <= 0 || p.far_factor <= 0.0f || p.near_factor <= 0.0f)
        return 0;
    const float t = static_cast<float>(i) / static_cast<float>(n);
    const float row = plane_src_at(p, t);
    const int r = static_cast<int>(row + 0.5f);
    if (r < 0) return 0;
    if (r > p.tile_h) return p.tile_h;
    return r;
}

} // namespace backdrop_wrap
