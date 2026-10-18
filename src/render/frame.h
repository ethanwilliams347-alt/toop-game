#pragma once
#include <vector>
#include "game/camera.h"
#include "render/backdrop_wrap.h"
#include "render/canvas.h"
#include "render/depth_rig.h"
#include "render/hidden_cell.h"
#include "render/light.h"

// The world layers of one frame, in the order they are drawn.
//
// UI is not here and does not become here. The reticle, the HUD, the hotbar, the
// run-over wash and the settings menu live in render/overlay.cpp and are drawn
// by a separate call after this one returns. The boundary is the one the light
// pass draws: everything in this file is in the world and gets lit, everything
// after it is not and must not be. A reticle that goes orange near a flame is
// the failure this prevents.
namespace frame {

// A non-simulated sprite anchored to a world position. Exercises no system and
// is never dug, ignited or displaced. Drawn before the cell texture rather than
// after, which is what gives a planted trunk its occlusion for free.
struct Prop {
    gfx::Texture texture;
    int w, h;  // native size, in world cells (1 BMP pixel = 1 cell)
    float anchor_x;  // world cell, bottom-centre of the sprite
    float anchor_y;
};

// One enemy, as the frame draws it. The body is not a sheet cell: it is whatever
// is left of it, in whatever pose it is in, painted into a slot of
// `Params::enemy_atlas` by the caller from the simulation's own answer to "which
// pixel is in this cell" (Enemy::posed_pixel), so what is drawn is exactly what
// an arrow can still hit. `src` is the rectangle of the slot the pose covers.
//
// Position is the collision box's top-left, interpolated, in world cells -- the
// same convention as the player -- and `offset_x`/`offset_y` are how far up and
// left of it `src`'s top-left corner is drawn, already allowing for the facing:
// a pose reaches outside the art's frame (a club raised overhead), so the anchor
// is the rectangle's, which the caller works out from the species' own offset
// and the pose's bounds. The size is `src`'s.
struct EnemySprite {
    gfx::Rect src;
    float x, y;
    int offset_x, offset_y;
    bool facing_left;
};

// One loose body -- a piece of structure toppling, tumbling, sliding or
// floating -- drawn as the one solid thing it is. `src` is its picture in the
// body atlas, unturned; (x, y) is where its centre of mass is in world cells,
// already interpolated; (pivot_x, pivot_y) is where that centre sits in `src`,
// in cells from its top-left corner; it is drawn turned by `angle_deg` about
// that point, clockwise. Its cells are left out of the cell texture while it
// moves, so the grid does not draw it a second time, a cell at a time.
struct BodySprite {
    gfx::Rect src;
    float x, y;
    float pivot_x, pivot_y;
    float angle_deg;
};

// HiddenCell (a loose body's cell and what shows there instead) is in
// render/hidden_cell.h, so SDL-free code can read it.

// One drop of spray (Grid::Spray): where it is in world cells, already
// interpolated, and its colour, alpha included. Drawn as a cell-sized square.
struct SprayDrop {
    float x, y;
    uint32_t color;
};

// One arrow: where its tip is, in world cells, and which way it points as a
// unit vector. A direction of (0, 0) -- an arrow at rest -- is drawn pointing
// down, which is how a dropped arrow lies.
struct ArrowSprite {
    float tip_x, tip_y;
    float dir_x, dir_y;
};

// One layer of a backdrop, as the frame draws it: a texture plus how it moves.
// Built by the caller from a backdrop_set::Layer (render/backdrop_set.h), which
// is where every field below is argued; this is that record with the texture
// attached and the factors resolved.
//
// w/h are the layer's size on screen, not its texture's size, and the two differ
// on purpose. The texture is the art at its native size and is stretched by the
// scene's integer scale at draw time, so it stays small in VRAM; the scaling is
// nearest-neighbour, so an integer factor is exact. tex_h is the texture's own
// height, which is what turns an art row into screen rows.
using Band = backdrop_wrap::Band;

struct ParallaxLayer {
    gfx::Texture texture;
    int w = 0, h = 0;
    int tex_h = 0;
    float parallax_x = 1.0f;
    float parallax_y = 1.0f;
    bool is_foreground = false;  // drawn in front of the player and the cells

