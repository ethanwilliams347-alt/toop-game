// The GL backend's parity with the SDL one.
//
// One fixture frame -- terrain, water, a fire and its light, every kind of
// backdrop layer, props, the player flipped, a turned body sprite, spray, and the
// whole overlay -- composed by the real frame::compose and overlay::draw twice:
// once through gfx::SdlCanvas over a software renderer, the backend
// golden_frame_test checksums, and once through gpu::GlCanvas into its offscreen
// target. The two are then compared pixel by pixel.
//
// The claim is "the same frame", not "the same bytes". A GPU blends in its own
// arithmetic (SDL's software blitter divides by 255 with shifts, a GPU in float)
// and filters the light texture by its own convention at the edges, so a few
// levels of difference on some pixels is expected and is not a defect. What a
// defect looks like is a layer missing, moved a pixel, drawn in the wrong order
// or with the wrong blend -- each of which puts hundreds of pixels far out of
// tolerance. So: every channel within +-3 levels on at least 99.5% of pixels.
//
// The cells are drawn on the GL side through cells.frag (GlCanvas::blit_cells),
// with the material pack (render/cell_pack.h) uploaded beside the colour, so the
// parity checks above also hold the shader's legacy mode to a plain blit. Then
// the pack itself is checked: drawn in the raw view, one pixel per cell, every
// byte read back must be the byte CellPack wrote -- the upload, the texel order
// and the shader's reading of the layout, all at once.
//
// Then the multiply light model (R3: gpu/pass_graph.h, gpu/pipeline.h), on the
// same fixture:
//   - a neutral lit frame (ambient 1, no sun, no lights, no fire light, nothing
//     emissive) equals the unlit SDL frame within the same tolerance, so the
//     albedo -> linear -> HDR -> display round trip moves no colour;
//   - the GPU light field equals the CPU LightField it ports, block for block,
//     within a few levels after the same tone map;
//   - light raises luminance (a fire, a point light), a dim ambient lowers it,
//     and the sun's march puts what is under a slab in shadow;
//   - a legacy frame drawn after a lit one still matches SDL, so resolve_lit
//     leaves the canvas as it found it.
//
// It needs a GL 3.3 core context, so a video driver: the window is created
// hidden and never shown. Without one it returns 77, which CMakeLists.txt tells
// ctest is a skip -- an unchecked backend, reported as unchecked.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "game/camera.h"
#include "game/display.h"
#include "gpu/gl_canvas.h"
#include "physics/grid.h"
#include "render/cell_pack.h"
#include "render/depth_rig.h"
#include "render/frame.h"
#include "render/light.h"
#include "render/lit_frame.h"
#include "render/overlay.h"
#include "render/sdl_canvas.h"
#include "test_util.h"

