// One frame of a scene through the GL renderer, in its mood, saved as a BMP --
// for comparing looks side by side (old against new, one mood against another)
// without playing to the spot.
//
//     build/Release/preview_frame bg_tarn tarn.bmp
//     build/Release/preview_frame bg_tarn tarn_legacy.bmp mood=legacy
//     build/Release/preview_frame bg_tarn noir.bmp mood=bg_tarn/bg_tarn_noir_mood.txt
//     build/Release/preview_frame bg_tarn tarn.bmp 3440x1440 view=120,140 steps=60
//
// Options, any order, after the two required arguments:
//   WxH          the window (default 1920x1080), framed at the scene's scale=
//   mood=<path>  a *_mood.txt under assets/ instead of the scene's own, or
//                `legacy` for today's look (the CPU light, no mood)
//   view=X,Y     the camera's top-left world cell (default: standing at the
//                middle of the world, as preview_backdrop frames it)
//   steps=N      simulation steps before the shot (default 0: as loaded)
//
// What is drawn: the scene's backdrop and its world, stepped, with the mood's
// light (gpu::Pipeline) -- or the CPU LightField for a legacy mood, the way
// main.cpp draws it. No player, enemies, props or HUD: this is for the look of
// a place, and those need a Run.
//
// GL in a hidden window, so it needs a GL 3.3 driver; it exits 1 without one.
// Not an add_test(), for preview_light's reason: it asserts nothing, and what it
// produces is for a person to judge. Run from the repo root, so assets/ resolves.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "game/camera.h"
#include "gpu/gl_canvas.h"
#include "physics/grid.h"
#include "render/backdrop_set.h"
#include "render/cell_pack.h"
#include "render/frame.h"
#include "render/light.h"
#include "render/lit_frame.h"
#include "render/mood.h"
#include "scene/bmp.h"
#include "scene/scene.h"
#include "scene/scene_list.h"

