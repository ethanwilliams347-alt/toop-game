#pragma once

// The one place that knows how a world cell maps to a screen pixel, in both
// directions, and which part of the world is currently in view.
class Camera {
public:
    // The default, not a constant. A default-constructed Camera is the old one,
    // which is what keeps test_golden_frame's checksum meaningful rather than lucky.
    //
    // A scene may state its own. Art authored at one art pixel to one world cell
    // needs a scale large enough that the window is a view into the scene rather
    // than larger than all of it.
    //
    // src/physics/ must never learn this exists. The scale is a rendering property;
    // the simulation is in cells.
    static constexpr int DEFAULT_SCALE = 4;

    int scale() const { return scale_; }

    // Set once per scene activation, before the viewport is computed from it.
    // Refuses a non-positive value rather than propagating a division by zero into
    // every coordinate conversion in the project.
    void set_scale(int scale) { scale_ = scale > 0 ? scale : DEFAULT_SCALE; }

    // Centres the viewport on (center_x, center_y) -- normally the player --
    // clamped so it never scrolls past the world's edges. A world no bigger than the
    // viewport on an axis clamps to 0 on that axis.
    //
    // Takes a fractional centre. The world is larger than every viewport in
    // DISPLAY_MODES, so the view is unclamped wherever the player usually is, which
    // pins the player near screen centre and scrolls the world instead. Rounded to
    // whole cells, that world moves in scale-sized jerks at the simulation's
    // irregular sub-cell cadence while the eye tracks it smoothly, which reads as
    // ghosting and is not fixed by smoothing the player alone.
    //
    // The split: view_x() stays an integer cell because it indexes the pixel buffer
    // the texture is uploaded from, and there is no such thing as a fractional array
    // index. The leftover is handed to the renderer as frac_x() and paid out in
    // screen pixels when the texture is drawn.
    void follow(float center_x, float center_y, int viewport_w, int viewport_h, int world_w, int world_h) {
        follow_mode(center_x, center_y, viewport_w, viewport_h, world_w, world_h, false);
    }

    // The same framing, told which of the two world paradigms this scene is -- as a
    // bool rather than the enum, because src/game/ must not depend on the scene
    // loader to position a viewport. The caller reads SceneDef::is_infinite() and
    // hands over the answer.
    //
    // Only the horizontal clamp is dropped, never the vertical one. An infinite
    // scene is unbounded in travel, not in depth: the world still has a floor and a
    // ceiling, and the texture upload reads rows out of the grid, so a view that
    // scrolled off the top would be reading outside it. The horizontal case is
    // different only because the upload clamps its own source rect and the backdrop
    // tiles.
    //
    // follow keeps its old signature and delegates here with false, so every
    // existing caller frames identically.
    void follow_mode(float center_x, float center_y,
                     int viewport_w, int viewport_h,
                     int world_w, int world_h,
                     bool is_infinite) {
        const float desired_y = center_y - static_cast<float>(viewport_h) * VERTICAL_ANCHOR;
        if (is_infinite) {
            view_fx_ = center_x - static_cast<float>(viewport_w) / 2.0f;
        } else {
            view_fx_ = clamp_view(center_x - static_cast<float>(viewport_w) / 2.0f, viewport_w, world_w);
        }
        view_fy_ = clamp_view(desired_y, viewport_h, world_h);
    }

    // The vertical framing is not centred, and it is a constant rather than a knob.
    // The player sits VERTICAL_ANCHOR of the way down the viewport wherever the view
    // is unclamped.
    //
    // A fixed offset rather than a moving one. An anchor that eases between values
    // as the player digs was tried and rejected by playing twice. What a receding
    // ground plane needs is one number, not a behaviour: the plane cannot take more
    // than about half the band below a centred player, and the reference composition
    // this is authored against is nearer two thirds. Do not re-derive an easing from
    // this constant.
    //
    // It lives here and not at the caller because follow is called twice per
    // rendered frame -- once on the stepped position, once re-aimed at the
    // interpolated draw position -- and an anchor applied at one call site and not
    // the other tears the backdrop against the world every frame. And it is a
    // fraction of the viewport rather than a count of cells, because DISPLAY_MODES
    // has several viewport heights.
    //
    // The world's bottom clamp still wins: near the floor clamp_view refuses the
    // framing outright and the player rides up the screen. That is correct -- the
    // alternative is uploading from outside the grid -- but it means this constant
    // states an intent, not a guarantee. test_camera pins both.
    static constexpr float VERTICAL_ANCHOR = 0.80f;