namespace {

constexpr int SKIP = 77;

// The golden fixture's sizes: a viewport whose height is not a multiple of the
// light block, so the light texture overhangs, and a fractional camera.
constexpr int PADDED_W = 120;
constexpr int PADDED_H = 68;
constexpr int WORLD_W = 400;
constexpr int WORLD_H = 200;
constexpr float CAM_X = 183.37f;
constexpr float CAM_Y = 96.62f;
constexpr int HORIZON = 110;
constexpr int CONTACT = 150;

constexpr int TOLERANCE = 3;
constexpr double REQUIRED_SHARE = 0.995;

uint32_t sky_pattern(int x, int y) {
    return 0xFF000000u | (static_cast<uint32_t>(0x14 + (y / 7)) << 16) |
           (static_cast<uint32_t>(0x10 + (x / 23)) << 8) | 0x22u;
}
uint32_t mountain_pattern(int x, int y) {
    const int ridge = 60 + ((x * 7) % 23);
    return y < ridge ? 0x00000000u : 0xFF2B2438u;
}
uint32_t plane_pattern(int x, int y) {
    if (y < HORIZON) return 0x00000000u;
    const uint32_t ramp = static_cast<uint32_t>(0x18 + (y - HORIZON) / 3);
    if ((x * 5 + y * 13) % 97 < 6) return 0xFF3C3452u;
    return 0xFF000000u | (ramp << 16) | ((ramp - 4) << 8) | (ramp + 26);
}
uint32_t reeds_pattern(int x, int y) {
    return (x * 7) % 31 < 3 && y > 70 + (x % 11) && y < 110 ? 0xFF1E2A18u : 0x00000000u;
}
uint32_t prop_pattern(int x, int y) { return (x + y) % 5 == 0 ? 0x00000000u : 0xFF1C3320u; }
uint32_t player_pattern(int x, int y) { return (x * 3 + y) % 7 == 0 ? 0x00000000u : 0xFF6E7C99u; }
// A half-transparent edge, so the turned body's alpha blend is in the frame.
uint32_t body_pattern(int x, int y) {
    return (x == 0 || y == 0) ? 0x80C08040u
                              : 0xFF000000u | static_cast<uint32_t>(x * 9 << 8) | 0x60u;
}

void stamp_world(Grid& grid) {
    for (int x = 0; x < WORLD_W; ++x) {
        const int surface = 120 + (x % 17) - (x / 40);
        for (int y = surface; y < WORLD_H; ++y)
            grid.set_element(x, y, y < surface + 6 ? ElementType::Sand : ElementType::Wall);
    }
    for (int x = 150; x < 190; ++x)
        for (int y = 112; y < 120; ++y) grid.set_element(x, y, ElementType::Water);
    for (int x = 205; x < 209; ++x)
        for (int y = 104; y < 108; ++y) grid.set_element(x, y, ElementType::Fire);
}

gfx::Texture pattern_texture(gfx::Canvas& canvas, int w, int h, uint32_t (*f)(int x, int y)) {
    const gfx::Texture tex = canvas.create_texture(w, h, gfx::Access::Static, gfx::Blend::Alpha);
    if (!tex) return tex;
    std::vector<uint32_t> buf(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) buf[static_cast<size_t>(y) * w + x] = f(x, y);
    canvas.update_texture(tex, nullptr, buf.data(), w * static_cast<int>(sizeof(uint32_t)));
    return tex;
}

// Everything one canvas needs to draw the fixture: its own textures, and the
// records pointing at them. Built once per backend from the same world, so the
// only difference between the two frames is the canvas.
struct Scene {
    std::vector<gfx::Texture> owned;
    frame::Backdrop backdrop;
    std::vector<frame::Prop> props;
    std::vector<frame::BodySprite> bodies;
    std::vector<frame::SprayDrop> spray;
    frame::Params p;
    overlay::Params op;
    bool available[DISPLAY_MODE_COUNT] = {};

    gfx::Texture keep(gfx::Texture t) {
        owned.push_back(t);
        return t;
    }

