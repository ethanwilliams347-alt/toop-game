#include "render/frame.h"

#include <algorithm>
#include <cmath>
#include "render/backdrop_wrap.h"
#include "render/player_sprite.h"

namespace frame {
namespace {

// --- the layers, one function each ----------------------------------------

// The clear matters because the authored sky is drawn behind an `if`: a
// missing or unreadable BMP leaves the framebuffer holding whatever was
// in it, which on a double-buffered renderer is two-frames-ago garbage.
// Clearing to the darkest sky tone makes the failure mode "the backdrop
// is flat" instead of "the window is full of noise". It is also the whole
// background of a scene that names no backdrop -- the `empty` sandbox.
void draw_clear(gfx::Canvas& canvas, const Params&) {
    // Palette colour for darkest sky tone, matching tools/pixel_art.py.
    canvas.set_draw_color(0x38, 0x2C, 0x57, 255);
    canvas.clear();
}

// A backdrop layer. render/backdrop_set.h has the model and depth_rig.h the
// argument; what is here is the placing.
//
// One function for every layer of every set. Every layer is placed about the
// backdrop's anchor, -(anchor + f * (cam - anchor)), on both axes, and wraps
// horizontally through backdrop_wrap::wrap_axis. With the anchor at the world's
// corner that is -(cam * f), the placement the bg_valley family was painted for; and
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
void draw_layer(gfx::Canvas& canvas, const Params& p, const ParallaxLayer& l) {
    if (!l.texture || l.tex_h <= 0) return;
    const int tex_w = l.texture.w;
    if (tex_w <= 0) return;

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

    const auto draw_span = [&](float x, const gfx::Rect* src, float y, float h) {
        const backdrop_wrap::Tiling t = backdrop_wrap::wrap_axis(x, l.w, window_w);
        for (int c = 0; c < t.count; ++c) {
            const gfx::FRect dst{t.first + static_cast<float>(c * l.w), y, static_cast<float>(l.w),
                                 h};
            canvas.blit(l.texture, src, dst);
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
            const gfx::Rect src{0, band.row0, tex_w, band.row1 - band.row0};
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
            const gfx::Rect src{0, row, tex_w, 1};
            draw_span(x, &src, top, bottom - top);
        }
        top = bottom;
    }
}

// The backdrop, in two passes either side of the world.
void draw_backdrop(gfx::Canvas& canvas, const Params& p) {
    if (!p.backdrop) return;
    for (const ParallaxLayer& l : p.backdrop->layers)
        if (!l.is_foreground) draw_layer(canvas, p, l);
}

void draw_foreground(gfx::Canvas& canvas, const Params& p) {
    if (!p.backdrop) return;
    for (const ParallaxLayer& l : p.backdrop->layers)
        if (l.is_foreground) draw_layer(canvas, p, l);
}

// Props. Drawn before the cell texture on purpose -- see the Prop
// comment in frame.h -- so a trunk that overlaps authored terrain gets
// buried by it with no depth test and no new code path, exactly the way
// the cell texture already occludes the backdrop wherever a cell is not
// Empty.
void draw_props(gfx::Canvas& canvas, const Params& p) {
    if (!p.props) return;
    const Camera& camera = *p.camera;
    for (const Prop& prop : *p.props) {
        if (!prop.texture) continue;
        const gfx::FRect dst{camera.world_to_screen_x(prop.anchor_x - prop.w / 2.0f),
                             camera.world_to_screen_y(prop.anchor_y - static_cast<float>(prop.h)),
                             static_cast<float>(camera.scale_length(prop.w)),
                             static_cast<float>(camera.scale_length(prop.h))};
        canvas.blit(prop.texture, nullptr, dst);
    }
}

// Drawn shifted by the camera's sub-cell remainder. The view is unclamped
// wherever the player usually is, so the player sits near screen centre
// and it is the world that scrolls -- in whole cells that is a jerk of
// everything on screen at once.
void draw_cells(gfx::Canvas& canvas, const Params& p) {
    const Camera& camera = *p.camera;
    const gfx::FRect world_dst{-camera.frac_x() * camera.scale(), -camera.frac_y() * camera.scale(),
                               static_cast<float>(p.padded_w * camera.scale()),
                               static_cast<float>(p.padded_h * camera.scale())};
    canvas.blit(p.cells, nullptr, world_dst);
}

// --- loose bodies ---
//
// Straight after the cells, because they are the cells: a body is a piece of the
// world that happens to be moving, and everything drawn over the world is drawn
// over it too. Turned about its centre of mass by the angle it really has and put
// where it really is, between steps -- not where the grid rounded its cells to --
// so a toppling post sweeps round as one object instead of being repainted a
// cell at a time.
void draw_bodies(gfx::Canvas& canvas, const Params& p) {
    if (!p.bodies || p.bodies->empty() || !p.body_atlas) return;
    const Camera& camera = *p.camera;
    const float scale = static_cast<float>(camera.scale_length(1));
    for (const BodySprite& b : *p.bodies) {
        const gfx::FRect dst{
            camera.world_to_screen_x(b.x - b.pivot_x), camera.world_to_screen_y(b.y - b.pivot_y),
            static_cast<float>(b.src.w) * scale, static_cast<float>(b.src.h) * scale};
        const gfx::FPoint centre{b.pivot_x * scale, b.pivot_y * scale};
        canvas.blit_ex(p.body_atlas, &b.src, dst, static_cast<double>(b.angle_deg), &centre,
                       gfx::Flip::None);
    }
}

// --- spray ---
//
// Over the bodies, because a splash is thrown up in front of what made it, and
// in the world, so it is lit with the water it came from. A cell-sized square
// each, at the drop's own colour: a drop is liquid that has left the grid for a
// few steps, and drawn as anything but a cell of it the landing would show.
void draw_spray(gfx::Canvas& canvas, const Params& p) {
    if (!p.spray || p.spray->empty()) return;
    const Camera& camera = *p.camera;
    const float scale = static_cast<float>(camera.scale_length(1));
    canvas.set_blend(gfx::Blend::Alpha);
    for (const SprayDrop& d : *p.spray) {
        const gfx::FRect dst{camera.world_to_screen_x(d.x - 0.5f),
                             camera.world_to_screen_y(d.y - 0.5f), scale, scale};
        canvas.set_draw_color(static_cast<uint8_t>(d.color >> 16),
                              static_cast<uint8_t>(d.color >> 8), static_cast<uint8_t>(d.color),
                              static_cast<uint8_t>(d.color >> 24));
        canvas.fill_rect(dst);
    }
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
void draw_objective(gfx::Canvas& canvas, const Params& p) {
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
        canvas.set_draw_color(ring.r, ring.g, ring.b, 255);
        const gfx::FRect box{camera.world_to_screen_x(gx - ring.cells / 2.0f),
                             camera.world_to_screen_y(gy - ring.cells / 2.0f),
                             static_cast<float>(camera.scale_length(ring.cells)),
                             static_cast<float>(camera.scale_length(ring.cells))};
        canvas.fill_rect(box);
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
void draw_player(gfx::Canvas& canvas, const Params& p) {
    const Camera& camera = *p.camera;
    const gfx::FRect body{camera.world_to_screen_x(p.player_x - player_sprite::OFFSET_X),
                          camera.world_to_screen_y(p.player_y - player_sprite::OFFSET_Y),
                          static_cast<float>(camera.scale_length(player_sprite::FRAME_W)),
                          static_cast<float>(camera.scale_length(player_sprite::FRAME_H))};
    if (p.player_tex) {
        const gfx::Flip flip = p.facing_left ? gfx::Flip::Horizontal : gfx::Flip::None;

        // The source rect is the whole of "this is a sheet rather than a
        // sprite" on this side of the boundary. Which cell it names is decided
        // in render/player_anim.cpp, which is SDL-free and tested.
        const gfx::Rect src{p.sheet_col * player_sprite::FRAME_W,
                            p.sheet_row * player_sprite::FRAME_H, player_sprite::FRAME_W,
                            player_sprite::FRAME_H};
        canvas.blit_ex(p.player_tex, &src, body, 0.0, nullptr, flip);
    } else {
        // A plain rectangle, kept as the fallback rather than deleted. A missing
        // asset should degrade to a visible player, not an invisible one --
        // load_art_texture already printed why it failed, and a game you can
        // still move around in is a better diagnostic than a world with nothing
        // in it.
        canvas.set_draw_color(235, 235, 245, 255);
        const gfx::FRect box{camera.world_to_screen_x(p.player_x),
                             camera.world_to_screen_y(p.player_y),
                             static_cast<float>(camera.scale_length(p.player_box_w)),
                             static_cast<float>(camera.scale_length(p.player_box_h))};
        canvas.fill_rect(box);
    }
}

// The enemies, before the player so the player walks in front of them.
//
// Drawn exactly like the player -- a frame anchored bottom-centre on the box,
// flipped for facing -- except that the frame comes out of an atlas the caller
// rebuilds from each body's surviving pixels rather than out of a fixed sheet.
// The flip is a horizontal flip over the pose's rectangle, which the caller
// anchors to match Enemy::pixel_at's flip, so a pixel drawn over a cell and the
// pixel an arrow finds in that cell stay the same pixel.
void draw_enemies(gfx::Canvas& canvas, const Params& p) {
    if (!p.enemies || p.enemies->empty() || !p.enemy_atlas) return;
    const Camera& camera = *p.camera;
    for (const EnemySprite& e : *p.enemies) {
        const gfx::FRect body{camera.world_to_screen_x(e.x - static_cast<float>(e.offset_x)),
                              camera.world_to_screen_y(e.y - static_cast<float>(e.offset_y)),
                              static_cast<float>(camera.scale_length(e.src.w)),
                              static_cast<float>(camera.scale_length(e.src.h))};
        canvas.blit_ex(p.enemy_atlas, &e.src, body, 0.0, nullptr,
                       e.facing_left ? gfx::Flip::Horizontal : gfx::Flip::None);
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
void draw_arrows(gfx::Canvas& canvas, const Params& p) {
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
            canvas.set_draw_color(part.r, part.gg, part.b, 255);
            for (int i = part.from; i <= part.to; ++i) {
                const float wx = std::floor(a.tip_x - dx * static_cast<float>(i));
                const float wy = std::floor(a.tip_y - dy * static_cast<float>(i));
                const gfx::FRect r{camera.world_to_screen_x(wx), camera.world_to_screen_y(wy), cell,
                                   cell};
                canvas.fill_rect(r);
            }
        }
    }
}

// One extra blit -- the whole cost of the lighting feature on the GPU
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
void draw_light(gfx::Canvas& canvas, const Params& p) {
    if (!p.light || !p.light->any_light()) return;
    const Camera& camera = *p.camera;
    const gfx::FRect light_dst{
        -camera.frac_x() * camera.scale(), -camera.frac_y() * camera.scale(),
        static_cast<float>(p.light->cols() * LightField::BLOCK * camera.scale()),
        static_cast<float>(p.light->rows() * LightField::BLOCK * camera.scale())};
    canvas.blit(p.light_texture, nullptr, light_dst);
}

// --- the ordered list -----------------------------------------------------
//
// The order in this table is the feature: reading it top to bottom is reading
// the frame back to front, and a band is a row here rather than an insertion
// between two comments in a long function.
constexpr Layer TABLE[] = {
    {"clear", Lighting::Lit, draw_clear},           {"backdrop", Lighting::Lit, draw_backdrop},
    {"props", Lighting::Lit, draw_props},           {"cells", Lighting::Lit, draw_cells},
    {"bodies", Lighting::Lit, draw_bodies},         {"spray", Lighting::Lit, draw_spray},
    {"objective", Lighting::Lit, draw_objective},   {"enemies", Lighting::Lit, draw_enemies},
    {"player", Lighting::Lit, draw_player},         {"arrows", Lighting::Lit, draw_arrows},
    {"foreground", Lighting::Lit, draw_foreground}, {"light", Lighting::Light, draw_light},
    // Nothing after the light pass: the UI drawn after it lives in
    // render/overlay.cpp, outside the world and outside this table.
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
              "Lit meaningless and two of them makes it ambiguous");

// Every Lit layer before the light pass, which is what Lit means: the pass adds
// light to everything already drawn, so a Lit layer after it would be drawn
// unlit while declaring otherwise.
constexpr bool lighting_matches_order() {
    bool past_light = false;
    for (int i = 0; i < TABLE_COUNT; ++i) {
        if (TABLE[i].lighting == Lighting::Light)
            past_light = true;
        else if (past_light)
            return false;
    }
    return true;
}
static_assert(lighting_matches_order(),
              "a Lit layer sits after the light pass in the table, so it is drawn "
              "unlit while declaring that it is lit");

} // namespace

const Layer* const LAYERS = TABLE;
const int LAYER_COUNT = TABLE_COUNT;

void compose(gfx::Canvas& canvas, const Params& p) {
    for (int i = 0; i < TABLE_COUNT; ++i) TABLE[i].draw(canvas, p);
}

} // namespace frame
