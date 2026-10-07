// Headless frames of a perspective-rig backdrop, for looking at.
//
//     build/Release/preview_backdrop bg_tarn out            four stills
//     build/Release/preview_backdrop bg_tarn out 120        a 120-frame flight
//     python tools/rawpng.py out/still_0.raw still_0.png 1920 1080
//
// Not an add_test(), for preview_light's reason: it asserts nothing, and what it
// produces is for a person to judge. The depth rig is a feel -- whether the lake
// reads as wide, whether climbing reads as climbing -- and no checksum says that.
//
// It composes with the real frame::compose, the real layer table and the real
// draw_rig_layer, into a software renderer over a surface, the way
// golden_frame_test does; only the world is stood in for. The cells are the
// scene's own albedo where its material map is solid, and there is no player and
// no light. Run from the repo root, so assets/ resolves.
#include <SDL.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "game/camera.h"
#include "render/frame.h"
#include "render/rig_backdrop.h"
#include "scene/bmp.h"

namespace {

constexpr int WINDOW_W = 1920;
constexpr int WINDOW_H = 1080;
constexpr int SCALE = 10;
constexpr int PADDED_W = WINDOW_W / SCALE + 1;
constexpr int PADDED_H = WINDOW_H / SCALE + 1;
constexpr int BODY_H = 26;  // Player::HEIGHT, stated here so this links no physics

SDL_Texture* load(SDL_Renderer* r, const std::string& path, bool key) {
    SDL_Surface* s = SDL_LoadBMP(path.c_str());
    if (!s) {
        std::fprintf(stderr, "cannot load %s: %s\n", path.c_str(), SDL_GetError());
        return nullptr;
    }
    if (key) SDL_SetColorKey(s, SDL_TRUE, SDL_MapRGB(s->format, 0xFF, 0x00, 0xFF));
    SDL_Texture* t = SDL_CreateTextureFromSurface(r, s);
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}

bool write_raw(SDL_Surface* surface, const std::string& path) {
    std::vector<unsigned char> rgb(static_cast<size_t>(WINDOW_W) * WINDOW_H * 3);
    for (int y = 0; y < WINDOW_H; ++y) {
        const Uint32* row = reinterpret_cast<const Uint32*>(
            static_cast<const unsigned char*>(surface->pixels) + y * surface->pitch);
        for (int x = 0; x < WINDOW_W; ++x) {
            const size_t o = (static_cast<size_t>(y) * WINDOW_W + x) * 3;
            rgb[o + 0] = static_cast<unsigned char>((row[x] >> 16) & 0xFF);
            rgb[o + 1] = static_cast<unsigned char>((row[x] >> 8) & 0xFF);
            rgb[o + 2] = static_cast<unsigned char>(row[x] & 0xFF);
        }
    }
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(rgb.data(), 1, rgb.size(), f) == rgb.size();
    std::fclose(f);
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: preview_backdrop <scene> <out_dir> [flight_frames]\n");
        return 2;
    }
    const rig_backdrop::Set* set = rig_backdrop::find(argv[1]);
    if (!set) {
        std::fprintf(stderr, "no rig set named '%s'\n", argv[1]);
        return 2;
    }
    const std::string out = argv[2];
    const int flight = argc > 3 ? std::atoi(argv[3]) : 0;

    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, WINDOW_W, WINDOW_H, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "no software renderer: %s\n", SDL_GetError());
        return 1;
    }

    frame::Backdrop backdrop;
    for (int i = 0; i < set->layer_count; ++i) {
        const rig_backdrop::Layer& sp = set->layers[i];
        frame::ParallaxLayer l;
        l.texture = load(renderer, std::string(set->dir) + sp.file, !sp.opaque);
        if (!l.texture) return 1;
        l.w = set->native_w * SCALE;
        l.h = set->native_h * SCALE;
        l.tex_h = set->native_h;
        l.parallax_x = rig_backdrop::factor_of(*set, sp);
        l.parallax_y = depth_rig::vertical_factor(set->rig, l.parallax_x);
        l.line_scroll = sp.line_scroll;
        l.on_plane = sp.on_plane;
        l.ripple_row0 = sp.ripple_row0;
        l.ripple_row1 = sp.ripple_row1;
        l.drift = sp.drift;
        l.is_foreground = sp.is_foreground;
        backdrop.layers.push_back(l);
    }
    backdrop.rig_on = true;
    backdrop.rig = set->rig;
    backdrop.ripple_amplitude = set->ripple_amplitude;

    // The world: the scene's albedo wherever its material map is not air.
    const std::string scene = std::string("assets/") + set->scene;
    bmp::Image mat, alb;
    std::string err;
    if (!bmp::read((scene + "_material.bmp").c_str(), mat, &err) ||
        !bmp::read((scene + "_albedo.bmp").c_str(), alb, &err)) {
        std::fprintf(stderr, "scene maps: %s\n", err.c_str());
        return 1;
    }
    const int world_w = mat.width, world_h = mat.height;
    std::vector<uint32_t> world(mat.pixels.size());
    for (size_t i = 0; i < world.size(); ++i)
        world[i] = (mat.pixels[i] & 0xFFFFFFu) == 0 ? 0u : (0xFF000000u | (alb.pixels[i] & 0xFFFFFFu));
    SDL_Texture* cells = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, PADDED_W, PADDED_H);
    SDL_SetTextureBlendMode(cells, SDL_BLENDMODE_BLEND);

    const float anchor_x = 0.5f * static_cast<float>(world_w - PADDED_W);
    const float anchor_y = depth_rig::standing_anchor_y(set->rig, BODY_H, PADDED_H,
                                                        Camera::VERTICAL_ANCHOR, world_h);
    backdrop.rig_anchor_x = anchor_x;
    backdrop.rig_anchor_y = anchor_y;

    const auto shoot = [&](float view_x, float view_y, float t, const std::string& name) {
        Camera camera;
        camera.set_scale(SCALE);
        // follow() takes a centre; hand it the one that lands the view at (x, y).
        camera.follow(view_x + PADDED_W / 2.0f, view_y + PADDED_H * Camera::VERTICAL_ANCHOR,
                      PADDED_W, PADDED_H, world_w, world_h);
        const uint32_t* src = world.data() + camera.view_y() * world_w + camera.view_x();
        const SDL_Rect visible{0, 0, PADDED_W, PADDED_H};
        SDL_UpdateTexture(cells, &visible, src, world_w * static_cast<int>(sizeof(uint32_t)));

        frame::Params fp;
        fp.camera = &camera;
        fp.padded_w = PADDED_W;
        fp.padded_h = PADDED_H;
        fp.world_w = world_w;
        fp.world_h = world_h;
        fp.backdrop = backdrop;
        fp.cells = cells;
        fp.time_s = t;
        frame::compose(renderer, fp);
        if (!write_raw(surface, out + "/" + name + ".raw")) {
            std::fprintf(stderr, "cannot write %s/%s.raw\n", out.c_str(), name.c_str());
            return false;
        }
        std::printf("%s: view (%.1f, %.1f)\n", name.c_str(), camera.view_fx(), camera.view_fy());
        return true;
    };

    const float max_x = static_cast<float>(world_w - PADDED_W);
    if (flight <= 0) {
        // Standing at the anchor, standing at both ends of the world, and flying at
        // the ceiling -- the four frames that show what the rig is for.
        if (!shoot(anchor_x, anchor_y, 0.0f, "still_0_standing")) return 1;
        if (!shoot(0.0f, anchor_y, 0.0f, "still_1_left")) return 1;
        if (!shoot(max_x, anchor_y, 0.0f, "still_2_right")) return 1;
        if (!shoot(anchor_x, 0.0f, 0.0f, "still_3_high")) return 1;
    } else {
        // A walk right along the floor, then a climb to the ceiling and back down.
        for (int i = 0; i < flight; ++i) {
            const float u = static_cast<float>(i) / static_cast<float>(flight);
            float x, y;
            if (u < 0.5f) {
                x = anchor_x - 120.0f + 240.0f * (u / 0.5f);
                y = anchor_y;
            } else {
                x = anchor_x + 120.0f;
                y = anchor_y * (0.5f + 0.5f * std::cos((u - 0.5f) / 0.5f * 6.2831853f));
            }
            char name[32];
            std::snprintf(name, sizeof(name), "flight_%03d", i);
            if (!shoot(x, y, static_cast<float>(i) / 30.0f, name)) return 1;
        }
    }
    return 0;
}
