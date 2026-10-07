#pragma once
#include <SDL.h>
#include <vector>
#include "game/camera.h"
#include "render/backdrop_wrap.h"
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
    SDL_Texture* texture;
    int w, h;  // native size, in world cells (1 BMP pixel = 1 cell)
    float anchor_x;  // world cell, bottom-centre of the sprite
    float anchor_y;
};

// One enemy, as the frame draws it. The body is not a sheet cell: it is whatever
// is left of it, rebuilt into a slot of `Params::enemy_atlas` by the caller from
// the simulation's own pixel mask, so what is drawn is exactly what an arrow can
// still hit. `src` is that slot.
//
// Position is the collision box's top-left, interpolated, in world cells -- the
// same convention as the player -- and the frame anchors it bottom-centre with
// Enemy::OFFSET_X/Y, like player_sprite::OFFSET_X/Y does for the owl.
struct EnemySprite {
    SDL_Rect src;
    float x, y;
    bool facing_left;
};

// One arrow: where its tip is, in world cells, and which way it points as a
// unit vector. A direction of (0, 0) -- an arrow at rest -- is drawn pointing
// down, which is how a dropped arrow lies.
struct ArrowSprite {
    float tip_x, tip_y;
    float dir_x, dir_y;
};

// A multiply. 255 is unchanged, 128 is half, 0 is black -- the operation the
// light pass cannot do.
//
// The light pass only ever adds, so every biome, every depth band and every time
// of day is at least as bright as the art was authored, and nothing on screen
// can be made darker than the pixel that was drawn. Night, underground, fog and
// aerial perspective are all the same missing operation, and it is this one.
//
// A struct of three bytes rather than one exposure number because the useful
// version is a tint: a distant band does not just dim, it drifts toward the
// colour of the air between you and it. One number would have to be tuned three
// times over as soon as anything wanted that.
//
// Applied by SDL_SetTextureColorMod for a textured layer, which is a multiply
// the renderer already does for free -- a graded layer costs no extra draw call
// and no extra texture. The world-wide grade in Params is the one that costs a
// quad, and it is skipped entirely when it is identity.
struct Grade {
    unsigned char r = 255, g = 255, b = 255;

    constexpr bool identity() const { return r == 255 && g == 255 && b == 255; }
};

// One layer of an authored multi-layer backdrop. The difference from the
// generated Backdrop below is where the numbers come from: that one's factors
// and sizes are derived together by tools/generate_backdrop.py, which writes
// backdrop_layers.h, so they can be constants in a header. A hand-painted set
// has no such generator, so its factors arrive with the art and have to be
// carried per-layer at runtime.
//
// w/h are the layer's size on screen, not its texture's size, and the two differ
// on purpose. draw_backdrop_layer puts the texture into a dst rect of exactly
// w x h, so authored art at its native pixel size is stretched by an integer
// factor at draw time and the texture stays small in VRAM. Sized as textures
// instead, a stack of layers with enough vertical headroom to pan costs two
// orders of magnitude more memory; the scaling is nearest-neighbour, so an
// integer factor is exact.
//
// One horizontal slice of an authored layer, scrolling at its own rate. The type
// and the argument for it live in render/backdrop_wrap.h, which knows no SDL, so
// a band table can be checked against the BMP it describes by a headless suite.
using Band = backdrop_wrap::Band;

struct ParallaxLayer {
    SDL_Texture* texture = nullptr;
    int w = 0, h = 0;
    float parallax_x = 1.0f;
    float parallax_y = 1.0f;
    Grade grade{};
    bool is_foreground = false;  // drawn in front of the player and the cells

    // The texture's own height in pixels, which w/h above deliberately are not.
    // Needed only by `bands`: a band names texture rows, and turning those into
    // destination rows is exactly h / tex_h.
    int tex_h = 0;

    // Empty for every layer that is an object. Non-empty only for a surface -- see
    // Band. When set, parallax_x above is unused and each band carries its own.
    std::vector<Band> bands;
};

// The generated parallax layers.
//
// The factors live in the generated render/backdrop_layers.h, written by
// `python tools/generate_backdrop.py --header` from the same table that sizes
// the images. Keeping them in both C++ and Python with nothing enforcing
// agreement produces a seam at the pan limit; there is now one copy.
//
// The factors themselves are eyeballed and unmeasured. The one reference that
// could have measured them cannot, because its frames are separately generated
// scenes rather than one camera pan.
//
// `ground` is a tile rather than an image. The other two are as wide as the
// window plus the whole pan range at their own factor, which is why the nearer
// one is the larger file; the plane sits nearer than either and would be tens of
// megabytes priced that way. It wraps instead -- ground_w/ground_h are a tile
// size, and how many copies the window needs is asked of
// backdrop_wrap::wrap_axis every frame.
//
// Its height is not a height. The tile's rows are the plane's depth, sampled top
// to bottom exactly once across the band, which is what lets one texture carry
// both the value ramp and the mark gradient.
struct Backdrop {
    SDL_Texture* sky = nullptr;
    SDL_Texture* mountains = nullptr;
    SDL_Texture* ground = nullptr;
    int sky_w = 0, sky_h = 0;
    int mountain_w = 0, mountain_h = 0;
    int ground_w = 0, ground_h = 0;

