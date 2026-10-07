// The golden frame.
//
// The frame is composed by a run of draw calls whose order is the whole feature,
// and every other suite in this project passes on a frame nobody can see
// through. This is the assertion: one fixed scene, composed by the real code,
// hashed.
//
// It drives frame::compose, not a copy of it. A parallel software compositor
// that reimplements the layer order passes happily while the shipped renderer is
// broken, because the two are different programs. So the only thing this file
// builds is the inputs: a grid, a camera, a handful of textures. The ordering under test
// is read out of src/render/frame.cpp.
//
// SDL_CreateSoftwareRenderer, because a GPU frame is not reproducible. Drivers
// differ in filtering, in rounding and in how they clip a float rect, so an
// accelerated checksum would fail on a second machine and mean nothing. The cost
// is stated plainly: this checks the software rasterisation of the real draw
// calls, and cannot see a defect that exists only on the GPU path. What it does
// see is every change to which layer is drawn, in what order, at what position,
// at what size.
//
// The textures are generated, not loaded from assets/. A checksum over the
// shipped backdrop art would fail the moment anyone redraws a mountain, and
// would make this suite depend on the build having staged assets. The patterns
// below are chosen only to be busy enough that a misplaced layer moves the hash.
//
// The include below must come before SDL.h, and it is load-bearing on Windows:
// SDL otherwise #defines `main` to `SDL_main` and expects SDL2main to supply the
// real one. This is a console program with a plain main() and does not link
// SDL2main, so without it the link fails on an unresolved `main`.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include "game/camera.h"
#include "physics/grid.h"
#include "render/depth_rig.h"
#include "render/frame.h"
#include "game/display.h"
#include "render/light.h"
#include "render/overlay.h"
#include "test_util.h"

namespace {

// The viewport, in cells. Small on purpose -- large enough to hold terrain, a
// fire, a prop and the body at once, small enough that the whole frame is a
// modest surface. padded_h is deliberately not a multiple of LightField::BLOCK,
// so the light texture's block extent genuinely overhangs the viewport, which is
// the alignment case the light pass's destination rect exists for.
constexpr int PADDED_W = 120;
constexpr int PADDED_H = 68;

constexpr int WORLD_W = 400;
constexpr int WORLD_H = 200;

// A fractional camera centre, so frac_x()/frac_y() are both non-zero and every
// sub-cell offset in the composition is actually exercised. A whole-cell camera
// would hash identically with all of them dropped.
constexpr float CAM_X = 183.37f;
constexpr float CAM_Y = 96.62f;

// FNV-1a over the whole surface. Not cryptographic and does not need to be --
// what it has to do is change when any pixel does.
uint64_t hash_surface(SDL_Surface* surf) {
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < surf->h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(surf->pixels) + y * surf->pitch;
        for (int i = 0; i < surf->w * 4; ++i) {
            h ^= row[i];
            h *= 1099511628211ull;
        }
    }
    return h;
}

// --- measuring a frame, as opposed to fingerprinting it --------------------
//
// A hash says "this frame is not the frame I expected" and nothing more; it is
// deliberately blind to how a frame differs, which is what makes it a good
// regression guard and a useless one for a claim like "the multiply darkens".
// These two say how much light is on screen.
//
// Rec. 709 weights. Doubles rather than integers because the whole point is a
// ratio between two frames, and the surface is BGRA in memory.
double luminance_at(const uint8_t* px) {
    return 0.2126 * px[2] + 0.7152 * px[1] + 0.0722 * px[0];
}

double mean_luminance(SDL_Surface* surf) {
    double total = 0.0;
    for (int y = 0; y < surf->h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(surf->pixels) + y * surf->pitch;
        for (int x = 0; x < surf->w; ++x) total += luminance_at(row + x * 4);
    }
    return total / (static_cast<double>(surf->w) * surf->h);
}

