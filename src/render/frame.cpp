#include "render/frame.h"

#include <algorithm>
#include "render/backdrop_layers.h"
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
// is flat" instead of "the window is full of noise".
void draw_clear(SDL_Renderer* renderer, const Params&, const Grade& g) {
    // Palette colour for darkest sky tone, matching tools/pixel_art.py.
    SDL_SetRenderDrawColor(renderer, graded(0x38, g.r), graded(0x2C, g.g),
                           graded(0x57, g.b), 255);
    SDL_RenderClear(renderer);
}

// The backdrop layers. Static textures, each shifted by the camera's continuous
// view position scaled by the camera scale and the layer's own parallax factor
// (Camera::parallax_origin_x/y). Full-texture draws with a negative destination
// offset rather than a cropped source rect: the art is static, so there is
// nothing to re-upload per frame, only where it is drawn needs to move.
//
// Two layouts, and they agree wherever the shipped art is concerned:
//
//   tools/generate_backdrop.py sizes a pan-sized layer as
//   window + pan_range * factor, where pan_range = (world_w - padded_w) * scale.
//   So w - window_w is pan_range * factor, and the fixed branch's u_x * span_x
//   expands to view_fx * scale * factor -- which is Camera::parallax_origin_x
//   exactly. The normalized form is the same pan, stated as a fraction of the
//   world instead of as a factor on the camera.
//
// They stop agreeing where the factor form is wrong: a layer that is not sized
// to the pan range. The factor form keeps sliding and runs off the end of the
// image, leaving the clear colour at the pan limit; the normalized form maps
// whatever width the image has across the whole pan, so both edges stay flush
// whatever the art happens to be. That is what makes a large authored fixed
// scene possible without re-deriving a factor per image.
//
// The infinite branch cannot be stated as a fraction at all, because there is no
// maximum camera position to divide by. It keeps the factor form and tiles the
// result through backdrop_wrap::wrap_axis.
//
// `authored` and `infinite` are separate questions and must not be conflated:
//
//   - Does this layer read its parallax factors? An authored stack is nothing
//     but factors -- images of one identical size whose only difference is the
//     rate each moves at -- so it always does. A generated layer is sized to its
//     pan range instead, which is what the normalized branch exists for.
//   - Does this layer tile? That is a property of the world, not the layer: an
//     unbounded world has no right-hand edge to run out at, and a bounded one
//     does.
//
// Declaring an authored scene infinite to reach the factor branch brings the
// tiling with it, which is a seam per layer sliding at a different speed.
//
// An authored layer in a bounded world needs no tiling and leaves no gap, and
// that is arithmetic rather than luck. For a world of W cells, a viewport of V,
// and a layer W cells wide at factor f, the right edge at the rightmost camera
// position is -(W - V) * scale * f + W * scale, which covers the window exactly
// when W - (W - V) * f >= V, i.e. when f <= 1. That is why authored art is
// world-sized and why its foreground factor is capped at 1.00. test_frame pins
// the inequality at both camera extremes.
void draw_backdrop_layer(SDL_Renderer* renderer, const Params& p,
                         SDL_Texture* tex, int w, int h,
                         const backdrop_layers::Layer& layer, const Grade& g,
                         bool authored) {
    if (!tex) return;
    apply_grade(tex, g);

    const Camera& camera = *p.camera;
    const int window_w = p.padded_w * camera.scale();
    const int window_h = p.padded_h * camera.scale();

    if (authored && !p.is_infinite) {
        // Bounded and authored: the factors, placed once. No wrap call, because
        // there is nothing to wrap onto.
        const SDL_FRect dst{
            camera.parallax_origin_x(layer.parallax_x),
            camera.parallax_origin_y(layer.parallax_y),
            static_cast<float>(w), static_cast<float>(h)
        };
        SDL_RenderCopyF(renderer, tex, nullptr, &dst);
        return;
    }

    if (p.is_infinite) {
        const backdrop_wrap::Tiling t = backdrop_wrap::wrap_axis(
            camera.parallax_origin_x(layer.parallax_x), w, window_w);
        const float origin_y = camera.parallax_origin_y(layer.parallax_y);
        for (int c = 0; c < t.count; ++c) {
            const SDL_FRect dst{
                t.first + static_cast<float>(c * w), origin_y,
                static_cast<float>(w), static_cast<float>(h)
            };
            SDL_RenderCopyF(renderer, tex, nullptr, &dst);
        }
        return;
    }

    // max(1, ...) and not max(0, ...): this is a divisor, and a world no bigger than
    // the viewport has a pan range of zero. Dividing by 1 there gives u = 0 for every
    // camera position the world can reach, which is the right answer -- a world that
    // cannot scroll shows the layer's left edge.
    const float max_cam_x = static_cast<float>(std::max(1, p.world_w - p.padded_w));
    const float max_cam_y = static_cast<float>(std::max(1, p.world_h - p.padded_h));

    const float u_x = std::clamp(camera.view_fx() / max_cam_x, 0.0f, 1.0f);
    const float u_y = std::clamp(camera.view_fy() / max_cam_y, 0.0f, 1.0f);

    // A layer no larger than the window has nothing to pan across and is pinned at
    // the origin, rather than being pulled off screen by a negative span.
    const float span_x = static_cast<float>(std::max(0, w - window_w));
    const float span_y = static_cast<float>(std::max(0, h - window_h));

    const SDL_FRect dst{
        -u_x * span_x, -u_y * span_y,
        static_cast<float>(w), static_cast<float>(h)
    };
    SDL_RenderCopyF(renderer, tex, nullptr, &dst);
}