    // The authored stack. Empty means this scene uses the three generated layers
    // above, and that is the whole switch -- the two mechanisms are never both live
    // in one frame, because draw_sky and draw_mountains return early when this is
    // non-empty. A scene gets one backdrop system or the other and never a
    // half-composed blend, which is what a per-layer flag would have made
    // representable.
    //
    // Drawn in vector order, so the vector is the depth ordering: index 0 is
    // furthest back. is_foreground is the one exception and it is not a reordering --
    // it moves a layer past the player into the second of the two passes.
    std::vector<ParallaxLayer> layers;
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

    // Which paradigm the loaded scene is, and how big its world is -- the two things
    // the backdrop layout cannot be decided without.
    //
    // Here rather than read off the Camera because they are facts about the scene,
    // not about the viewport, and the camera is deliberately stateless about which
    // world it is looking at. The defaults are the engine's historical world size,
    // so a caller built before this field existed composes exactly the frame it did
    // before.
    bool is_infinite = false;
    int world_w = 1920;
    int world_h = 1080;

    Backdrop backdrop;
    const std::vector<Prop>* props = nullptr;

    // The world's ARGB streaming texture, already uploaded for this frame.
    SDL_Texture* cells = nullptr;

    // The objective marker, in world cells.
    bool has_objective = false;
    int objective_x = 0;
    int objective_y = 0;

    // The body, at its interpolated draw position -- the caller owns the
    // interpolation and the teleport clamp, because both read simulation state this
    // file has no business seeing.
    SDL_Texture* player_tex = nullptr;
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
    SDL_Texture* enemy_atlas = nullptr;
    const std::vector<EnemySprite>* enemies = nullptr;
    const std::vector<ArrowSprite>* arrows = nullptr;

    // The field is read for any_light() and its block extent; the texture is what
    // gets drawn, and the caller has already uploaded it.
    const LightField* light = nullptr;
    SDL_Texture* light_texture = nullptr;

    // The world-wide multiply -- night, underground, fog, per-biome grading.
    // Applied to everything in the world after the last world layer and before the
    // additive light pass, so a fire keeps its brightness in a graded scene instead
    // of being dimmed by it.
    //
    // Identity by default, and identity means the quad is not drawn at all.
    //
    // Nothing sets this yet. It is a knob with no caller -- see the note at the
    // layer table for why it ships anyway and what would make it a defect.
    Grade world_grade;
};


// Where a layer sits in the light pass. A field rather than a comment because
// every layer added later has to declare where it belongs, and the static_asserts
// in frame.cpp hold the table to it -- a layer whose declared position disagrees
// with where it actually sits fails the build rather than shipping a comment
// that has gone false.
//
// This says only what the composition can currently enforce, which is an
// ordering.
//
// Grade is what makes Lit mean more than "earlier". The grade quad multiplies
// everything drawn so far and the light pass then adds on top of the result,
// which is why they are two entries and in that order: a fire at night must not
// be dimmed by the night. Reverse them and the only light source in the frame
// gets graded down with the rock it is sitting on.
//
// What it does not claim: that a layer can sit behind the world and be exempt
// from the additive pass. The grade is a multiply over everything already drawn,
// not a per-layer exemption, and expressing that needs a render target. The
// per-layer Grade on a Layer is the piece that is per-layer, and it is a
// separate field for exactly that reason -- one of these two is a property of a
// layer and the other of the frame, and collapsing them is how a value that
// means two things gets tuned for one of them.
enum class Lighting : unsigned char {
    Lit,  // in the world, drawn before the grade and the light pass
    Grade,  // the world-wide multiply - at most one, and it may be absent
    Light,  // the additive light pass - the boundary, and there is exactly one
    Unlit,  // drawn after it and deliberately untouched by it
};

// One entry in the ordered list. `name` exists for the static_assert messages
// and for anything that later wants to say which layer it is talking about; it
// is not drawn.
//
// `grade` is this layer's own multiply, applied by whatever primitive the layer
// draws with. A layer that ignores it is a lie the compiler cannot catch, so
// every draw function below takes it as an argument rather than reading it from
// the table -- a new layer cannot be written without the parameter being in
// front of the person writing it.
struct Layer {
    const char* name;
    Lighting lighting;
    Grade grade;
    void (*draw)(SDL_Renderer*, const Params&, const Grade&);
};

// Clears and draws the world layers, in order. Presents nothing and leaves the
// draw colour and blend mode as it found them, since the UI drawn after this
// sets both for itself.
//
// A loop over an ordered table rather than a run of draw calls, so adding a band
// is an entry in that table instead of surgery between two comments. The table
// is in frame.cpp, where the draw functions it names have internal linkage.
void compose(SDL_Renderer* renderer, const Params& p);

// The table, exposed for tests and for anything that wants to enumerate the
// composition without drawing it. LAYER_COUNT is its length.
extern const Layer* const LAYERS;
extern const int LAYER_COUNT;

// Screen y of the ground plane's far edge, given the loaded mountains' height.
//
// Exposed because render/surface_plane.cpp needs to know which rows of the
// window are on the plane, which is this number plus PLANE_TEXEL_SCALE. The
// derivation -- why the horizon is a row of the mountains BMP and why it moves
// at the mountains' vertical factor rather than the plane's -- is in frame.cpp
// and stays there.
float ground_horizon_y(const Camera& camera, int mountain_h);

} // namespace frame