    // A painted surface scrolled as horizontal bands, each at its own factor;
    // parallax_x is unused when set.
    std::vector<Band> bands;
    // The ground plane: one texture row at a time, each at the rig's factor for
    // that row on both axes. parallax_x/y are unused when set.
    bool line_scroll = false;
    // Placed vertically by the plane's rows but scrolled at parallax_x: paint lying
    // on the plane, like the sun's reflection. Drawn per row over the ripple range.
    bool on_plane = false;
    // Rows [ripple_row0, ripple_row1) of the texture shimmer sideways with time.
    int ripple_row0 = 0, ripple_row1 = 0;
    // Cells per second of sideways motion independent of the camera (clouds).
    float drift = 0.0f;
};

// The backdrop of the loaded scene. Empty `layers` draws nothing, and the clear
// colour shows behind the cells -- the `empty` scene.
//
// Drawn in vector order, so the vector is the depth ordering: index 0 is
// furthest back. is_foreground is the one exception and it is not a reordering --
// it moves a layer past the player into the second of the two passes.
//
// Every layer is placed about the anchor, -(anchor + f * (cam - anchor)), on
// both axes (render/depth_rig.h). The caller sets the anchor per frame, since it
// depends on the viewport: (0, 0) for a set anchored at the world's corner, the
// standing camera for one anchored there.
struct Backdrop {
    std::vector<ParallaxLayer> layers;
    depth_rig::Rig rig{0, 1, 0.0f};
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
    float ripple_amplitude = 0.0f;  // cells
};


// Everything one composed frame reads, and nothing else.
//
// A struct rather than a very long argument list, and every field is something
// the caller already had. There is no state here, so a second caller -- the
// golden frame test -- can build one without a window, a Run or a session.
struct Params {
    const Camera* camera = nullptr;

    // The padded viewport, in cells. What the cell texture and the world rect are
    // sized to; not the window, which is the camera's scale times this. Read the
    // scale off `camera` rather than off Camera: it is a per-scene value and Params
    // deliberately does not keep a second copy.
    int padded_w = 0;
    int padded_h = 0;

    // Borrowed, and may be null for no backdrop. A pointer rather than a copy:
    // the caller owns the backdrop for the whole scene, and copying it into every
    // frame's Params copied each layer's band vector sixty times a second.
    const Backdrop* backdrop = nullptr;
    const std::vector<Prop>* props = nullptr;

    // The world's ARGB streaming texture, already uploaded for this frame.
    gfx::Texture cells;

    // The objective marker, in world cells.
    bool has_objective = false;
    int objective_x = 0;
    int objective_y = 0;

    // The body, at its interpolated draw position -- the caller owns the
    // interpolation and the teleport clamp, because both read simulation state this
    // file has no business seeing.
    gfx::Texture player_tex;
    float player_x = 0.0f;
    float player_y = 0.0f;
    bool facing_left = false;
    int sheet_col = 0;
    int sheet_row = 0;
    // The fallback rectangle's size, in cells, when player_tex is null.
    int player_box_w = 0;
    int player_box_h = 0;

    // The enemies and the arrows. Null, or empty, draws nothing -- which is every
    // frame the golden test composes, so its checksum does not move.
    gfx::Texture enemy_atlas;
    const std::vector<EnemySprite>* enemies = nullptr;
    const std::vector<ArrowSprite>* arrows = nullptr;

    // The loose bodies, the same way: null or empty draws nothing.
    gfx::Texture body_atlas;
    const std::vector<BodySprite>* bodies = nullptr;

    // Spray in the air, the same way: null or empty draws nothing.
    const std::vector<SprayDrop>* spray = nullptr;

    // The field is read for any_light() and its block extent; the texture is what
    // gets drawn, and the caller has already uploaded it.
    const LightField* light = nullptr;
    gfx::Texture light_texture;

    // Seconds of wall clock, for the backdrop's own motion -- drifting clouds and
    // rippling water. Render-only by construction: it is read nowhere but the rig's
    // draw path, and a frame composed at 0 (the default, and what golden_frame_test
    // composes) is the still painting.
    float time_s = 0.0f;
};


// Where a layer sits in the light pass. A field rather than a comment because
// every layer added later has to declare where it belongs, and the static_asserts
// in frame.cpp hold the table to it -- a layer whose declared position disagrees
// with where it actually sits fails the build rather than shipping a comment
// that has gone false.
//
// The light pass is additive and lights everything drawn before it; the UI drawn
// after it (render/overlay.cpp) is the part of the screen that is not in the
// world and must not be lit. A layer that has to be in the world and yet unlit
// would be a third value here, and the asserts would place it after Light.
enum class Lighting : unsigned char {
    Lit,    // in the world, drawn before the light pass
    Light,  // the additive light pass - the boundary, and there is exactly one
};

// One entry in the ordered list. `name` exists for the static_assert messages
// and for anything that later wants to say which layer it is talking about; it
// is not drawn.
struct Layer {
    const char* name;
    Lighting lighting;
    void (*draw)(gfx::Canvas&, const Params&);
};

// Clears and draws the world layers, in order. Presents nothing and leaves the
// draw colour and blend mode as it found them, since the UI drawn after this
// sets both for itself.
//
// A loop over an ordered table rather than a run of draw calls, so adding a band
// is an entry in that table instead of surgery between two comments. The table
// is in frame.cpp, where the draw functions it names have internal linkage.
void compose(gfx::Canvas& canvas, const Params& p);

// The table, exposed for tests and for anything that wants to enumerate the
// composition without drawing it. LAYER_COUNT is its length.
extern const Layer* const LAYERS;
extern const int LAYER_COUNT;

} // namespace frame