// The two authored passes, and the early return in the generated three.
//
// A scene has one backdrop system or the other, never a blend: an authored stack
// is a whole depth ladder painted together, and drawing the generated sky behind
// it would put two horizons in one frame at two unrelated factors. The switch is
// layers.empty(), tested in three places rather than hoisted into compose(),
// because the layer table is what declares what is drawn and a pass that
// silently skips itself is still a row in it -- which is what keeps the
// static_asserts below meaningful.
//
// draw_ground takes the early return too. The ground BMP is loaded once at
// startup for the whole process, not per scene, so "an authored set that wants
// no plane loads no ground BMP" is not a switch that exists; without the return
// the generated plane draws over the authored stack from the horizon down.
//
// What does not early-return is render/surface_plane.cpp's terrain tint, which
// reads the same geometry to blend near terrain toward the plane. It is a
// separate pass over the cell texture rather than a layer here, and an authored
// scene with no terrain gives it nothing to tint -- so it is inert rather than
// wrong today, and becomes wrong the day an authored backdrop sits behind a
// scene that has terrain.
//
// An authored layer that is a surface is drawn as horizontal bands, each at its
// own factor. See frame::Band.
//
// The vertical is 1:1 and the whole layer's parallax_y is used for every band,
// so the bands are one image vertically and are cut apart only in how fast they
// scroll sideways. Nothing here shrinks or stretches a source row: the authored
// stack's vertical factor is locked so the composition is the painting at every
// camera height, and a band that scaled its rows would undo that.
//
// Destination edges are computed per boundary, not per band, for the same reason
// backdrop_wrap::plane_src_row exists: band i's bottom edge and band i+1's top
// edge have to be one number evaluated once, or the rounding leaves a one-pixel
// line of whatever was behind the layer between two bands meant to touch.
void draw_authored_bands(SDL_Renderer* renderer, const Params& p,
                         const ParallaxLayer& l) {
    if (l.tex_h <= 0) return;

    int tex_w = 0;
    SDL_QueryTexture(l.texture, nullptr, nullptr, &tex_w, nullptr);
    if (tex_w <= 0) return;

    const Camera& camera = *p.camera;
    const float origin_y = camera.parallax_origin_y(l.parallax_y);
    const float rows_to_px = static_cast<float>(l.h) / static_cast<float>(l.tex_h);

    // Boundary `r` of the layer, in screen pixels, rounded once. Called for both
    // edges of every band, so band i's bottom and band i+1's top are the same
    // expression on the same argument and cannot disagree.
    const auto edge = [&](int row) {
        return std::floor(origin_y + static_cast<float>(row) * rows_to_px + 0.5f);
    };

    for (const Band& b : l.bands) {
        if (b.row1 <= b.row0) continue;
        const float top = edge(b.row0);
        const SDL_Rect src{0, b.row0, tex_w, b.row1 - b.row0};
        const SDL_FRect dst{camera.parallax_origin_x(b.parallax_x), top,
                            static_cast<float>(l.w), edge(b.row1) - top};
        SDL_RenderCopyF(renderer, l.texture, &src, &dst);
    }
}

