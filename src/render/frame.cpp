#include "render/frame.h"

#include <algorithm>
#include <cmath>
#include "render/backdrop_wrap.h"
#include "render/player_sprite.h"

namespace frame {
namespace {

// --- the layers, one function each ----------------------------------------

// Multiply one channel by a grade channel, the way SDL's own texture colour mod
// does it: v * m / 255, truncating. Matching SDL rather than rounding half-up
// matters because both paths are live in the same frame -- a graded rectangle
// and a graded texture side by side must not land on different values for the
// same grade. 255 is exactly identity either way.
constexpr uint8_t graded(uint8_t v, uint8_t m) {
    return static_cast<uint8_t>(v * m / 255);
}

// Apply a layer's grade to a texture, and always apply it, including when it is
// identity. Colour mod is state that lives on the texture, not on the draw call,
// so a layer that sets it only when it has something to say leaves the previous
// layer's mod on a texture it shares -- and the props, the cell texture and the
// player sheet all outlive the frame.
void apply_grade(SDL_Texture* tex, const Grade& g) {
    if (tex) SDL_SetTextureColorMod(tex, g.r, g.g, g.b);
}

// The clear matters because the authored sky is drawn behind an `if`: a
// missing or unreadable BMP leaves the framebuffer holding whatever was
// in it, which on a double-buffered renderer is two-frames-ago garbage.
// Clearing to the darkest sky tone makes the failure mode "the backdrop
// is flat" instead of "the window is full of noise". It is also the whole
// background of a scene that names no backdrop -- the `empty` sandbox.
void draw_clear(SDL_Renderer* renderer, const Params&, const Grade& g) {
    // Palette colour for darkest sky tone, matching tools/pixel_art.py.
    SDL_SetRenderDrawColor(renderer, graded(0x38, g.r), graded(0x2C, g.g),
                           graded(0x57, g.b), 255);
    SDL_RenderClear(renderer);
}

// A backdrop layer. render/backdrop_set.h has the model and depth_rig.h the
// argument; what is here is the placing.
//
// One function for every layer of every set. Every layer is placed about the
// backdrop's anchor, -(anchor + f * (cam - anchor)), on both axes, and wraps
// horizontally through backdrop_wrap::wrap_axis. With the anchor at the world's
// corner that is -(cam * f), the placement the bg1 family was painted for; and
// a world-sized layer at f <= 1 then covers the window from its first copy at
// every camera position, so a set whose art does not tile never shows a second
// one. (For a world W cells wide and a viewport of V, the layer's right edge at
// the rightmost camera is W - (W - V) * f >= V exactly when f <= 1.
// backdrop_set_test holds every corner-anchored set to it.)
//
// A plain layer is one draw per wrapped copy. Three kinds are drawn in pieces:
//
//   - A banded layer, one draw per band, each band at its own factor. Its rows
//     are not moved vertically relative to one another, so the bands are one
//     image vertically and are cut apart only in how fast they scroll sideways.
//   - The plane (line_scroll), one texture row at a time, because every row is
//     its own depth: each at depth_rig::plane_factor of its row, on both axes.
//   - Rippled rows, one at a time, because every row is its own phase. Paint
//     lying on the plane (on_plane -- the sun's reflection) takes the plane's
//     rows vertically and its own factor horizontally: the reflection of
//     something at infinity stays under it, but it must stay on the water it was
//     painted on as the plane stretches under a rising camera.
//
// Destination edges are computed per boundary, not per piece: piece i's bottom
// and piece i+1's top are one expression on one argument, rounded once, or the
// rounding leaves a one-pixel line of whatever was behind the layer between two
// pieces meant to touch.
//
// At 1080p about 75 plane rows and 27 rippled glint rows are on screen, two
// copies each at most, so the per-row path is a couple of hundred single-row
// copies a frame. SDL batches them into one draw per texture; it is small next
// to the light field, which is where the frame's measured cost is.
void draw_layer(SDL_Renderer* renderer, const Params& p, const ParallaxLayer& l) {
    if (!l.texture || l.tex_h <= 0) return;
    int tex_w = 0;
    SDL_QueryTexture(l.texture, nullptr, nullptr, &tex_w, nullptr);
    if (tex_w <= 0) return;
    apply_grade(l.texture, l.grade);

    const Camera& camera = *p.camera;
    const Backdrop& b = *p.backdrop;  // non-null: only called from the two passes below
    const int scale = camera.scale();
    const int window_w = p.padded_w * scale;
    const int window_h = p.padded_h * scale;
    const float cam_x = camera.view_fx();
    const float cam_y = camera.view_fy();
    const int row_px = l.h / l.tex_h;  // one art row on screen; an integer scale

    // The clouds' own motion, folded into the horizontal origin. Reduced mod the
    // tile here, not left to wrap_axis, so a long session's growing product never
    // reaches the magnitude where a float stops resolving a pixel.
    float drift_px = 0.0f;
    if (l.drift != 0.0f && l.w > 0)
        drift_px = std::fmod(l.drift * p.time_s * static_cast<float>(scale),
                             static_cast<float>(l.w));

    const auto draw_span = [&](float x, const SDL_Rect* src, float y, float h) {
        const backdrop_wrap::Tiling t = backdrop_wrap::wrap_axis(x, l.w, window_w);
        for (int c = 0; c < t.count; ++c) {
            const SDL_FRect dst{t.first + static_cast<float>(c * l.w), y,
                                static_cast<float>(l.w), h};
            SDL_RenderCopyF(renderer, l.texture, src, &dst);
        }
    };

    const float flat_y = depth_rig::origin(cam_y, b.anchor_y, l.parallax_y, scale);
    const float flat_x = depth_rig::origin(cam_x, b.anchor_x, l.parallax_x, scale) + drift_px;
    const auto flat_edge = [&](int row) {
        return std::floor(flat_y + static_cast<float>(row * row_px) + 0.5f);
    };

    if (!l.bands.empty()) {
        for (const Band& band : l.bands) {
            if (band.row1 <= band.row0) continue;
            const float top = flat_edge(band.row0);
            const SDL_Rect src{0, band.row0, tex_w, band.row1 - band.row0};
            draw_span(depth_rig::origin(cam_x, b.anchor_x, band.parallax_x, scale) + drift_px, &src,
                      top, flat_edge(band.row1) - top);
        }
        return;
    }

    const bool rippled = l.ripple_row1 > l.ripple_row0;
    const bool plane_rows = l.line_scroll || l.on_plane;
    if (!l.line_scroll && !rippled) {
        draw_span(flat_x, nullptr, flat_y, static_cast<float>(l.h));
        return;
    }

    // Rows drawn one at a time. For a rippled object layer that is only the ripple
    // range -- backdrop_set_test holds its paint inside it -- and for the plane it
    // is every row from the horizon down.
    const int row0 = l.line_scroll ? std::max(0, b.rig.horizon_row) : l.ripple_row0;
    const int row1 = l.line_scroll ? l.tex_h : std::min(l.ripple_row1, l.tex_h);
    const auto edge = [&](int row) {
        return plane_rows
                   ? std::floor(depth_rig::plane_edge_y(b.rig, row, cam_y, b.anchor_y, row_px) +
                                0.5f)
                   : flat_edge(row);
    };

    float top = edge(row0);
    for (int row = row0; row < row1; ++row) {
        const float bottom = edge(row + 1);
        if (bottom > top && bottom > 0.0f && top < static_cast<float>(window_h)) {
            // The row's horizontal factor is taken at its centre: one depth standing
            // in for the row's whole range, and the middle is least wrong at both
            // edges.
            float x = flat_x;
            if (l.line_scroll) {
                const float f = depth_rig::plane_factor(b.rig, static_cast<float>(row) + 0.5f);
                x = depth_rig::origin(cam_x, b.anchor_x, f, scale) + drift_px;
            }
            if (row >= l.ripple_row0 && row < l.ripple_row1)
                x += depth_rig::ripple_cells(row, p.time_s, b.ripple_amplitude) *
                     static_cast<float>(scale);
            const SDL_Rect src{0, row, tex_w, 1};
            draw_span(x, &src, top, bottom - top);
        }
        top = bottom;
    }
}

// The backdrop, in two passes either side of the world. Each layer brings its
// own Grade, which is why these rows of the table carry PLAIN and ignore theirs.
void draw_backdrop(SDL_Renderer* renderer, const Params& p, const Grade&) {
    if (!p.backdrop) return;
    for (const ParallaxLayer& l : p.backdrop->layers)
        if (!l.is_foreground) draw_layer(renderer, p, l);
}

void draw_foreground(SDL_Renderer* renderer, const Params& p, const Grade&) {
    if (!p.backdrop) return;
    for (const ParallaxLayer& l : p.backdrop->layers)
        if (l.is_foreground) draw_layer(renderer, p, l);
}

// Props. Drawn before the cell texture on purpose -- see the Prop
// comment in frame.h -- so a trunk that overlaps authored terrain gets
// buried by it with no depth test and no new code path, exactly the way
// the cell texture already occludes the backdrop wherever a cell is not
// Empty.
void draw_props(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    if (!p.props) return;
    const Camera& camera = *p.camera;
    for (const Prop& prop : *p.props) {
        if (!prop.texture) continue;
        apply_grade(prop.texture, g);
        const SDL_FRect dst{
            camera.world_to_screen_x(prop.anchor_x - prop.w / 2.0f),
            camera.world_to_screen_y(prop.anchor_y - static_cast<float>(prop.h)),
            static_cast<float>(camera.scale_length(prop.w)),
            static_cast<float>(camera.scale_length(prop.h))
        };
        SDL_RenderCopyF(renderer, prop.texture, nullptr, &dst);
    }
}

// Drawn shifted by the camera's sub-cell remainder. The view is unclamped
// wherever the player usually is, so the player sits near screen centre
// and it is the world that scrolls -- in whole cells that is a jerk of
// everything on screen at once.
void draw_cells(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    const Camera& camera = *p.camera;
    apply_grade(p.cells, g);
    const SDL_FRect world_dst{
        -camera.frac_x() * camera.scale(),
        -camera.frac_y() * camera.scale(),
        static_cast<float>(p.padded_w * camera.scale()),
        static_cast<float>(p.padded_h * camera.scale())
    };
    SDL_RenderCopyF(renderer, p.cells, nullptr, &world_dst);
}

// --- the objective marker ---
//
// Drawn in world cells, not screen pixels, which is the opposite choice
// from the reticle and for the opposite reason: the reticle is a cursor
// and has to keep its legibility at any scale, while this is a thing
// that is somewhere and has to sit still in the world as the camera
// moves over it.
//
// Drawn after the world and before the player, so the body passes in
// front of it, and before the light pass, which is additive and
// therefore leaves an unlit marker at exactly the colour written here.
//
// Three concentric squares rather than a sprite: no new asset, no
// manifest entry, and the dark ring is what stops it disappearing
// against sand, the same reason the reticle has an outline.
void draw_objective(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    if (!p.has_objective) return;
    const Camera& camera = *p.camera;
    const float gx = static_cast<float>(p.objective_x);
    const float gy = static_cast<float>(p.objective_y);
    struct Ring { int cells; uint8_t r, g, b; };
    const Ring rings[3] = {
        // A near-black outline, deliberately not tied to the backdrop. This
        // ring's job is to stay legible against sand, the same job the
        // reticle's outline has, and nothing about it wants to track the sky.
        { 12, 0x14, 0x10, 0x22 },
        { 10, 0xF0, 0xC0, 0x40 },
        {  4, 0xFF, 0xFF, 0xFF },
    };
    for (const Ring& ring : rings) {
        SDL_SetRenderDrawColor(renderer, graded(ring.r, g.r), graded(ring.g, g.g),
                               graded(ring.b, g.b), 255);
        const SDL_FRect box{
            camera.world_to_screen_x(gx - ring.cells / 2.0f),
            camera.world_to_screen_y(gy - ring.cells / 2.0f),
            static_cast<float>(camera.scale_length(ring.cells)),
            static_cast<float>(camera.scale_length(ring.cells))
        };
        SDL_RenderFillRectF(renderer, &box);
    }
}

// The player is not a cell, so it is not in the pixel buffer either --
// it is drawn on top of the world as its own sprite. Float rect and
// float position: rounding either one reintroduces the whole-cell jitter.
//
// Positioned by subtracting the offsets from the box's corner, which is
// what anchoring to the box's bottom-centre works out to -- the sprite's
// baseline lands on the box's baseline and its extra width is split
// evenly either side.
void draw_player(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    const Camera& camera = *p.camera;
    const SDL_FRect body{
        camera.world_to_screen_x(p.player_x - player_sprite::OFFSET_X),
        camera.world_to_screen_y(p.player_y - player_sprite::OFFSET_Y),
        static_cast<float>(camera.scale_length(player_sprite::FRAME_W)),
        static_cast<float>(camera.scale_length(player_sprite::FRAME_H))
    };
    if (p.player_tex) {
        apply_grade(p.player_tex, g);
        const SDL_RendererFlip flip =
            p.facing_left ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE;

        // The source rect is the whole of "this is a sheet rather than a
        // sprite" on this side of the boundary. Which cell it names is decided
        // in render/player_anim.cpp, which is SDL-free and tested.
        const SDL_Rect src{
            p.sheet_col * player_sprite::FRAME_W,
            p.sheet_row * player_sprite::FRAME_H,
            player_sprite::FRAME_W, player_sprite::FRAME_H
        };
        SDL_RenderCopyExF(renderer, p.player_tex, &src, &body, 0.0, nullptr, flip);
    } else {
        // A plain rectangle, kept as the fallback rather than deleted. A missing
        // asset should degrade to a visible player, not an invisible one --
        // load_art_texture already printed why it failed, and a game you can
        // still move around in is a better diagnostic than a world with nothing
        // in it.
        SDL_SetRenderDrawColor(renderer, graded(235, g.r), graded(235, g.g),
                               graded(245, g.b), 255);
        const SDL_FRect box{
            camera.world_to_screen_x(p.player_x),
            camera.world_to_screen_y(p.player_y),
            static_cast<float>(camera.scale_length(p.player_box_w)),
            static_cast<float>(camera.scale_length(p.player_box_h))
        };
        SDL_RenderFillRectF(renderer, &box);
    }
}

// The enemies, before the player so the player walks in front of them.
//
// Drawn exactly like the player -- a frame anchored bottom-centre on the box,
// flipped for facing -- except that the frame comes out of an atlas the caller
// rebuilds from each body's surviving pixels rather than out of a fixed sheet.
// The flip is SDL_FLIP_HORIZONTAL over the pose's rectangle, which the caller
// anchors to match Enemy::pixel_at's flip, so a pixel drawn over a cell and the
// pixel an arrow finds in that cell stay the same pixel.
void draw_enemies(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    if (!p.enemies || p.enemies->empty() || !p.enemy_atlas) return;
    const Camera& camera = *p.camera;
    apply_grade(p.enemy_atlas, g);
    for (const EnemySprite& e : *p.enemies) {
        const SDL_FRect body{camera.world_to_screen_x(e.x - static_cast<float>(e.offset_x)),
                             camera.world_to_screen_y(e.y - static_cast<float>(e.offset_y)),
                             static_cast<float>(camera.scale_length(e.src.w)),
                             static_cast<float>(camera.scale_length(e.src.h))};
        SDL_RenderCopyExF(renderer, p.enemy_atlas, &e.src, &body, 0.0, nullptr,
                          e.facing_left ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
    }
}

// Arrows, after the player, so a shot leaving the bow is in front of the body
// that loosed it.
//
// Seven one-cell squares stepped back from the tip along the direction of
// travel, rather than a line. A line is one screen pixel wide at any scale and
// reads as a scratch on the glass; cell-sized squares are drawn in the world's
// own unit, so an arrow is pixel art at the same resolution as everything it
// flies past.
void draw_arrows(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    if (!p.arrows || p.arrows->empty()) return;
    const Camera& camera = *p.camera;
    struct Part { int from, to; uint8_t r, gg, b; };
    // Head, shaft, fletching -- the head in the mask's bone so it reads against
    // the dark moss, the fletching pale so the tail of a shot buried in a wall
    // is still visible against the wall.
    const Part parts[] = {
        {0, 0, 0xC8, 0xC4, 0xB4},
        {1, 4, 0x7A, 0x5A, 0x34},
        {5, 6, 0xE4, 0xDE, 0xCC},
    };
    const float cell = static_cast<float>(camera.scale_length(1));
    for (const ArrowSprite& a : *p.arrows) {
        float dx = a.dir_x, dy = a.dir_y;
        if (dx == 0.0f && dy == 0.0f) dy = 1.0f;
        for (const Part& part : parts) {
            SDL_SetRenderDrawColor(renderer, graded(part.r, g.r), graded(part.gg, g.g),
                                   graded(part.b, g.b), 255);
            for (int i = part.from; i <= part.to; ++i) {
                const float wx = std::floor(a.tip_x - dx * static_cast<float>(i));
                const float wy = std::floor(a.tip_y - dy * static_cast<float>(i));
                const SDL_FRect r{camera.world_to_screen_x(wx), camera.world_to_screen_y(wy),
                                  cell, cell};
                SDL_RenderFillRectF(renderer, &r);
            }
        }
    }
}

// --- the pass that can darken -------------------------
//
// One full-screen rectangle in SDL_BLENDMODE_MOD, which is dst = dst * src.
// Everything before it can only add, so without this no biome, no time of day
// and no depth band could be darker than the art as authored.
//
// It sits before the light pass and that ordering is the design. Multiply first,
// add second, so a fire at night burns at the brightness the light pass computed
// for it instead of being graded down with the rock it is standing on. The
// reverse order dims the one thing in the frame that is supposed to survive a
// dark grade, and it reads as the lighting having stopped working.
//
// No custom blend mode is composed for this and none is needed.
// SDL_BLENDMODE_MOD is supported by both the accelerated and the software
// backends -- the second matters, because the golden frame test rasterises in
// software and would be blind to a custom blend mode.
//
// The alpha channel is deliberately 255 and MOD ignores it: MOD multiplies
// colour and leaves destination alpha alone, so this cannot be used to fade
// anything.
void draw_grade(SDL_Renderer* renderer, const Params& p, const Grade&) {
    if (p.world_grade.identity()) return;  // the common case: not one draw call

    SDL_BlendMode prev = SDL_BLENDMODE_NONE;
    SDL_GetRenderDrawBlendMode(renderer, &prev);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_MOD);
    SDL_SetRenderDrawColor(renderer, p.world_grade.r, p.world_grade.g,
                           p.world_grade.b, 255);
    SDL_RenderFillRect(renderer, nullptr);  // null rect is the whole target
    SDL_SetRenderDrawBlendMode(renderer, prev);
}

// One extra RenderCopy -- the whole cost of the lighting feature on the GPU
// side.
//
// Drawn after the world and after the player, and before the reticle and HUD.
// Everything in the world is a surface that light lands on, including the
// player, who otherwise stays flatly lit while standing inside a fire.
// Everything after it is UI, which is not in the world and must not be tinted by
// it. That ordering is declared as well as observed: this is the table's one
// Lighting::Light entry, and the static_asserts below hold every Lit layer in
// front of it.
//
// The destination is the block extent, not the padded cell extent, and that is
// what aligns the stretch: cols()*BLOCK is the padded width rounded up to a
// whole block, so each texel's centre lands on the centre of the cells it was
// computed from. Sized to the padded extent instead, every texel would sit up to
// half a block off and the glow would trail behind the flame that cast it.
void draw_light(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    if (!p.light || !p.light->any_light()) return;
    const Camera& camera = *p.camera;
    apply_grade(p.light_texture, g);
    const SDL_FRect light_dst{
        -camera.frac_x() * camera.scale(),
        -camera.frac_y() * camera.scale(),
        static_cast<float>(p.light->cols() * LightField::BLOCK * camera.scale()),
        static_cast<float>(p.light->rows() * LightField::BLOCK * camera.scale())
    };
    SDL_RenderCopyF(renderer, p.light_texture, nullptr, &light_dst);
}

// --- the ordered list -----------------------------------------------------
//
// The order in this table is the feature: reading it top to bottom is reading
// the frame back to front, and a band is a row here rather than an insertion
// between two comments in a long function.
constexpr Grade PLAIN{};  // 255,255,255 - drawn as authored

// Every row is PLAIN. The per-layer multiply is live where it belongs, on
// each backdrop layer's own Grade (frame::ParallaxLayer), and the shipped sets
// leave it at identity because their art carries its own aerial perspective.
// The backdrop rows carry PLAIN for that reason and not by oversight: a grade
// here would multiply on top of the one the art was authored against and
// neither number would say so.
//
// (There was one graded row: the generated mountains, at 0.60, to separate them
// from a sky of nearly the same luminance. It went with the generated backdrop;
// see render/backdrop_set.h.)
constexpr Layer TABLE[] = {
    {"clear", Lighting::Lit, PLAIN, draw_clear},
    {"backdrop", Lighting::Lit, PLAIN, draw_backdrop},
    {"props", Lighting::Lit, PLAIN, draw_props},
    {"cells", Lighting::Lit, PLAIN, draw_cells},
    {"objective", Lighting::Lit, PLAIN, draw_objective},
    {"enemies", Lighting::Lit, PLAIN, draw_enemies},
    {"player", Lighting::Lit, PLAIN, draw_player},
    {"arrows", Lighting::Lit, PLAIN, draw_arrows},
    {"foreground", Lighting::Lit, PLAIN, draw_foreground},
    {"grade", Lighting::Grade, PLAIN, draw_grade},
    {"light", Lighting::Light, PLAIN, draw_light},
    // Nothing Unlit yet, and that is not an omission: the UI drawn after the
    // light pass lives elsewhere. The value exists so that the first thing to
    // cross the boundary declares which side it is on instead of inheriting a
    // position.
    //
    // `grade` is a layer with no caller -- nothing sets Params::world_grade, so
    // it returns before its first draw call on every frame the game currently
    // composes. It ships because the per-layer half of the same mechanism is
    // live on every backdrop layer, and because the pass and its ordering argument are the part that is
    // expensive to add later. It becomes a defect the day it is still unset and
    // the ordering claim above has stopped being checked by anything.
};
constexpr int TABLE_COUNT = static_cast<int>(sizeof(TABLE) / sizeof(TABLE[0]));

// --- the invariant, held by the compiler ----------------------------------
//
// Lighting is a claim about where a layer sits relative to the light pass, so it
// is enforced at compile time, in the file that would have to be edited to break
// it -- the same guard CMakeLists.txt's source sets give the
// simulation/rendering boundary.

// How many entries carry a given value.
constexpr int count_of(Lighting want) {
    int n = 0;
    for (int i = 0; i < TABLE_COUNT; ++i) if (TABLE[i].lighting == want) ++n;
    return n;
}

static_assert(count_of(Lighting::Light) == 1,
              "the layer table needs exactly one Lighting::Light entry - it is the "
              "boundary the other values are defined against, so zero of them makes "
              "Lit and Unlit meaningless and two of them makes them ambiguous");

// At most one, not exactly one. Two grade quads would multiply into a third
// grade that nothing declares; zero is legal, because a build with no world-wide
// grade is a coherent build.
static_assert(count_of(Lighting::Grade) <= 1,
              "at most one Lighting::Grade entry - two full-screen multiplies compose "
              "into a third grade that nothing declares, and the second would be tuned "
              "against the first without either row saying so");

// A rank rather than a boundary index: with a grade pass there are three
// positions in a fixed order, and the invariant is that the table never goes
// backwards through them. Written as a rank rather than as pairwise comparisons
// so the next value inserted into Lighting is one line here instead of a case
// analysis.
constexpr int rank(Lighting l) {
    switch (l) {
        case Lighting::Lit:   return 0;
        case Lighting::Grade: return 1;
        case Lighting::Light: return 2;
        case Lighting::Unlit: return 3;
    }
    return -1;
}

constexpr bool lighting_matches_order() {
    for (int i = 1; i < TABLE_COUNT; ++i) {
        if (rank(TABLE[i].lighting) < rank(TABLE[i - 1].lighting)) return false;
    }
    return true;
}
static_assert(lighting_matches_order(),
              "a layer's declared Lighting disagrees with where it sits in the table. "
              "The order is Lit, then Grade, then Light, then Unlit, and it is not a "
              "convention: the grade multiplies and the light pass adds, so putting the "
              "grade after the light is the difference between a fire that survives "
              "nightfall and one that gets dimmed by it");

// The two mechanisms are separate fields and this is the guard on that. A grade
// quad carrying a per-layer grade of its own would multiply twice -- once as a
// property of the layer and once as its whole purpose -- and the second one is
// invisible in the table, because it looks exactly like every other row.
constexpr bool grade_layer_is_plain() {
    for (int i = 0; i < TABLE_COUNT; ++i) {
        if (TABLE[i].lighting == Lighting::Grade && !TABLE[i].grade.identity())
            return false;
    }
    return true;
}
static_assert(grade_layer_is_plain(),
              "the Lighting::Grade row must carry an identity per-layer grade - it "
              "reads Params::world_grade and applies that. A grade on the grade row "
              "multiplies twice and neither number says so");

} // namespace

const Layer* const LAYERS = TABLE;
const int LAYER_COUNT = TABLE_COUNT;

void compose(SDL_Renderer* renderer, const Params& p) {
    for (int i = 0; i < TABLE_COUNT; ++i) TABLE[i].draw(renderer, p, TABLE[i].grade);
}

} // namespace frame