double max_luminance(SDL_Surface* surf) {
    double peak = 0.0;
    for (int y = 0; y < surf->h; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(surf->pixels) + y * surf->pitch;
        for (int x = 0; x < surf->w; ++x) {
            const double l = luminance_at(row + x * 4);
            if (l > peak) peak = l;
        }
    }
    return peak;
}

// A static texture filled from a generated pattern. `f` returns ARGB8888 for a
// pixel. Blend mode matches load_art_texture's, which is what the game gives
// every one of these.
SDL_Texture* pattern_texture(SDL_Renderer* r, int w, int h,
                             uint32_t (*f)(int x, int y)) {
    SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) return nullptr;
    std::vector<uint32_t> buf(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) buf[static_cast<size_t>(y) * w + x] = f(x, y);
    SDL_UpdateTexture(tex, nullptr, buf.data(), w * static_cast<int>(sizeof(uint32_t)));
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

uint32_t sky_pattern(int x, int y) {
    return 0xFF000000u | (static_cast<uint32_t>(0x14 + (y / 7)) << 16) |
           (static_cast<uint32_t>(0x10 + (x / 23)) << 8) | 0x22u;
}

// Transparent above a jagged ridge, opaque below it -- the silhouette a
// parallaxed mountain layer actually has, so a wrong y offset shows up in the
// hash rather than being hidden by a full-rect fill.
uint32_t mountain_pattern(int x, int y) {
    const int ridge = 60 + ((x * 7) % 23);
    return y < ridge ? 0x00000000u : 0xFF2B2438u;
}

// The backdrop's art rows. The fixture's backdrop is world-sized, 400x200, the
// way a shipped set is, and its rig puts the horizon and the contact row where
// the fixture's terrain is.
constexpr int HORIZON = 110;
constexpr int CONTACT = 150;

// The ground plane: transparent above the horizon and solid from it down, as
// backdrop_set_test requires of a shipped plane. A vertical ramp with sparse
// marks, because both are what the per-row loop transforms -- a flat fill would
// hash identically with every row's source wrong.
uint32_t plane_pattern(int x, int y) {
    if (y < HORIZON) return 0x00000000u;
    const uint32_t ramp = static_cast<uint32_t>(0x18 + (y - HORIZON) / 3);
    if ((x * 5 + y * 13) % 97 < 6) return 0xFF3C3452u;  // a mark
    return 0xFF000000u | (ramp << 16) | ((ramp - 4) << 8) | (ramp + 26);
}

// Sparse reeds, for the foreground pass. Painted higher than reeds would stand
// because the fixture's camera is well above the standing anchor, and at 1.3 the
// foreground rises out of the window fastest of all.
uint32_t reeds_pattern(int x, int y) {
    return (x * 7) % 31 < 3 && y > 70 + (x % 11) && y < 110 ? 0xFF1E2A18u : 0x00000000u;
}

// A painted surface for the banded fixture: transparent sky, then three runs
// of rows with marks, so a band at the wrong factor or the wrong rows moves the
// hash.
uint32_t banded_pattern(int x, int y) {
    if (y < 90) return 0x00000000u;
    if ((x * 3 + y * 7) % 41 < 4) return 0xFF52443Cu;
    return 0xFF000000u | (static_cast<uint32_t>(0x30 + y / 4) << 8) | 0x2Au;
}

uint32_t prop_pattern(int x, int y) {
    return (x + y) % 5 == 0 ? 0x00000000u : 0xFF1C3320u;
}

uint32_t player_pattern(int x, int y) {
    return (x * 3 + y) % 7 == 0 ? 0x00000000u : 0xFF6E7C99u;
}