void draw_custom_background_layers(SDL_Renderer* renderer, const Params& p, const Grade&) {
    for (const ParallaxLayer& l : p.backdrop.layers) {
        if (l.is_foreground || !l.texture) continue;
        if (!l.bands.empty()) { apply_grade(l.texture, l.grade); draw_authored_bands(renderer, p, l); continue; }
        // A per-layer spec built here rather than stored: backdrop_layers::Layer
        // is the generated table's row type and carries a generated size, which
        // an authored layer has none of. Passing its own w/h through as the size
        // keeps one draw path for both systems.
        const backdrop_layers::Layer spec{l.parallax_x, l.parallax_y, l.w, l.h};
        draw_backdrop_layer(renderer, p, l.texture, l.w, l.h, spec, l.grade, true);
    }
}

void draw_custom_foreground_layers(SDL_Renderer* renderer, const Params& p, const Grade&) {
    for (const ParallaxLayer& l : p.backdrop.layers) {
        if (!l.is_foreground || !l.texture) continue;
        if (!l.bands.empty()) { apply_grade(l.texture, l.grade); draw_authored_bands(renderer, p, l); continue; }
        const backdrop_layers::Layer spec{l.parallax_x, l.parallax_y, l.w, l.h};
        draw_backdrop_layer(renderer, p, l.texture, l.w, l.h, spec, l.grade, true);
    }
}

void draw_sky(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    if (!p.backdrop.layers.empty()) return;  // the authored stack owns the sky
    draw_backdrop_layer(renderer, p, p.backdrop.sky,
                        p.backdrop.sky_w, p.backdrop.sky_h, backdrop_layers::SKY, g, false);
}

void draw_mountains(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    if (!p.backdrop.layers.empty()) return;  // the authored stack owns the skyline
    draw_backdrop_layer(renderer, p, p.backdrop.mountains,
                        p.backdrop.mountain_w, p.backdrop.mountain_h,
                        backdrop_layers::MOUNTAINS, g, false);
}

// A mid-ground band between the mountains and the world was built here and
// removed: our world is much taller than the camera sees, so simulated terrain
// already occupies that band where a hand-painted stack cannot.
//
// What replaced it is not what came out. The deleted band was a silhouette at
// one factor; the ground plane below is a surface at a range of factors, drawn
// behind the world, and the near silhouette in front of it stays the simulated
// terrain -- deliberately, because a painted band in front of the world would
// occlude the one verb the game has.

// --- the ground plane ----------------------------------------------------
//
// A receding plane has no single depth. Drawn flat at one parallax factor it
// reads as a wall standing behind the world; drawn as N strips between two
// factors it reads as ground going away. The arithmetic -- which strip is at
// which depth, what it samples, and where the wrapping copies go -- is in
// render/backdrop_wrap.h and tested headless in tests/test_backdrop.cpp; this
// function does nothing but turn its answers into SDL_RenderCopy calls.
//
// Two constants live here rather than in the generated header, because they are
// composition and not parallax: where the band sits in the frame and how finely
// it is cut. Both are TUNING.md rows.
//
// STRIPS is a real cost knob: the plane issues STRIPS * (copies per strip) draw
// calls every frame, against one for every other backdrop layer. It is chosen as
// the point where the factor stepping between adjacent strips stops being
// visible as banding, and is not measured against a frame budget, because
// grid_bench times the simulation and cannot see a draw call at all.
constexpr int GROUND_STRIPS = 24;

// Where the plane's far edge sits is a row of the mountains BMP rather than a
// fraction of the window.
//
// A fraction of the window is the right shape of constant and the wrong space to
// state it in. The window is switchable at runtime, so a horizon in window
// pixels would sit at three different heights -- but the plane's far edge is not
// a fact about the window at all. It is where the ground meets the mountains,
// which is a fact about the mountains. Stated there it is resolution-independent
// for free and cannot contradict the art, because it is the art:
// backdrop_layers::MOUNTAINS_SKYLINE_MAX_ROW is generated from the same seeded
// walk that draws the silhouette. A horizon derived from the terrain skyline
// instead contradicts the mountains at every camera position, and the plane is
// opaque and drawn after them, so it covers the band completely.
//
// The deepest row of the skyline, not the highest, so the whole jagged edge
// stands clear above the plane and only the solid body below it is covered.
//
// mountain_h is the loaded texture's height rather than the generated one, so
// the horizon follows the art that is actually on screen. Against an absolute
// row index, a fixture with a shorter synthetic mountain texture drops the plane
// off the bottom of the window entirely.
//
// Defined below, outside the anonymous namespace: the surface_plane pass
// needs the same number and frame.h declares it.