    bool build(gfx::Canvas& canvas, const Grid& grid, const Camera& camera,
               const LightField& light) {
        gfx::Texture cells = keep(
            canvas.create_texture(PADDED_W, PADDED_H, gfx::Access::Streaming, gfx::Blend::Alpha));
        const std::vector<uint32_t>& pixels = grid.get_pixels();
        const gfx::Rect visible{0, 0, PADDED_W, PADDED_H};
        canvas.update_texture(cells, &visible,
                              pixels.data() + camera.view_y() * WORLD_W + camera.view_x(),
                              WORLD_W * static_cast<int>(sizeof(uint32_t)));
        gfx::Texture light_tex =
            keep(canvas.create_texture(light.cols(), light.rows(), gfx::Access::Streaming,
                                       gfx::Blend::Add, gfx::Filter::Linear));
        canvas.update_texture(light_tex, nullptr, light.pixels().data(),
                              light.cols() * static_cast<int>(sizeof(uint32_t)));

        p.camera = &camera;
        p.padded_w = PADDED_W;
        p.padded_h = PADDED_H;
        p.cells = cells;
        p.light = &light;
        p.light_texture = light_tex;

        const depth_rig::Rig rig{HORIZON, CONTACT, 0.75f};
        auto layer = [&](uint32_t (*f)(int, int), float fx) {
            frame::ParallaxLayer l;
            l.texture = keep(pattern_texture(canvas, WORLD_W, WORLD_H, f));
            l.w = WORLD_W * camera.scale();
            l.h = WORLD_H * camera.scale();
            l.tex_h = WORLD_H;
            l.parallax_x = fx;
            l.parallax_y = depth_rig::vertical_factor(rig, fx);
            return l;
        };
        backdrop.rig = rig;
        backdrop.ripple_amplitude = 0.6f;
        backdrop.anchor_x = 0.5f * static_cast<float>(WORLD_W - PADDED_W);
        backdrop.anchor_y =
            depth_rig::standing_anchor_y(rig, 20, PADDED_H, Camera::VERTICAL_ANCHOR, WORLD_H);
        backdrop.layers.push_back(layer(sky_pattern, 0.04f));
        backdrop.layers.push_back(layer(mountain_pattern, depth_rig::factor_at(rig, 116.0f)));
        frame::ParallaxLayer plane = layer(plane_pattern, 0.0f);
        plane.line_scroll = true;
        plane.ripple_row0 = HORIZON + 4;
        plane.ripple_row1 = HORIZON + 20;
        backdrop.layers.push_back(plane);
        frame::ParallaxLayer reeds = layer(reeds_pattern, 1.3f);
        reeds.is_foreground = true;
        backdrop.layers.push_back(reeds);
        p.backdrop = &backdrop;

        gfx::Texture prop_tex = keep(pattern_texture(canvas, 24, 40, prop_pattern));
        props = {frame::Prop{prop_tex, 24, 40, 160.0f, 121.0f},
                 frame::Prop{prop_tex, 24, 40, 215.0f, 118.0f}};
        p.props = &props;

        p.body_atlas = keep(pattern_texture(canvas, 16, 16, body_pattern));
        bodies = {frame::BodySprite{gfx::Rect{0, 0, 10, 6}, 195.3f, 110.6f, 5.0f, 3.0f, 27.0f}};
        p.bodies = &bodies;
        spray = {frame::SprayDrop{170.4f, 108.2f, 0xA0E0F0FFu},
                 frame::SprayDrop{172.9f, 106.7f, 0x60E0F0FFu}};
        p.spray = &spray;

        p.has_objective = true;
        p.objective_x = 230;
        p.objective_y = 108;

        p.player_tex = keep(pattern_texture(canvas, 56, 52, player_pattern));
        p.player_x = 181.4f;
        p.player_y = 100.8f;
        p.facing_left = true;
        p.sheet_col = 2;
        p.sheet_row = 1;
        p.player_box_w = 8;
        p.player_box_h = 20;

        op.window_w = PADDED_W * Camera::DEFAULT_SCALE;
        op.window_h = PADDED_H * Camera::DEFAULT_SCALE;
        op.ui_scale = 2;
        op.show_reticle = true;
        op.mouse_x = op.window_w / 2 + 37;
        op.mouse_y = op.window_h / 2 - 21;
        op.in_range = true;
        op.hud_text = "HP:100  GOAL:740E  FPS:60 BRUSH:SAND(3)";
        op.hud_lines.push_back({"PAUSED  STEP", 0xFF80D0FF});
        op.hotbar_selected = 1;
        op.run_over = true;
        for (bool& a : available) a = true;
        available[DISPLAY_MODE_COUNT - 1] = false;
        op.settings_open = true;
        op.cursor = 1;
        op.mode_count = DISPLAY_MODE_COUNT;
        op.current_mode = 0;
        op.modes = DISPLAY_MODES;
        op.available = available;
        op.notice = "MODE CHANGED";

        for (const gfx::Texture& t : owned)
            if (!t) return false;
        return true;
    }

