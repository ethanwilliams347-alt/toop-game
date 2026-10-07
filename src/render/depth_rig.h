#pragma once

#include <cmath>

// The perspective rig: parallax factors derived from a camera instead of typed
// into a table.
//
// Everything here is arithmetic and knows no SDL, for the reason backdrop_wrap.h
// gives: the draw calls live in render/frame.cpp, and the question of where a
// row lands does not need them, so a headless suite (rig_test) can hold it.
//
// --- what the bg1 family could not do, and why ------------------------------
//
// render/bg1_backdrop.h is careful and correct about the problem it had: nine
// painted layers that all draw pieces of ONE ground plane. Give them different
// vertical factors and the plane shears apart under the objects standing on it,
// so the vertical factor is locked at 1.00 and the vertical depth cue is given
// up ("If the cue is wanted later it cannot come from this column; it needs art
// whose layers do not share a plane"). The same shared plane forces the ground
// into three bands, cut on flat paint, because one factor per row would shear
// the painted shorelines.
//
// The rig gets both back by asking what camera the painting implies, and then
// being that camera.
//
// --- the pinhole relation ---------------------------------------------------
//
// A flat ground plane seen by a pinhole camera has a horizon row h where depth
// is infinite. A ground point at depth d appears e/d rows below the horizon,
// where e is the eye's height. Its parallax factor -- how far it moves on screen
// per unit the camera moves -- is 1/d. So for every row r of the plane:
//
//     factor(r) = (r - h) / (c - h)
//
// where c is the row at depth 1, the depth of the playable world (which is
// locked to the camera at 1.00 by definition). The factor is LINEAR in the row.
//
// Two consequences carry the whole design:
//
//   1. A thing standing on the plane at row r must scroll at factor(r), or its
//      feet slide on the ground under it. So a layer's factor is not a taste, it
//      is a fact about where the layer stands: rig_backdrop.h states a foot row
//      per layer and the factor is computed. rig_test reads each BMP and checks
//      the lowest painted row is that foot. Retune the camera (h, c) and every
//      factor in the stack moves together, consistently.
//
//      bg1 already obeys this, which is how the relation was checked: its
//      hills_midfar (0.30) stands at row 73 and hills_near (0.70) at row 89,
//      which solves to h = 61, c = 101 -- and bg1_08_ground's first opaque row
//      is 63. Ethan's factors were a pinhole camera all along.
//
//   2. The plane can scroll one row at a time, each at factor(r) -- line scroll,
//      what the SNES did per scanline with HDMA. There is no band boundary to
//      hide, so the plane can carry detail at every depth. A shoreline shears as
//      the camera passes, because a real shoreline does.
//
// --- the vertical axis ------------------------------------------------------
//
// The same relation holds vertically: a camera that rises moves a ground point
// at depth d up the screen by 1/d of the rise. Every plane row and every object
// at the same depth move by the same amount, so the feet stay glued vertically
// too, and the plane stretches about its horizon as the camera climbs -- which is
// what looking down on a valley from higher up is.
//
// Taken at full strength that stretch is large. bg_tarn's eye height is 64 rows
// and a 1080p camera can rise about 164 rows above the standing view, so flying
// to the ceiling magnifies the plane 1 + 164/64 = 3.6x vertically, and pixel art
// at 3.6x in one axis reads as smearing. So the vertical factor is the horizontal one pulled toward 1:
//
//     vfactor(f) = 1 - k * (1 - f)
//
// k = 1 is the honest camera; k = 0 is bg1's locked vertical. Any k keeps the
// glue, because the plane row and the object standing on it go through the same
// function of the same f. Only the amount of vertical depth cue changes. k is a
// feel constant and lives on the set, with TUNING.md.
//
// --- the anchor --------------------------------------------------------------
//
// bg1's layers are placed at -(cam * f): the painting is exact when the camera
// is at the world's top-left corner and nowhere else. Here a layer is placed at
//
//     -(anchor + f * (cam - anchor))
//
// so the painting is exact at the anchor -- chosen as the camera position of a
// player standing on the contact row -- and the parallax spreads out from there.
// That is where the player spends their time, so that is where the composition
// should be the composition.
//
// Coverage needs nothing from the anchor horizontally (every rig layer wraps)
// and vertically is the same convex-combination argument draw_backdrop_layer
// makes: for f in [0, 1] and both cam and anchor in [0, world - view], the art row
// at the window's top edge is anchor + f * (cam - anchor), which is in that same
// range, so a world-tall opaque layer always covers the window. rig_test pins it.
namespace depth_rig {

struct Rig {
    // Art rows, in the set's native pixels. horizon < contact.
    int horizon_row = 0;
    int contact_row = 1;