    // Float in, float out: a world position drawn against a fractional view has a
    // fractional screen position, and rounding it here would put the jitter straight
    // back for anything drawn through it. Callers that need a pixel hand these to
    // SDL's float-rect calls.
    float world_to_screen_x(float world_x) const { return (world_x - view_fx_) * static_cast<float>(scale_); }
    float world_to_screen_y(float world_y) const { return (world_y - view_fy_) * static_cast<float>(scale_); }

    // Still integers, and deliberately: this direction answers which cell is under
    // the mouse, and there is no fractional answer to that question. Floored rather
    // than truncated, since a negative intermediate would otherwise round towards
    // zero and pick the cell on the wrong side.
    int screen_to_world_x(int screen_x) const { return floor_to_int(static_cast<float>(screen_x) / static_cast<float>(scale_) + view_fx_); }
    int screen_to_world_y(int screen_y) const { return floor_to_int(static_cast<float>(screen_y) / static_cast<float>(scale_) + view_fy_); }

    // Where a parallax layer's top-left corner goes, in screen pixels, for a layer
    // that moves at `factor` times the camera's speed. A factor of 1.0 gives a layer
    // locked to the world; 0.0 gives one pinned to the window.
    //
    // Here rather than at the draw site because it is a camera question: the offset
    // is a function of the view position and nothing else, and computing it inline
    // means reassembling three of this class's outputs at the caller.
    //
    // The continuous view is rebuilt as view_x() + frac_x() rather than read off
    // view_fx_ directly, so the expression is the one the composition used before
    // this was extracted.
    float parallax_origin_x(float factor) const {
        return -(static_cast<float>(view_x()) + frac_x()) * static_cast<float>(scale_) * factor;
    }
    float parallax_origin_y(float factor) const {
        return -(static_cast<float>(view_y()) + frac_y()) * static_cast<float>(scale_) * factor;
    }

    // A length, not a position, so it is never shifted by the viewport's offset,
    // only scaled. Also covers the on-screen size of one world cell, as
    // scale_length(1).
    int scale_length(int world_length) const { return world_length * scale_; }

    // World-cell coordinates of the viewport's top-left corner -- what the texture
    // upload in main.cpp reads the visible rect's pixels from. Floored, so the cell
    // named here is always the one the fractional view sits inside rather than the
    // nearest one, which is what makes frac_x() below non-negative and the render
    // offset a shift in one direction only.
    //
    // The continuous view position is what a parallax layer that has to be placed
    // rather than offset needs. Exposed rather than reassembled by the caller from
    // view_x() + frac_x(), for the reason parallax_origin_x gives.
    float view_fx() const { return view_fx_; }
    float view_fy() const { return view_fy_; }

    int view_x() const { return floor_to_int(view_fx_); }
    int view_y() const { return floor_to_int(view_fy_); }

    // How far into that cell the view actually is, 0 to just under 1. The renderer
    // draws the world texture shifted left/up by this much of a cell so the world
    // scrolls smoothly between whole-cell uploads. main.cpp uploads one extra cell
    // of margin to cover the sliver this exposes.
    float frac_x() const { return view_fx_ - static_cast<float>(view_x()); }
    float frac_y() const { return view_fy_ - static_cast<float>(view_y()); }

private:
    static int floor_to_int(float v) {
        const int truncated = static_cast<int>(v);
        return (v < 0.0f && static_cast<float>(truncated) != v) ? truncated - 1 : truncated;
    }

    static float clamp_view(float desired, int viewport_len, int world_len) {
        const float max_view = world_len > viewport_len ? static_cast<float>(world_len - viewport_len) : 0.0f;
        if (desired < 0.0f) return 0.0f;
        if (desired > max_view) return max_view;
        return desired;
    }

    int scale_ = DEFAULT_SCALE;
    float view_fx_ = 0.0f;
    float view_fy_ = 0.0f;
};