    void draw(gfx::Canvas& canvas, bool with_overlay) {
        frame::compose(canvas, p);
        if (with_overlay) overlay::draw(canvas, op);
    }

    void release(gfx::Canvas& canvas) {
        for (gfx::Texture& t : owned) canvas.destroy_texture(t);
        owned.clear();
    }
};

struct Diff {
    double share_within = 0.0;  // fraction of pixels with every channel within TOLERANCE
    int worst = 0;              // largest channel difference anywhere
    int first_x = -1, first_y = -1;
};

Diff compare(const SDL_Surface* sdl, const std::vector<uint32_t>& gl) {
    Diff d;
    long within = 0;
    for (int y = 0; y < sdl->h; ++y) {
        const auto* row =
            reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(sdl->pixels) +
                                              y * sdl->pitch);  // NOLINT(*-reinterpret-cast)
        for (int x = 0; x < sdl->w; ++x) {
            const uint32_t a = row[x], b = gl[static_cast<size_t>(y) * sdl->w + x];
            int worst_here = 0;
            for (int shift = 0; shift < 32; shift += 8) {
                const int ca = static_cast<int>((a >> shift) & 0xFF);
                const int cb = static_cast<int>((b >> shift) & 0xFF);
                worst_here = std::max(worst_here, std::abs(ca - cb));
            }
            if (worst_here <= TOLERANCE) {
                ++within;
            } else if (d.first_x < 0) {
                d.first_x = x;
                d.first_y = y;
            }
            d.worst = std::max(d.worst, worst_here);
        }
    }
    d.share_within = static_cast<double>(within) / (static_cast<double>(sdl->w) * sdl->h);
    return d;
}

// Mean luma of a rect of an ARGB frame, 0..255.
double luma(const std::vector<uint32_t>& px, int stride, int x0, int y0, int w, int h) {
    double sum = 0.0;
    for (int y = y0; y < y0 + h; ++y)
        for (int x = x0; x < x0 + w; ++x) {
            const uint32_t c = px[static_cast<size_t>(y) * stride + x];
            sum += 0.299 * ((c >> 16) & 0xFF) + 0.587 * ((c >> 8) & 0xFF) + 0.114 * (c & 0xFF);
        }
    return sum / (static_cast<double>(w) * h);
}

// A lit frame for the fixture's camera, as main.cpp builds one: neutral unless
// the caller changes it.
lit::LitFrame neutral_frame(const Camera& camera) {
    lit::LitFrame f;
    f.fire_gain = 0.0f;
    f.origin_x = camera.view_x();
    f.origin_y = camera.view_y();
    f.screen_x = -camera.frac_x() * static_cast<float>(camera.scale());
    f.screen_y = -camera.frac_y() * static_cast<float>(camera.scale());
    f.scale = camera.scale();
    return f;
}

void check_parity(const char* name, const SDL_Surface* sdl, const std::vector<uint32_t>& gl) {
    const Diff d = compare(sdl, gl);
    char detail[160];
    std::snprintf(detail, sizeof(detail),
                  "%.3f%% of pixels within +-%d (need %.1f%%), worst %d, first miss at (%d, %d)",
                  100.0 * d.share_within, TOLERANCE, 100.0 * REQUIRED_SHARE, d.worst, d.first_x,
                  d.first_y);
    std::printf("  %s: %s\n", name, detail);
    check(name, d.share_within >= REQUIRED_SHARE, detail);
}

} // namespace