namespace {

constexpr int BODY_H = 26;  // Player::HEIGHT, for the standing camera

gfx::Texture load(gfx::Canvas& canvas, const std::string& path, bool key) {
    SDL_Surface* s = SDL_LoadBMP(path.c_str());
    if (!s) {
        std::fprintf(stderr, "cannot load %s: %s\n", path.c_str(), SDL_GetError());
        return gfx::Texture{};
    }
    if (key) SDL_SetColorKey(s, SDL_TRUE, SDL_MapRGB(s->format, 0xFF, 0x00, 0xFF));
    SDL_Surface* argb = SDL_ConvertSurfaceFormat(s, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(s);
    if (!argb) return gfx::Texture{};
    const gfx::Texture t =
        canvas.create_texture(argb->w, argb->h, gfx::Access::Static, gfx::Blend::Alpha);
    if (t) canvas.update_texture(t, nullptr, argb->pixels, argb->pitch);
    SDL_FreeSurface(argb);
    return t;
}

bool parse_pair(const char* text, char sep, long& a, long& b) {
    char* end = nullptr;
    a = std::strtol(text, &end, 10);
    if (end == text || *end != sep) return false;
    const char* second = end + 1;
    b = std::strtol(second, &end, 10);
    return end != second && *end == '\0';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: preview_frame <scene> <out.bmp> [WxH] [mood=<path>|mood=legacy] "
                     "[view=X,Y] [steps=N]\n");
        return 2;
    }
    int window_w = 1920, window_h = 1080;
    std::string mood_field;
    bool mood_given = false;
    bool view_given = false;
    long view_x = 0, view_y = 0;
    int steps = 0;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        long w = 0, h = 0;
        if (a.compare(0, 5, "mood=") == 0) {
            mood_field = a.substr(5);
            mood_given = true;
        } else if (a.compare(0, 5, "view=") == 0 && parse_pair(argv[i] + 5, ',', view_x, view_y)) {
            view_given = true;
        } else if (a.compare(0, 6, "steps=") == 0) {
            steps = static_cast<int>(std::strtol(argv[i] + 6, nullptr, 10));
        } else if (parse_pair(argv[i], 'x', w, h) && w > 0 && h > 0) {
            window_w = static_cast<int>(w);
            window_h = static_cast<int>(h);
        } else {
            std::fprintf(stderr, "unknown option '%s'\n", argv[i]);
            return 2;
        }
    }

    std::string err;
    const std::vector<scene_list::SceneDef> scenes =
        scene_list::load_scene_list("assets/scenes.txt", &err);
    const scene_list::SceneDef* def = nullptr;
    for (const scene_list::SceneDef& d : scenes)
        if (d.name == argv[1]) def = &d;
    if (!def || def->material.empty()) {
        std::fprintf(stderr, "no scene '%s' with a material map%s%s\n", argv[1],
                     err.empty() ? "" : ": ", err.c_str());
        return 2;
    }
    const int scale = def->scale > 0 ? def->scale : Camera::DEFAULT_SCALE;
    const int padded_w = window_w / scale + 1;
    const int padded_h = window_h / scale + 1;

    // The world, stamped the way level::start stamps it.
    const Scene scene =
        bmp::load(("assets/" + def->material).c_str(), ("assets/" + def->albedo).c_str(), &err);
    if (!err.empty()) {
        std::fprintf(stderr, "scene maps: %s\n", err.c_str());
        return 1;
    }
    const int world_w = def->custom_width > 0 ? def->custom_width : scene.width;
    const int world_h = def->custom_height > 0 ? def->custom_height : scene.height;
    auto grid = std::make_unique<Grid>(world_w, world_h);
    load_scene(*grid, scene);
    for (int i = 0; i < steps; ++i) grid->update();

    std::string mood_error;
    const mood::Mood m =
        mood_given && mood_field == "legacy"
            ? mood::legacy()
            : mood::load_for_scene(mood_given ? mood_field : def->mood, "assets/", &mood_error);
    if (!mood_error.empty()) {
        std::fprintf(stderr, "%s\n", mood_error.c_str());
        return 1;
    }
    const bool lit_frame = m.light_model == mood::LightModel::Multiply;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "no video: %s\n", SDL_GetError());
        return 1;
    }
    gpu::GlCanvas::set_window_attributes();
    SDL_Window* window =
        SDL_CreateWindow("preview_frame", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                         window_w, window_h, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    std::string why;
    std::unique_ptr<gpu::GlCanvas> gl =
        window ? gpu::GlCanvas::create(window, "assets/shaders", false, why) : nullptr;
    if (!gl) {
        std::fprintf(stderr, "no GL renderer: %s\n", window ? why.c_str() : SDL_GetError());
        return 1;
    }
    gl->set_light_constants(LightField::gpu_constants());
    int code = 0;
    {
        // The backdrop, as shell::load_backdrop builds it.
        frame::Backdrop backdrop;
        backdrop_set::Set set;
        std::vector<gfx::Texture> owned;
        if (!def->backdrop.empty()) {
            const std::string dir = "assets/" + def->backdrop + "/";
            set = backdrop_set::load(dir + "backdrop.txt", dir, &err);
            for (const backdrop_set::Layer& sp : set.layers) {
                frame::ParallaxLayer l;
                l.texture = load(*gl, set.dir + sp.file, !sp.opaque);
                owned.push_back(l.texture);
                l.w = backdrop_set::width_of(set, sp) * scale;
                l.h = set.native_h * scale;
                l.tex_h = set.native_h;
                l.parallax_x = backdrop_set::factor_of(set, sp);
                l.parallax_y = backdrop_set::vertical_factor_of(set, sp);
                l.bands = sp.bands;
                l.line_scroll = sp.plane;
                l.on_plane = sp.on_plane;
                l.ripple_row0 = sp.ripple_row0;
                l.ripple_row1 = sp.ripple_row1;
                l.drift = sp.drift;
                l.is_foreground = sp.foreground;
                backdrop.layers.push_back(l);
            }
            backdrop.rig = set.rig;
            backdrop.ripple_amplitude = set.ripple_amplitude;
        }

        const float stand_x = 0.5f * static_cast<float>(world_w - padded_w);
        const float stand_y = set.has_rig
                                  ? depth_rig::standing_anchor_y(set.rig, BODY_H, padded_h,
                                                                 Camera::VERTICAL_ANCHOR, world_h)
                                  : static_cast<float>(world_h - padded_h);
        if (set.anchor == backdrop_set::Anchor::Standing) {
            backdrop.anchor_x = stand_x;
            backdrop.anchor_y = stand_y;
        }
        const float vx = view_given ? static_cast<float>(view_x) : stand_x;
        const float vy = view_given ? static_cast<float>(view_y) : stand_y;
        Camera camera;
        camera.set_scale(scale);
        camera.follow(vx + padded_w / 2.0f, vy + padded_h * Camera::VERTICAL_ANCHOR, padded_w,
                      padded_h, world_w, world_h);

        // The cells, the pack and (legacy) the CPU light, over the same window.
        const int visible_w = std::min(padded_w, world_w - camera.view_x());
        const int visible_h = std::min(padded_h, world_h - camera.view_y());
        const gfx::Texture cells =
            gl->create_texture(padded_w, padded_h, gfx::Access::Streaming, gfx::Blend::Alpha);
        owned.push_back(cells);
        const std::vector<uint32_t>& pixels = grid->get_pixels();
        const gfx::Rect visible{0, 0, visible_w, visible_h};
        gl->update_texture(cells, &visible,
                           pixels.data() + static_cast<size_t>(camera.view_y()) * world_w +
                               camera.view_x(),
                           world_w * static_cast<int>(sizeof(uint32_t)));
        cell_pack::CellPack pack;
        pack.pack(*grid, camera.view_x(), camera.view_y(), visible_w, visible_h, {});
        gl->upload_cell_pack(pack.texels().data(), pack.width(), pack.height());

        LightField light(padded_w, padded_h);
        gfx::Texture light_tex;
        if (!lit_frame) {
            light.update(*grid, camera.view_x(), camera.view_y());
            light_tex = gl->create_texture(light.cols(), light.rows(), gfx::Access::Streaming,
                                           gfx::Blend::Add, gfx::Filter::Linear);
            owned.push_back(light_tex);
            gl->update_texture(light_tex, nullptr, light.pixels().data(),
                               light.cols() * static_cast<int>(sizeof(uint32_t)));
        }

        frame::Params fp;
        fp.camera = &camera;
        fp.padded_w = padded_w;
        fp.padded_h = padded_h;
        fp.backdrop = &backdrop;
        fp.cells = cells;
        fp.light = lit_frame ? nullptr : &light;
        fp.light_texture = light_tex;
        if (lit_frame) gl->begin_lit_frame();
        frame::compose(*gl, fp);
        if (lit_frame) {
            lit::LitFrame lf;
            mood::fill_lit(m, lf);
            lf.origin_x = camera.view_x();
            lf.origin_y = camera.view_y();
            lf.screen_x = -camera.frac_x() * static_cast<float>(scale);
            lf.screen_y = -camera.frac_y() * static_cast<float>(scale);
            lf.scale = scale;
            if (!gl->resolve_lit(lf)) {
                std::fprintf(stderr, "the lit frame failed: %s\n", gl->error());
                code = 1;
            }
        }

        std::vector<uint32_t> out;
        gl->read_pixels(out);
        SDL_Surface* s =
            SDL_CreateRGBSurfaceWithFormatFrom(out.data(), gl->target_w(), gl->target_h(), 32,
                                               gl->target_w() * 4, SDL_PIXELFORMAT_ARGB8888);
        if (!s || SDL_SaveBMP(s, argv[2]) != 0) {
            std::fprintf(stderr, "cannot write %s: %s\n", argv[2], SDL_GetError());
            code = 1;
        } else {
            std::printf("%s: scene %s, %s mood, view (%d, %d), %dx%d\n", argv[2], def->name.c_str(),
                        lit_frame ? "multiply" : "legacy", camera.view_x(), camera.view_y(),
                        gl->target_w(), gl->target_h());
        }
        if (s) SDL_FreeSurface(s);
        for (gfx::Texture& t : owned) gl->destroy_texture(t);
    }
    gl.reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return code;
}
