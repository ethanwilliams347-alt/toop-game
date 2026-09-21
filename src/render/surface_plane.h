#pragma once

#include <cstdint>
#include <vector>

// Makes the near ground read as the plane it stands on, instead of as a shelf
// in front of it.
//
// Measured down the frame at the spawn, luminance recedes cleanly from the
// horizon for four fifths of the window and then reverses hard at exactly the
// row the player stands on, because the last bands are almost entirely solid
// terrain while the plane behind that terrain carries on brightening. A shelf is
// what you get when the receding surface stops at the character.
//
// Reference frames measure the same thing from the other side: the character
// stands at 61-72% of the plane's depth with 28-39% of the plane in front of
// them. Ours is 0%, not because the camera is wrong -- geometrically the player
// already sits inside that family -- but because solid terrain occludes every
// row below the feet. That is a fact about composition, not about shading: no
// value, ramp or grade produces it.
//
// So this pass gives the terrain below the surface the plane's own value at its
// own depth, and the recession runs unbroken from the horizon to the bottom edge
// with the player standing in the middle of it rather than on top of it.
//
// --- five decisions, each of which was the other way first ------------------
//
// 1. A pixel pass and not a per-strip colour multiply. The cheap version is to
// draw the cell texture in horizontal strips with a per-strip
// SDL_SetTextureColorMod, which is machinery apply_grade already has. It cannot
// work: a grade is a multiply and a multiply can only darken, and the target
// here is brighter than the world at every row it touches. This is the first
// thing in the project that has needed to write pixels rather than scale them.
//
// 2. The tile's row-average colour, not the tile's marks. Painting the ground
// BMP's texture onto the terrain looks wrong in motion: the plane scrolls at its
// own parallax and the terrain scrolls at 1.0, so painted marks would slide
// across the very cells they are supposed to be lying on -- a second motion cue
// contradicting the first. Value is what carries recession; the marks are
// texture, and only the first survives being pinned to world coordinates. So
// each screen row takes the average of the tile row it would have sampled, and
// nothing horizontal is sampled at all.
//
// 3. Bounded by depth below the column's own surface, not by screen row. The
// plane's band covers the whole lower half of the window, and goes on covering
// it when the camera is deep underground, where every cell on screen is solid
// rock and none of it is a receding surface. The bound that means what it says
// is how far this cell is below the open sky in its own column.
//
// 4. Blended in over a skin, not switched on at the surface. The topmost cells
// of a column keep their material colour, because that is the line the player
// walks on and digs into, and sand has to stay distinguishable from stone at the
// one row where the distinction is acted on.
//
// 5. Applied to the cell buffer at upload, not as a new row in frame.cpp's layer
// table. A layer would need a mask -- "the plane, but only where terrain is" is
// not a rectangle -- and SDL2 has no cheap way to say that. Folded into the
// pixels instead, it inherits the cell texture's position in the stack for free:
// behind the player, in front of the backdrop, one SDL_RenderCopyF, and no new
// draw calls. The cost is the zero-copy upload.
//
// It links no SDL, for the same reason render/light.cpp does not: the part that
// is easy to get wrong does not need a window to check.