int main() {
    const int W = PADDED_W * Camera::DEFAULT_SCALE, H = PADDED_H * Camera::DEFAULT_SCALE;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::printf("SKIP: no video subsystem (%s)\n", SDL_GetError());
        return SKIP;
    }
    gpu::GlCanvas::set_window_attributes();
    SDL_Window* window =
        SDL_CreateWindow("gl_frame_test", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, W, H,
                         SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::printf("SKIP: no GL window (%s)\n", SDL_GetError());
        SDL_Quit();
        return SKIP;
    }
    std::string why;
    std::unique_ptr<gpu::GlCanvas> gl = gpu::GlCanvas::create(window, "assets/shaders", false, why);
    if (!gl) {
        // A context that exists but whose shaders do not compile is a failure,
        // not a skip -- that is this code's fault, not the machine's.
        const bool shader_fault = why.find("shaders/") != std::string::npos;
        std::printf("%s: %s\n", shader_fault ? "FAIL" : "SKIP", why.c_str());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return shader_fault ? 1 : SKIP;
    }
    std::printf("GL: %s\n", gl->describe().c_str());
    check("the GL target is the window's size", gl->target_w() == W && gl->target_h() == H);

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, W, H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    check("software renderer created", renderer != nullptr, SDL_GetError());
    if (!renderer) return report();
    {
        gfx::SdlCanvas sdl(renderer);

        Grid grid(WORLD_W, WORLD_H, 1234567u);
        stamp_world(grid);
        Camera camera;
        camera.follow(CAM_X, CAM_Y, PADDED_W, PADDED_H, WORLD_W, WORLD_H);
        LightField light(PADDED_W, PADDED_H);
        light.update(grid, camera.view_x(), camera.view_y());
        check("the fixture scene is lit", light.any_light());

        Scene on_sdl, on_gl;
        check("every SDL fixture texture created", on_sdl.build(sdl, grid, camera, light),
              sdl.error());
        check("every GL fixture texture created", on_gl.build(*gl, grid, camera, light),
              gl->error());

        // The material pack for the same window, with two loose-body cells hidden
        // in it -- one in water, one in sand -- so the raw check below sees the
        // HIDDEN and OCCLUDER flags and a liquid's G as well as the fire's ticks.
        // (The fixture's terrain sits below this window; only the fire is in it.)
        cell_pack::CellPack pack;
        const int hidden_index = (camera.view_y() + 40) * WORLD_W + camera.view_x() + 30;
        const frame::HiddenCell hidden[] = {{hidden_index, 0u, ElementType::Water},
                                            {hidden_index + 1, 0u, ElementType::Sand}};
        pack.pack(grid, camera.view_x(), camera.view_y(), PADDED_W, PADDED_H, hidden);
        gl->upload_cell_pack(pack.texels().data(), pack.width(), pack.height());

        std::vector<uint32_t> gl_px;

        on_sdl.draw(sdl, false);
        on_gl.draw(*gl, false);
        gl->read_pixels(gl_px);
        check_parity("the composed world matches the SDL frame", surface, gl_px);

        on_sdl.draw(sdl, true);
        on_gl.draw(*gl, true);
        gl->read_pixels(gl_px);
        check_parity("the world and overlay match the SDL frame", surface, gl_px);

        // The comparison has to be able to fail, or the two numbers above say
        // nothing. One cell of camera movement on one side only must break it.
        camera.follow(CAM_X + 1.0f, CAM_Y, PADDED_W, PADDED_H, WORLD_W, WORLD_H);
        on_gl.draw(*gl, true);
        gl->read_pixels(gl_px);
        check("the comparison notices a one-cell camera move",
              compare(surface, gl_px).share_within < REQUIRED_SHARE);
        camera.follow(CAM_X, CAM_Y, PADDED_W, PADDED_H, WORLD_W, WORLD_H);

        // A frame presented and the next drawn must start clean: present() ends
        // the frame, and the next clear() opens another on the same target.
        gl->present();
        on_gl.draw(*gl, true);
        gl->read_pixels(gl_px);
        check_parity("a frame after present still matches", surface, gl_px);

        // Hot reload: the shipped shaders recompile and still draw the same frame.
        check("the shaders reload", gl->reload_shaders(), gl->error());
        on_gl.draw(*gl, true);
        gl->read_pixels(gl_px);
        check_parity("the frame after a reload matches", surface, gl_px);

        // The raw view: the pack's bytes as colour, one pixel per cell.
        gl->set_cell_view(gpu::GlCanvas::CellView::Raw);
        gl->set_draw_color(0, 0, 0, 255);
        gl->clear();
        gl->blit_cells(on_gl.p.cells, gfx::FRect{0.0f, 0.0f, static_cast<float>(PADDED_W),
                                                 static_cast<float>(PADDED_H)});
        gl->read_pixels(gl_px);
        gl->set_cell_view(gpu::GlCanvas::CellView::Legacy);
        int wrong = 0, first_wrong = -1;
        bool saw_hidden = false, saw_occluder = false, saw_fire = false;
        for (int y = 0; y < PADDED_H; ++y) {
            for (int x = 0; x < PADDED_W; ++x) {
                const uint32_t want = pack.texels()[static_cast<size_t>(y) * PADDED_W + x];
                const uint32_t argb = gl_px[static_cast<size_t>(y) * W + x];
                // ARGB8888 back to the pack's r | g << 8 | b << 16 | a << 24.
                const uint32_t got = ((argb >> 16) & 0xFFu) | (argb & 0xFF00u) |
                                     ((argb & 0xFFu) << 16) | (argb & 0xFF000000u);
                if (got != want && wrong++ == 0) first_wrong = y * PADDED_W + x;
                saw_hidden |= ((want >> 24) & cell_pack::FLAG_HIDDEN) != 0;
                saw_occluder |= ((want >> 24) & cell_pack::FLAG_OCCLUDER) != 0;
                saw_fire |= (want & 0xFFu) == static_cast<uint32_t>(ElementType::Fire);
            }
        }
        char detail[96];
        std::snprintf(detail, sizeof(detail), "%d of %d texels differ, first at cell %d", wrong,
                      PADDED_W * PADDED_H, first_wrong);
        std::printf("  raw cell pack: %s\n", detail);
        check("the shader reads back every pack texel exactly", wrong == 0, detail);
        check("the pack window holds a hidden cell, an occluder and fire",
              saw_hidden && saw_occluder && saw_fire);

        // And the material view draws something the legacy one does not, so the
        // switch reaches the shader: at least the fire's 16 cells change colour.
        std::vector<uint32_t> legacy_px;
        on_gl.draw(*gl, false);
        gl->read_pixels(legacy_px);
        gl->set_cell_view(gpu::GlCanvas::CellView::Material);
        on_gl.draw(*gl, false);
        gl->read_pixels(gl_px);
        gl->set_cell_view(gpu::GlCanvas::CellView::Legacy);
        long changed = 0;
        for (size_t i = 0; i < gl_px.size(); ++i) changed += gl_px[i] != legacy_px[i];
        const long fire_px = 16L * Camera::DEFAULT_SCALE * Camera::DEFAULT_SCALE;
        check("the material view repaints the cells", changed >= fire_px);

        // --- the multiply light model ---
        //
        // The pack without the hidden cells for these, so the GPU field sees the
        // same grid the CPU one reads.
        pack.pack(grid, camera.view_x(), camera.view_y(), PADDED_W, PADDED_H, {});
        gl->upload_cell_pack(pack.texels().data(), pack.width(), pack.height());
        const lit::LightConstants real = LightField::gpu_constants();
        lit::LightConstants dark_cells = real;
        for (float& e : dark_cells.emissive) e = 0.0f;

        const auto lit_draw = [&](const lit::LitFrame& f, std::vector<uint32_t>& out) {
            const frame::Params keep = on_gl.p;
            on_gl.p.light = nullptr;  // as main.cpp: the CPU light is not drawn in a lit frame
            gl->begin_lit_frame();
            frame::compose(*gl, on_gl.p);
            const bool ok = gl->resolve_lit(f);
            on_gl.p = keep;
            gl->read_pixels(out);
            return ok;
        };

        // Neutral: the unlit SDL frame, within tolerance.
        {
            const frame::Params keep = on_sdl.p;
            on_sdl.p.light = nullptr;
            on_sdl.draw(sdl, false);
            on_sdl.p = keep;
        }
        gl->set_light_constants(dark_cells);
        check("a neutral lit frame resolves", lit_draw(neutral_frame(camera), gl_px), gl->error());
        check_parity("a neutral lit frame matches the unlit SDL frame", surface, gl_px);
        std::vector<uint32_t> neutral_px = gl_px;

        // The GPU light field against the CPU LightField it ports.
        gl->set_light_constants(real);
        lit::LitFrame fire = neutral_frame(camera);
        fire.fire_gain = 1.0f;
        check("a lit frame with fire resolves", lit_draw(fire, gl_px), gl->error());
        {
            std::vector<float> rgb;
            int cols = 0, rows = 0;
            gl->read_light_blocks(rgb, cols, rows);
            check("the GPU light field is LightField's size",
                  cols == light.cols() && rows == light.rows());
            int within = 0, lit_blocks = 0, worst = 0;
            const int n = std::min(cols * rows, light.cols() * light.rows());
            for (int i = 0; i < n; ++i) {
                const uint32_t want = light.pixels()[static_cast<size_t>(i)];
                int worst_here = 0;
                for (int k = 0; k < 3; ++k) {
                    const float v = rgb[3 * static_cast<size_t>(i) + k];
                    const float t = v <= 0.0f ? 0.0f : v / (1.0f + v);
                    // light.cpp's own packing, rounding and all, so the two compare like for like.
                    // NOLINTNEXTLINE(bugprone-incorrect-roundings)
                    const int got = static_cast<int>(std::clamp(t, 0.0f, 1.0f) * 255.0f + 0.5f);
                    const int exp = static_cast<int>((want >> (16 - 8 * k)) & 0xFFu);
                    worst_here = std::max(worst_here, std::abs(got - exp));
                }
                within += worst_here <= TOLERANCE ? 1 : 0;
                lit_blocks += (want & 0xFFFFFFu) != 0 ? 1 : 0;
                worst = std::max(worst, worst_here);
            }
            char field[128];
            std::snprintf(field, sizeof(field), "%d of %d blocks within +-%d, worst %d, %d lit",
                          within, n, TOLERANCE, worst, lit_blocks);
            std::printf("  GPU light field vs LightField: %s\n", field);
            check("the GPU light field matches LightField", within >= n * 99 / 100, field);
            check("the fixture's light field lights more than its fire's block", lit_blocks > 4,
                  field);
        }

        // Light raises luminance: around the fire, with its light against without,
        // under a dim ambient -- under a full one the paint is already as bright as
        // it gets, and the fire adds little to a dark albedo by multiplying it.
        {
            lit::LitFrame dim = neutral_frame(camera);
            dim.ambient[0] = dim.ambient[1] = dim.ambient[2] = 0.2f;
            lit_draw(dim, neutral_px);
            dim.fire_gain = 1.0f;
            lit_draw(dim, gl_px);
            const int fx = (205 - camera.view_x()) * Camera::DEFAULT_SCALE;
            const int fy = (104 - camera.view_y()) * Camera::DEFAULT_SCALE;
            const int x0 = std::clamp(fx - 40, 0, W - 96), y0 = std::clamp(fy - 40, 0, H - 96);
            const double with = luma(gl_px, W, x0, y0, 96, 96);
            const double without = luma(neutral_px, W, x0, y0, 96, 96);
            std::printf("  around the fire: luma %.1f lit, %.1f unlit\n", with, without);
            check("fire light raises luminance around the fire", with > without * 1.2);
            gl->set_light_constants(dark_cells);
            lit_draw(neutral_frame(camera), neutral_px);
        }

        // A dim ambient darkens the frame; a point light brightens around itself.
        {
            lit::LitFrame dim = neutral_frame(camera);
            dim.ambient[0] = dim.ambient[1] = dim.ambient[2] = 0.2f;
            gl->set_light_constants(dark_cells);
            lit_draw(dim, gl_px);
            const double dim_all = luma(gl_px, W, 0, 0, W, H);
            const double neutral_all = luma(neutral_px, W, 0, 0, W, H);
            check("a dim ambient darkens the frame", dim_all < neutral_all * 0.8);

            const int lx = 40, ly = 20;  // cells into the view, in the sky
            dim.light_count = 1;
            dim.lights[0] = lit::PointLight{static_cast<float>(camera.view_x() + lx),
                                            static_cast<float>(camera.view_y() + ly),
                                            2.0f,
                                            1.6f,
                                            1.2f,
                                            12.0f,
                                            0.0f};
            std::vector<uint32_t> lamp_px;
            lit_draw(dim, lamp_px);
            const int px = lx * Camera::DEFAULT_SCALE, py = ly * Camera::DEFAULT_SCALE;
            const double at_lamp = luma(lamp_px, W, px - 8, py - 8, 16, 16);
            const double dark_there = luma(gl_px, W, px - 8, py - 8, 16, 16);
            const double far = luma(lamp_px, W, W - 24, 8, 16, 16);
            const double far_dark = luma(gl_px, W, W - 24, 8, 16, 16);
            std::printf("  point light: luma %.1f at it (%.1f without), %.1f far (%.1f)\n", at_lamp,
                        dark_there, far, far_dark);
            check("a point light brightens around itself", at_lamp > dark_there + 10.0);
            check("a point light does not reach past its radius", std::abs(far - far_dark) < 1.0);
        }

        // The sun's march: a slab of wall in the open, the sun straight overhead.
        {
            Grid slab(WORLD_W, WORLD_H, 7u);
            const int sx = camera.view_x() + 40, sy = camera.view_y() + 20;
            for (int x = sx; x < sx + 30; ++x)
                for (int y = sy; y < sy + 4; ++y) slab.set_element(x, y, ElementType::Wall);
            cell_pack::CellPack slab_pack;
            slab_pack.pack(slab, camera.view_x(), camera.view_y(), PADDED_W, PADDED_H, {});
            gl->upload_cell_pack(slab_pack.texels().data(), slab_pack.width(), slab_pack.height());
            lit::LitFrame sun = neutral_frame(camera);
            sun.sun_on = true;
            sun.sun[0] = sun.sun[1] = sun.sun[2] = 1.0f;
            sun.sun_dir[0] = 0.0f;
            sun.sun_dir[1] = 1.0f;
            lit_draw(sun, gl_px);
            std::vector<float> vis;
            int vw = 0, vh = 0;
            gl->read_sun(vis, vw, vh);
            const auto at = [&](int x, int y) { return vis[static_cast<size_t>(y) * vw + x]; };
            check("the sun texture is one texel per pack cell", vw == PADDED_W && vh == PADDED_H);
            if (vw == PADDED_W && vh == PADDED_H) {
                std::printf("  sun: %.3f above the slab, %.3f on it, %.3f under it, %.3f beside\n",
                            at(55, 10), at(55, 20), at(55, 40), at(20, 40));
                check("the sun lights the open air", at(55, 10) > 0.99f && at(20, 40) > 0.99f);
                check("the sun lights the slab's top cell", at(55, 20) > 0.99f);
                check("the slab shades what is under it", at(55, 40) < 0.05f);
            }
            gl->upload_cell_pack(pack.texels().data(), pack.width(), pack.height());
        }

        // And a legacy frame after all that is still the SDL frame.
        gl->set_light_constants(real);
        on_sdl.draw(sdl, true);
        on_gl.draw(*gl, true);
        gl->read_pixels(gl_px);
        check_parity("a legacy frame after lit frames still matches", surface, gl_px);

        on_sdl.release(sdl);
        on_gl.release(*gl);
    }
    SDL_DestroyRenderer(renderer);
    SDL_FreeSurface(surface);
    gl.reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return report();
}