    // k above. 0..1.
    float vertical_strength = 1.0f;
};

// factor(r), unclamped. Rows below the contact row come out above 1.0 -- nearer
// than the world -- which is right for a foreground standing there, and what the
// plane clamps away (see plane_factor).
constexpr float factor_at(const Rig& rig, float row) {
    return (row - static_cast<float>(rig.horizon_row)) /
           static_cast<float>(rig.contact_row - rig.horizon_row);
}

// The plane's own rows, clamped to [0, 1]. Above the horizon there is no plane;
// below the contact row the plane is under the world's terrain and moving with
// it, so locking it at 1.00 keeps it seamless with the world where a dug hole
// shows it, rather than sliding away underneath at a factor above 1.
constexpr float plane_factor(const Rig& rig, float row) {
    const float f = factor_at(rig, row);
    return f < 0.0f ? 0.0f : (f > 1.0f ? 1.0f : f);
}

constexpr float vertical_factor(const Rig& rig, float f) {
    return 1.0f - rig.vertical_strength * (1.0f - f);
}

// Screen position of a layer's art origin on one axis, in pixels.
//
// cam and anchor are view positions in cells (Camera::view_fx/fy). At f = 1 this
// is -cam * scale, the world's own placement, whatever the anchor; at f = 0 it is
// -anchor * scale, fixed.
inline float origin(float cam, float anchor, float factor, int scale) {
    return -(anchor + factor * (cam - anchor)) * static_cast<float>(scale);
}

// The screen y of the top edge of plane row `row`, in pixels, unrounded.
//
// Each row edge is placed at its own vertical factor, so the plane stretches
// about the horizon as the camera rises. Both edges of a row are evaluated at
// edges, never at the row's centre, so row r's bottom and row r+1's top are one
// number and the strips meet.
inline float plane_edge_y(const Rig& rig, int row, float cam_y, float anchor_y, int scale) {
    const float g = vertical_factor(rig, plane_factor(rig, static_cast<float>(row)));
    return static_cast<float>(row * scale) + origin(cam_y, anchor_y, g, scale);
}

// The camera's view-y when a player of height `body_h` stands on the contact row,
// given how far down the viewport the camera holds them (Camera::VERTICAL_ANCHOR).
// Clamped into the camera's own range, because that is where the camera will be.
inline float standing_anchor_y(const Rig& rig, int body_h, int viewport_h,
                               float vertical_anchor, int world_h) {
    const float want = static_cast<float>(rig.contact_row) - static_cast<float>(body_h) * 0.5f -
                       static_cast<float>(viewport_h) * vertical_anchor;
    const float hi = static_cast<float>(world_h - viewport_h);
    if (hi <= 0.0f) return 0.0f;
    return want < 0.0f ? 0.0f : (want > hi ? hi : want);
}

// Sideways ripple for a water row, in cells. A sine travelling down the rows, so
// adjacent rows are out of phase and the surface shimmers rather than sways.
// Render-only: `time_s` is the wall clock and never reaches the simulation.
inline float ripple_cells(int row, float time_s, float amplitude) {
    return amplitude * std::sin(time_s * 2.2f + static_cast<float>(row) * 0.85f);
}

} // namespace depth_rig