namespace surface_plane {

// --- the depth ramp, in cells below a column's exposed surface --------------
//
// TUNING.md carries these three as a row. The spawn frame is what they are set
// against: everything the tester is looking at is within the first few dozen
// cells of depth, so the fade never appears there.
//
// DEPTH_END must not try to do two jobs. Bounding what counts as near-surface
// ground and keeping the pass off the world when the camera is underground are
// different questions, and a DEPTH_END tight enough for the second drops the
// interior of every hill out of the pass, leaving dark patches in the band this
// exists to make continuous. The second job belongs to row_scale below, which
// states it geometrically, which leaves this one free to be generous.
//
// SKIN_CELLS is small for the opposite reason: a long ramp puts a visible notch
// directly under the feet, which is the shelf edge again at a smaller size. Four
// cells is still more than the player can dig in one swing, which is all the
// material identity at the walkable line was ever for.
constexpr int SKIN_CELLS = 4;  // 0 -> full over these, measured from the surface
constexpr int FULL_END   = 256;  // full strength through here
constexpr int DEPTH_END  = 320;  // and back to nothing by here

// Blend weight for a cell `d` rows below its column's exposed surface, 0-255.
//
// The bottom is a fade rather than an edge because a hard end would draw a
// horizontal line across the world at a fixed depth, visible the moment anyone
// digs a shaft. Nothing at the spawn reaches it; it exists for the descent.
inline int weight_at_depth(int d) {
    if (d < 0 || d >= DEPTH_END) return 0;
    if (d < SKIN_CELLS) return (d * 255) / SKIN_CELLS;
    if (d <= FULL_END) return 255;
    return ((DEPTH_END - d) * 255) / (DEPTH_END - FULL_END);
}

// The tile, reduced to one averaged colour per row, which is all decision 2
// leaves of it. `rgb` is `count` triples, top row first -- the tile's top row is
// the plane's far edge, the same layout backdrop_wrap::plane_src_at assumes.
struct TileRows {
    const uint8_t* rgb;
    int count;
};

// Reduce a loaded tile to one averaged colour per row. `out` is resized to h*3.
//
// Takes raw 0xAARRGGBB rather than a bmp::Image on purpose: scene/bmp.cpp is
// SCENE_PROP_SOURCES and this file is RENDER_SOURCES, and the two are kept apart
// by CMakeLists.txt. A pointer and two integers cost the caller one line and
// cost the source-set guard nothing.
void average_rows(const uint32_t* pixels, int w, int h, std::vector<uint8_t>& out);

// The visible rect, in cells.
struct View {
    int view_x, view_y;  // top-left, in grid cells
    int w, h;  // the padded viewport, in cells
};

// Fills `depth` (v.w * v.h) with each visible cell's distance in rows to the
// open air directly above it in its own column -- 0 for a cell with nothing on
// top of it, -1 for a cell that is not matter, and DEPTH_END for one whose air
// is at least that far up (which has weight 0 either way).
//
// Distance to the air above, not to the top of the column, and the two differ
// wherever there is a cave, a shelf or a trunk. Measuring from the topmost cell
// puts every cell in a column with an overhang or a tree past DEPTH_END, so a
// large share of the near band silently keeps its raw colour and the ladder
// comes out flat.
//
// One top-to-bottom pass with a running counter per column gets it exactly:
// matter increments the counter, air resets it. That is O(w * (h + DEPTH_END)),
// against the millions of reads a per-cell upward walk would cost.
//
// The pass starts DEPTH_END rows above the window rather than at row 0, and that
// bound is not an approximation: anything deeper has weight 0 whatever the exact
// number is. Cells in the seed row are treated as already that deep, which is
// the only honest answer when the air above them was never looked at.
//
// `run_scratch`, if given, is v.w ints the pass uses as its per-column running
// counter, so a per-frame caller allocates nothing. Its contents on entry do not
// matter -- it is filled with -1 first -- and on exit it holds the last visible
// row of `depth`. Pass nullptr and the buffer is allocated locally.
void depth_map(const uint32_t* grid_pixels, int grid_w, int grid_h,
               const View& v, int* depth, int* run_scratch = nullptr);

// Copy the visible rect out of the grid's pixel buffer, blending the near
// terrain toward the plane on the way.
//
// `src_row_for` maps a window row (0..h-1, in cells) to a tile row, or to -1 for
// rows that are not on the plane at all -- above the horizon, or well past the
// near edge. `row_scale` is that row's own 0-255 multiplier on the depth weight.
// The caller owns both because it owns the camera; here they are two tables of
// `h` entries, which is also what makes this testable without one.
//
// row_scale exists because "how deep is this cell" cannot answer "is the camera
// underground". Underground the horizon is off the top of the window, every row
// is past the plane's near edge, and the nearest tile row -- the brightest one --
// would be painted over solid rock in every direction. The geometric statement
// of that is `t`, the row's position along the plane from horizon to near edge:
// t <= 1 is on the plane, and beyond it the surface has ended. It fades rather
// than cuts for the same reason the depth ramp does.
//
// grade_* are the `ground` layer's grade from frame.cpp's table, applied here so
// the blended colour matches the plane as composited rather than as authored.
// The cell texture's own row is PLAIN, so nothing divides back out -- and the day
// that stops being true, this is the line that has to change.
//
// `out` is w*h and tightly packed. Empty cells are copied through untouched, so
// a dug hole still shows the backdrop.
//
// `scratch` is w*h ints for depth_map's output, owned by the caller so the
// per-frame path allocates nothing.
void apply(const uint32_t* grid_pixels, int grid_w, int grid_h,
           const View& v,
           const TileRows& rows, const int* src_row_for, const int* row_scale,
           int grade_r, int grade_g, int grade_b,
           int* scratch, uint32_t* out);

// The row multiplier for a row at position `t` along the plane, 0-255.
//
// Full on the plane, fading to nothing over the quarter-depth past its near
// edge. A caller with no camera can pass nullptr for `row_scale` and get 255.
constexpr int PLANE_FADE_END_T = 125;  // hundredths of the plane's depth
inline int row_scale_at(float t) {
    if (t < 0.0f) return 0;
    if (t <= 1.0f) return 255;
    const float end = static_cast<float>(PLANE_FADE_END_T) / 100.0f;
    if (t >= end) return 0;
    return static_cast<int>(255.0f * (end - t) / (end - 1.0f));
}

} // namespace surface_plane