void draw_ground(SDL_Renderer* renderer, const Params& p, const Grade& g) {
    if (!p.backdrop.layers.empty()) return;  // the authored stack owns the plane
    SDL_Texture* tex = p.backdrop.ground;
    if (!tex || p.backdrop.ground_w <= 0 || p.backdrop.ground_h <= 0) return;
    apply_grade(tex, g);

    const Camera& camera = *p.camera;
    const int window_w = p.padded_w * camera.scale();
    const int window_h = p.padded_h * camera.scale();

    // The horizon moves with the camera at the mountains' vertical factor and
    // not the plane's own. A receding plane's far edge is the most distant
    // thing in the frame -- at infinity by construction -- so its parallax
    // factor has to be the smallest in the scene, not the plane's near-edge
    // one, or it climbs past the mountains within a few hundred cells of
    // descent.
    //
    // The band runs from wherever that lands to a near edge the plane
    // decides, and not to the bottom of the window: a parallaxed far edge
    // against a near edge nailed to a window constant is a layer whose
    // nearer end is less parallaxed than its further end, which reads as the
    // tile squishing as the camera climbs. The argument is at
    // PLANE_TEXEL_SCALE in render/backdrop_wrap.h.
    const backdrop_wrap::Plane plane = backdrop_wrap::plane_geometry(
        ground_horizon_y(camera, p.backdrop.mountain_h),
        p.backdrop.ground_h,
        backdrop_layers::GROUND.parallax_x,
        backdrop_layers::GROUND_NEAR_X);

    for (int i = 0; i < GROUND_STRIPS; ++i) {
        const backdrop_wrap::Strip s = backdrop_wrap::plane_strip(plane, i, GROUND_STRIPS);
        if (s.dst_h <= 0.0f || s.src_h <= 0.0f) continue;
        // Strips above the window happen whenever the camera is low enough to
        // push the horizon off the top, which is most of the world's height.
        // Skipping them saves their tiling, for rows nobody can see.
        if (s.dst_y + s.dst_h <= 0.0f || s.dst_y >= static_cast<float>(window_h)) continue;

        const backdrop_wrap::Tiling t = backdrop_wrap::wrap_axis(
            camera.parallax_origin_x(s.factor), p.backdrop.ground_w, window_w);

        // The source rect is integer, so it rounds; the destination stays float,
        // the way every other layer's does. Rounding the destination would make
        // the plane jerk in whole pixels while the world under it scrolls
        // smoothly.
        //
        // The two rows come from plane_src_row, one boundary at a time, and not
        // from rounding this strip's start and height independently: two
        // neighbouring strips rounded in isolation do not meet, so the texture
        // repeats a row at some boundaries and skips one at others.
        //
        // A strip whose two boundaries round to the same row is one whose depth
        // range has collapsed below a single texel, which happens at the horizon
        // end where the compression is steepest. It gets one row rather than
        // being skipped: a skipped strip is a transparent gap in the destination
        // band.
        const int row0 = backdrop_wrap::plane_src_row(plane, i, GROUND_STRIPS);
        const int row1 = backdrop_wrap::plane_src_row(plane, i + 1, GROUND_STRIPS);
        SDL_Rect src{0, row0, p.backdrop.ground_w, row1 - row0 > 0 ? row1 - row0 : 1};
        if (src.y + src.h > p.backdrop.ground_h) src.h = p.backdrop.ground_h - src.y;
        if (src.h <= 0) continue;

        for (int c = 0; c < t.count; ++c) {
            const SDL_FRect dst{
                t.first + static_cast<float>(c) * p.backdrop.ground_w,
                s.dst_y, static_cast<float>(p.backdrop.ground_w), s.dst_h
            };
            SDL_RenderCopyF(renderer, tex, &src, &dst);
        }
    }

    // Below the plane's near edge, when there is a below. The plane is a fixed
    // depth of art, so a camera low enough -- or a window tall enough -- can
    // leave rows underneath it that the tile has nothing to say about.
    //
    // Filled with the tile's nearest row rather than left to the clear colour.
    // What is below the near end of a receding plane is ground nearer still, and
    // the nearest thing the art knows about is its last row, so this is the
    // plane's own near tone continued rather than a new colour or a new band.
    // The world's terrain usually covers this region, which is not a guarantee
    // at every camera position.
    if (plane.bottom_y < static_cast<float>(window_h)) {
        const backdrop_wrap::Tiling t = backdrop_wrap::wrap_axis(
            camera.parallax_origin_x(backdrop_layers::GROUND_NEAR_X),
            p.backdrop.ground_w, window_w);
        const SDL_Rect src{0, p.backdrop.ground_h - 1, p.backdrop.ground_w, 1};
        const float top = plane.bottom_y > 0.0f ? plane.bottom_y : 0.0f;
        for (int c = 0; c < t.count; ++c) {
            const SDL_FRect dst{
                t.first + static_cast<float>(c) * p.backdrop.ground_w,
                top, static_cast<float>(p.backdrop.ground_w),
                static_cast<float>(window_h) - top
            };
            SDL_RenderCopyF(renderer, tex, &src, &dst);
        }
    }
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

// The mountains are multiplied down, and it is the one graded row.
//
// Measured in luminance, the sky and the ungraded mountains sit within a couple
// of levels out of 255 of each other, with the more distant band the brighter of
// the pair -- so the two most distant bands in the frame do not separate at all.
// Graded down, the mountains read as a silhouette against the sky, which is
// where the reference gets its depth from.
//
// Darker with nearness, not lighter, because the sky is the only light in the
// frame. Daylight aerial perspective washes distant things toward the sky, and
// that instinct is the wrong one here: at night the sky is the bright thing and
// everything in front of it is a cut-out.
//
// A number here and not darker mountain art. Regenerating the BMP darker
// produces the same pixels this frame, and stops doing so the moment
// world_grade is non-identity -- baked art cannot respond to a night grade,
// where a per-layer multiply composes with it. It is also tens of megabytes of
// asset regenerated for what is a composition decision.
//
// TUNING.md carries this row. The other layers are PLAIN on purpose: the sky is
// the reference the rest is judged against, and the world's own spread is
// already the widest in the frame, so grading it would compress the one band
// that does not need help.
//
// The two custom_* rows carry PLAIN and that is not an oversight. An authored
// layer brings its own Grade with it -- the value is per layer, not per row,
// because several of them share one row -- so a grade here would multiply on top
// of the one the art was authored against and neither number would say so.
constexpr Layer TABLE[] = {
    {"clear",             Lighting::Lit,   PLAIN,           draw_clear},
    {"sky",               Lighting::Lit,   PLAIN,           draw_sky},
    {"mountains",         Lighting::Lit,   {153, 153, 153}, draw_mountains},
    {"custom_background", Lighting::Lit,   PLAIN,           draw_custom_background_layers},
    {"ground",            Lighting::Lit,   {135, 135, 135}, draw_ground},
    {"props",             Lighting::Lit,   PLAIN,           draw_props},
    {"cells",             Lighting::Lit,   PLAIN,           draw_cells},
    {"objective",         Lighting::Lit,   PLAIN,           draw_objective},
    {"player",            Lighting::Lit,   PLAIN,           draw_player},
    {"custom_foreground", Lighting::Lit,   PLAIN,           draw_custom_foreground_layers},
    {"grade",             Lighting::Grade, PLAIN,           draw_grade},
    {"light",             Lighting::Light, PLAIN,           draw_light},
    // Nothing Unlit yet, and that is not an omission: the UI drawn after the
    // light pass lives elsewhere. The value exists so that the first thing to
    // cross the boundary declares which side it is on instead of inheriting a
    // position.
    //
    // `grade` is a layer with no caller -- nothing sets Params::world_grade, so
    // it returns before its first draw call on every frame the game currently
    // composes. It ships because the per-layer half of the same mechanism is
    // live on the mountains row, which is what proves the multiply works at
    // all, and because the pass and its ordering argument are the part that is
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

float ground_horizon_y(const Camera& camera, int mountain_h) {
    return camera.parallax_origin_y(backdrop_layers::MOUNTAINS.parallax_y) +
           static_cast<float>(mountain_h) * backdrop_layers::MOUNTAINS_SKYLINE_MAX;
}

const Layer* const LAYERS = TABLE;
const int LAYER_COUNT = TABLE_COUNT;

void compose(SDL_Renderer* renderer, const Params& p) {
    for (int i = 0; i < TABLE_COUNT; ++i) TABLE[i].draw(renderer, p, TABLE[i].grade);
}

} // namespace frame