// The scene. Deterministic by construction -- a fixed seed and hand-placed
// cells, no update() at all, because what is under test is the composition and a
// stepped world would make this suite fail whenever the simulation legitimately
// changed.
void stamp_world(Grid& grid) {
    for (int x = 0; x < WORLD_W; ++x) {
        const int surface = 120 + (x % 17) - (x / 40);
        for (int y = surface; y < WORLD_H; ++y)
            grid.set_element(x, y, y < surface + 6 ? ElementType::Sand : ElementType::Wall);
    }
    // Water in a dip, and a fire so the light pass has something to do -- an
    // unlit frame skips the light draw entirely, so a checksum taken without one
    // would not cover it.
    for (int x = 150; x < 190; ++x)
        for (int y = 112; y < 120; ++y) grid.set_element(x, y, ElementType::Water);
    for (int x = 205; x < 209; ++x)
        for (int y = 104; y < 108; ++y) grid.set_element(x, y, ElementType::Fire);
}

} // namespace

int main() {
    // No SDL_Init(SDL_INIT_VIDEO): a software renderer over a surface needs no
    // display, which is what lets this run headless on a build machine.
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(
        0, PADDED_W * Camera::DEFAULT_SCALE, PADDED_H * Camera::DEFAULT_SCALE, 32,
        SDL_PIXELFORMAT_ARGB8888);
    check("surface created", surface != nullptr, SDL_GetError());
    if (!surface) return report();

    SDL_Renderer* renderer = SDL_CreateSoftwareRenderer(surface);
    check("software renderer created", renderer != nullptr, SDL_GetError());
    if (!renderer) return report();

    Grid grid(WORLD_W, WORLD_H, 1234567u);
    stamp_world(grid);

    Camera camera;
    // The shipped framing, not the default. A checksum over a configuration the
    // game never runs covers nothing that ships -- the same lesson the null-texture
    // note above records. Every parallax origin is a function of the view, so the
    // framing moves all of them.
    camera.follow(CAM_X, CAM_Y, PADDED_W, PADDED_H, WORLD_W, WORLD_H);

    // The cell texture, uploaded exactly the way main.cpp uploads it -- the visible
    // rect out of the grid's own pixel buffer, at the grid's pitch.
    SDL_Texture* cells = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, PADDED_W, PADDED_H);
    check("cell texture created", cells != nullptr, SDL_GetError());
    SDL_SetTextureBlendMode(cells, SDL_BLENDMODE_BLEND);
    {
        const std::vector<uint32_t>& pixels = grid.get_pixels();
        const SDL_Rect visible{0, 0, PADDED_W, PADDED_H};
        const uint32_t* src = pixels.data() + camera.view_y() * WORLD_W + camera.view_x();
        SDL_UpdateTexture(cells, &visible, src, WORLD_W * static_cast<int>(sizeof(uint32_t)));
    }

    LightField light(PADDED_W, PADDED_H);
    light.update(grid, camera.view_x(), camera.view_y());
    check("the fixture scene is lit", light.any_light(),
          "no light means the light pass is not in the checksum at all");
    SDL_Texture* light_tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               light.cols(), light.rows());
    SDL_SetTextureBlendMode(light_tex, SDL_BLENDMODE_ADD);
    SDL_SetTextureScaleMode(light_tex, SDL_ScaleModeLinear);
    SDL_UpdateTexture(light_tex, nullptr, light.pixels().data(),
                      light.cols() * static_cast<int>(sizeof(uint32_t)));

    frame::Params p;
    p.camera = &camera;
    p.padded_w = PADDED_W;
    p.padded_h = PADDED_H;
    // The backdrop: one of each kind of layer the frame draws, as a rig set
    // anchored at the standing camera -- an opaque sky at a typed factor, a
    // silhouette standing on the plane, the line-scrolled plane with rippled rows,
    // and a foreground above 1.0. time_s is 0, so the ripple is the still frame's.
    // The banded, corner-anchored model gets its own checksum further down.
    const depth_rig::Rig rig{HORIZON, CONTACT, 0.75f};
    const int bd_w = WORLD_W * camera.scale(), bd_h = WORLD_H * camera.scale();
    auto layer = [&](uint32_t (*f)(int, int), float fx) {
        frame::ParallaxLayer l;
        l.texture = pattern_texture(renderer, WORLD_W, WORLD_H, f);
        l.w = bd_w;
        l.h = bd_h;
        l.tex_h = WORLD_H;
        l.parallax_x = fx;
        l.parallax_y = depth_rig::vertical_factor(rig, fx);
        return l;
    };
    frame::Backdrop backdrop;
    p.backdrop = &backdrop;
    backdrop.rig = rig;
    backdrop.ripple_amplitude = 0.6f;
    backdrop.anchor_x = 0.5f * static_cast<float>(WORLD_W - PADDED_W);
    backdrop.anchor_y =
        depth_rig::standing_anchor_y(rig, 20, PADDED_H, Camera::VERTICAL_ANCHOR, WORLD_H);
    backdrop.layers.push_back(layer(sky_pattern, 0.04f));
    backdrop.layers.push_back(layer(mountain_pattern, depth_rig::factor_at(rig, 116.0f)));
    {
        frame::ParallaxLayer plane = layer(plane_pattern, 0.0f);
        plane.line_scroll = true;
        plane.ripple_row0 = HORIZON + 4;
        plane.ripple_row1 = HORIZON + 20;
        backdrop.layers.push_back(plane);
    }
    {
        frame::ParallaxLayer reeds = layer(reeds_pattern, 1.3f);
        reeds.is_foreground = true;
        backdrop.layers.push_back(reeds);
    }
    p.cells = cells;

    SDL_Texture* prop_tex = pattern_texture(renderer, 24, 40, prop_pattern);
    const std::vector<frame::Prop> props = {
        frame::Prop{prop_tex, 24, 40, 160.0f, 121.0f},
        frame::Prop{prop_tex, 24, 40, 215.0f, 118.0f},
        // A null texture, because main.cpp's list can hold one and the skip is a
        // branch in the code under test.
        frame::Prop{nullptr, 24, 40, 240.0f, 118.0f},
    };
    p.props = &props;

    p.has_objective = true;
    p.objective_x = 230;
    p.objective_y = 108;

    p.player_tex = pattern_texture(renderer, 56, 52, player_pattern);  // 4 cols x 2 rows
    p.player_x = 181.4f;
    p.player_y = 100.8f;
    p.facing_left = true;
    p.sheet_col = 2;
    p.sheet_row = 1;
    p.player_box_w = 8;
    p.player_box_h = 20;

    p.light = &light;
    p.light_texture = light_tex;

    check("every fixture texture created",
          backdrop.layers[0].texture && backdrop.layers[1].texture && backdrop.layers[2].texture &&
              backdrop.layers[3].texture && prop_tex && p.player_tex && light_tex,
          SDL_GetError());

    // The layer table's shape, asserted here rather than only in the compiler.
    // frame.cpp holds the Lit/Light/Unlit invariant with static_asserts, which is
    // the stronger guard. What that cannot catch is the table silently becoming
    // empty or losing the light pass through some future edit that still compiles --
    // so the count and the one boundary entry are checked from the outside too.
    int lit = 0, graded = 0, boundary = 0, unlit = 0;
    for (int i = 0; i < frame::LAYER_COUNT; ++i) {
        switch (frame::LAYERS[i].lighting) {
            case frame::Lighting::Lit: ++lit; break;
            case frame::Lighting::Grade: ++graded; break;
            case frame::Lighting::Light: ++boundary; break;
            case frame::Lighting::Unlit: ++unlit; break;
        }
    }
    check("the layer table has exactly one light pass", boundary == 1,
          "Lit and Unlit are defined against it; without it they mean nothing");
    check("at most one world-wide grade", graded <= 1,
          "two full-screen multiplies compose into a third grade nothing declares");
    check("every world layer is lit", lit == frame::LAYER_COUNT - 1 - graded && unlit == 0,
          "UI belongs in render/overlay.cpp, after the light pass - a layer arriving "
          "here "
          "unlit means the world/UI boundary has moved and B1 is reachable again");

    // The world-wide grade gets its own check rather than riding on the golden
    // number. Params::world_grade defaults to identity and draw_grade returns before
    // its first draw call when it is, so the fixture below composes with the quad
    // skipped -- the checksum covers the per-layer half of the multiply and not the
    // world-wide half.
    //
    // So: compose once more with a non-identity grade and require the frame to move.
    // Without it, draw_grade could early-return unconditionally and every check in
    // this file would still pass.
    check("the grade pass is skipped at identity", p.world_grade.identity(),
          "the fixture is supposed to compose without the quad, so that the golden "
          "checksum keeps meaning what it meant before step 3");

    frame::compose(renderer, p);
    const uint64_t first = hash_surface(surface);

    // The composed frame's checksum.
    //
    // Updating this number is a legitimate thing to do and is exactly what the suite
    // is for. It must be updated in the same commit as the change that moved it, so
    // that `git log -S` on this line lists every frame the renderer has ever
    // produced; updating it in a separate tidy-up commit destroys that, which is the
    // only way to misuse this test.
    //
    // Where a change has a no-op half -- a restructure that moves lines, or a new
    // mechanism that can ship at identity first -- run that half against the old
    // checksum before the half that moves pixels, so the diff cannot hide a second
    // cause inside it. Some changes have no such half: a new band that draws pixels
    // cannot compose to the old number, and leaving its texture null to arrange that
    // covers the layer's absence rather than its position. Where the separation is
    // not available, say so and let the headless suites -- backdrop_test's rounding
    // properties, camera_test's framing in screen terms -- carry it instead.
    constexpr uint64_t GOLDEN = 0xf23f87b761b69d53ull;
    char detail[128];
    std::snprintf(detail, sizeof(detail), "got 0x%016llx, expected 0x%016llx",
                  static_cast<unsigned long long>(first),
                  static_cast<unsigned long long>(GOLDEN));
    check("the composed frame matches the golden checksum", first == GOLDEN, detail);

    // Every backdrop layer has to be in the frame for the number above to say
    // anything about it. A checksum over a layer with a null texture covers the
    // layer's absence, and the same hazard is reachable by geometry: a plane whose
    // rows land below the window, or a foreground drawn off-screen, composes
    // cleanly with every check passing.
    //
    // So each layer's presence is asserted rather than assumed, by the only
    // instrument that can: compose again without it and require a different frame.
    {
        const char* names[] = {"the sky", "the standing silhouette", "the plane", "the foreground"};
        for (size_t i = 0; i < backdrop.layers.size(); ++i) {
            SDL_Texture* real = backdrop.layers[i].texture;
            backdrop.layers[i].texture = nullptr;
            frame::compose(renderer, p);
            const uint64_t without = hash_surface(surface);
            backdrop.layers[i].texture = real;
            check((std::string(names[i]) + " actually reaches the fixture's window").c_str(),
                  without != first,
                  "removing the layer's texture changed no pixel, so the golden "
                  "checksum is not covering it");
        }
        frame::compose(renderer, p);
        check("restoring the backdrop restores the golden frame", hash_surface(surface) == first);
    }

    // The other model: a corner-anchored, vertically locked set with a banded
    // surface -- the bg1 family. Its own checksum, so a change to one model's
    // placement cannot hide inside the other's number.
    {
        frame::Backdrop corner;
        corner.layers.push_back(backdrop.layers[0]);  // the sky, at 0.04
        corner.layers[0].parallax_y = 1.0f;
        frame::ParallaxLayer ground = layer(banded_pattern, 0.0f);
        ground.parallax_y = 1.0f;
        ground.bands = {{0, 130, 0.30f}, {130, 160, 0.70f}, {160, WORLD_H, 1.00f}};
        corner.layers.push_back(ground);
        p.backdrop = &corner;
        frame::compose(renderer, p);
        const uint64_t banded = hash_surface(surface);

        constexpr uint64_t CORNER_GOLDEN = 0x4de151aa6a5cb6c8ull;
        char cd[128];
        std::snprintf(cd, sizeof(cd), "got 0x%016llx, expected 0x%016llx",
                      static_cast<unsigned long long>(banded),
                      static_cast<unsigned long long>(CORNER_GOLDEN));
        check("the corner-anchored banded frame matches its golden checksum",
              banded == CORNER_GOLDEN, cd);

        SDL_Texture* real = corner.layers[1].texture;
        corner.layers[1].texture = nullptr;
        frame::compose(renderer, p);
        check("the banded surface actually reaches the fixture's window",
              hash_surface(surface) != banded);
        SDL_DestroyTexture(real);
        p.backdrop = &backdrop;
        frame::compose(renderer, p);
        check("restoring the rig backdrop restores the golden frame",
              hash_surface(surface) == first);
    }

    // Composing the same inputs twice must give the same frame. Not redundant with
    // the check above: it separates "the renderer changed" from "this suite is
    // reading uninitialised memory", and only one of those is worth anybody's
    // afternoon.
    frame::compose(renderer, p);
    check("composition is repeatable", hash_surface(surface) == first,
          "the same inputs produced two different frames");

    // And the checksum has to be sensitive, or a passing golden test is worth
    // nothing. One cell of camera movement is the smallest change the composition
    // can be asked to notice, and it moves every layer at once -- each by a
    // different amount, which is what the parallax factors are.
    camera.follow(CAM_X + 1.0f, CAM_Y, PADDED_W, PADDED_H, WORLD_W, WORLD_H);
    frame::compose(renderer, p);
    check("the checksum moves when the frame does", hash_surface(surface) != first,
          "a one-cell camera move left the frame identical - the checksum is not "
          "watching what it claims to");

    // --- the grade pass has to actually darken ----------------
    //
    // Two claims, and neither is covered by anything above. world_grade is identity
    // in the fixture, so draw_grade returns immediately and the golden checksum has
    // never seen the quad; an unconditional early return would pass every other
    // check in this file.
    //
    // Measured as mean luminance rather than as a checksum, because "the frame
    // changed" is not the claim -- the claim is that it got darker, which is the
    // whole of what an additive pass cannot do. A hash cannot tell those apart, and
    // a grade that brightened would satisfy one and not the other.
    camera.follow(CAM_X, CAM_Y, PADDED_W, PADDED_H, WORLD_W, WORLD_H);
    frame::compose(renderer, p);
    const double plain_mean = mean_luminance(surface);

    p.world_grade = frame::Grade{128, 128, 128};
    frame::compose(renderer, p);
    const double graded_mean = mean_luminance(surface);

    char graded_detail[192];
    std::snprintf(graded_detail, sizeof(graded_detail),
                  "mean luminance %.2f ungraded, %.2f at half grade",
                  plain_mean, graded_mean);
    check("a half grade darkens the composed frame", graded_mean < plain_mean * 0.75,
          graded_detail);

    // And the ordering, as a number rather than as a comment.
    //
    // Comparing peak luminance cannot do the job: the fixture's fire already
    // saturates ungraded, so the number a correct ordering produces and the number a
    // reversed one produces are both squeezed against the clip and land close
    // together. "The brightest pixel stays bright" is a true statement about the
    // design and a bad instrument for it.
    //
    // What discriminates cleanly is the light pass's contribution: compose with and
    // without it and subtract. Drawn before the light, as declared, the grade cannot
    // touch that difference and it comes out the same in both frames. Drawn after
    // it, the difference is multiplied along with everything else and comes out at
    // half. The threshold sits between the two, so it separates the two orderings
    // and not two tunings.
    //
    // The static_assert in frame.cpp catches a reversed table first, so this is not
    // the primary guard. It is the guard on the case the compiler cannot see:
    // rank() and the table edited together, which is exactly what someone would do
    // to "fix the build" after moving a row.
    const LightField* saved_light = p.light;

    p.light = nullptr;  // draw_light returns before drawing
    frame::compose(renderer, p);
    const double graded_unlit = mean_luminance(surface);
    p.world_grade = frame::Grade{};
    frame::compose(renderer, p);
    const double plain_unlit = mean_luminance(surface);
    p.light = saved_light;

    const double plain_contribution = plain_mean - plain_unlit;
    const double graded_contribution = graded_mean - graded_unlit;

    std::snprintf(graded_detail, sizeof(graded_detail),
                  "the light pass adds %.3f ungraded and %.3f at half grade; drawn "
                  "after the grade instead it would add about %.3f",
                  plain_contribution, graded_contribution, plain_contribution * 0.5);
    check("the light pass carries a fixture the grade can be measured against",
          plain_contribution > 0.05, graded_detail);
    check("the light pass is not dimmed by the grade",
          graded_contribution > plain_contribution * 0.8, graded_detail);

    // --- the screen-space layer -----------------------------------
    //
    // A second checksum, not a change to the one above. The composition and the
    // overlay are two calls with a hard boundary between them -- everything in the
    // world gets lit and everything after it must not -- and one number over both
    // would make a change to the HUD indistinguishable from a change to the sky.
    // GOLDEN stays a hash of frame::compose alone and is taken before this section
    // runs; OVERLAY_GOLDEN is the hash after overlay::draw has run on top of that
    // same frame, so the step between them is the UI and nothing else.
    //
    // The state must be back at the golden configuration first, or the second number
    // is a hash of the sensitivity checks above rather than of the shipped frame;
    // the check immediately below is what says it is.
    camera.follow(CAM_X, CAM_Y, PADDED_W, PADDED_H, WORLD_W, WORLD_H);
    frame::compose(renderer, p);
    check("the world is back at the golden frame before the overlay is drawn",
          hash_surface(surface) == first,
          "the checks above left state behind, so the overlay checksum would be "
          "taken over a frame the game never composes");

    // Every field set to something the game actually produces, and every optional
    // block switched on, so one checksum covers all four of them. The per-block
    // sensitivity checks after it are what say that is true rather than assumed.
    overlay::Params op;
    op.window_w = PADDED_W * Camera::DEFAULT_SCALE;
    op.window_h = PADDED_H * Camera::DEFAULT_SCALE;
    // Not this surface's own derived UI scale, which would be 1. No shipped mode
    // produces a UI scale of 1, and a checksum over a scale that never ships covers
    // nothing that ships. Every glyph, backing rect and gap in the overlay is a
    // multiple of this, so it is the single number that decides whether the layout
    // arithmetic is exercised at all.
    op.ui_scale = 2;

    op.show_reticle = true;
    op.mouse_x = op.window_w / 2 + 37;  // off both centre lines, so the four
    op.mouse_y = op.window_h / 2 - 21;  // arms land on four distinct rects
    op.in_range = true;

    op.hud_text = "HP:100  GOAL:740E  FPS:60 BRUSH:SAND(3) CHUNKS:12+FALLING";
    op.hud_lines.push_back({"SAVED session.rec  1200 FRAMES", 0xFFFFC080});
    op.hud_lines.push_back({"PAUSED  STEP", 0xFF80D0FF});
    op.hud_lines.push_back({"(183, 96) SAND  T:0", 0xFFE0E0E0});
    op.hotbar_selected = 1;

    op.run_over = true;
    op.won = false;

    // One unavailable mode, so the greyed branch and its suffix are inside the
    // number; the cursor sits on a mode rather than on Quit, and the mode in use is
    // a different one, so the two markers are on separate rows and cannot be
    // confused for each other.
    bool available[DISPLAY_MODE_COUNT];
    for (int i = 0; i < DISPLAY_MODE_COUNT; ++i) available[i] = true;
    available[DISPLAY_MODE_COUNT - 1] = false;

    op.settings_open = true;
    op.cursor = 1;
    op.mode_count = DISPLAY_MODE_COUNT;
    op.current_mode = 0;
    op.modes = DISPLAY_MODES;
    op.available = available;
    op.notice = "MODE CHANGED, BUT SETTINGS.TXT COULD NOT BE WRITTEN.";

    overlay::draw(renderer, op);
    const uint64_t with_overlay = hash_surface(surface);

    // The overlay checksum. Same rule as GOLDEN above: update it in the same commit
    // as the change that moved it, so `git log -S` on this line lists every UI the
    // game has ever drawn.
    //
    // It is taken over the composed frame with the UI drawn on top, so every world
    // pixel a camera change moves is inside it. That is the expected coupling and
    // not a defect, but it is why the two numbers must always be reasoned about in
    // order: a moved OVERLAY_GOLDEN beside an unmoved GOLDEN is the only combination
    // that says the UI changed.
    constexpr uint64_t OVERLAY_GOLDEN = 0xd465cdf478e9cbd4ull;
    char odetail[128];
    std::snprintf(odetail, sizeof(odetail), "got 0x%016llx, expected 0x%016llx",
                  static_cast<unsigned long long>(with_overlay),
                  static_cast<unsigned long long>(OVERLAY_GOLDEN));
    check("the overlay matches its golden checksum", with_overlay == OVERLAY_GOLDEN,
          odetail);

    check("the overlay actually draws over the composed frame",
          with_overlay != first,
          "drawing the UI changed no pixel, so the number above is a hash of the "
          "world and covers none of it");

    // Each block, switched off on its own. The single checksum above says the four
    // blocks together produce a frame; it does not say all four are in it. A hotbar
    // drawn off-screen, a wash of zero alpha or a settings loop that never enters
    // its body would all pass it. Same instrument as the backdrop layers' presence
    // check, applied four times.
    auto without = [&](const char* name, auto&& mutate) {
        overlay::Params q = op;
        mutate(q);
        frame::compose(renderer, p);
        overlay::draw(renderer, q);
        const uint64_t h = hash_surface(surface);
        check(name, h != with_overlay,
              "switching this block off changed no pixel, so the overlay checksum "
              "is not covering it");
    };
    without("the reticle reaches the frame", [](overlay::Params& q) { q.show_reticle = false; });
    without("the HUD stack reaches the frame", [](overlay::Params& q) { q.hud_lines.clear(); });
    without("the hotbar reaches the frame", [](overlay::Params& q) { q.hotbar_selected = -1; });
    without("the run-over wash reaches the frame", [](overlay::Params& q) { q.run_over = false; });
    without("the settings screen reaches the frame",
            [](overlay::Params& q) { q.settings_open = false; });
    // The notice is the one part of the settings screen with a live timer behind it,
    // and an empty string is how an expired one arrives -- so "draws nothing when
    // empty" is a claim with a caller, not a defensive branch.
    without("the settings notice reaches the frame",
            [](overlay::Params& q) { q.notice.clear(); });
    // The greyed row is a branch, not a layer, and it is the one the fixture exists
    // to reach: a mode list where everything fits would never enter it.
    without("the unavailable-mode branch reaches the frame",
            [&](overlay::Params& q) {
                static bool all_fit[DISPLAY_MODE_COUNT];
                for (int i = 0; i < DISPLAY_MODE_COUNT; ++i) all_fit[i] = true;
                q.available = all_fit;
            });

    // Drawing the same overlay twice over the same frame must give the same pixels,
    // for the reason the composition's repeatability check exists: it separates "the
    // UI changed" from "this suite is reading uninitialised memory".
    frame::compose(renderer, p);
    overlay::draw(renderer, op);
    check("the overlay is repeatable", hash_surface(surface) == with_overlay,
          "the same inputs produced two different overlays");

    SDL_DestroyTexture(p.player_tex);
    SDL_DestroyTexture(prop_tex);
    for (frame::ParallaxLayer& l : backdrop.layers) SDL_DestroyTexture(l.texture);
    SDL_DestroyTexture(light_tex);
    SDL_DestroyTexture(cells);
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
    return report();
}
