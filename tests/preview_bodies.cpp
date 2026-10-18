// Headless frames of loose bodies being drawn, for judging by eye.
//
// Asserts nothing, like preview_light. It steps a world in which a wooden post
// goes over a ledge, and at a few steps composes the frame exactly the way
// main.cpp does -- present::paint_bodies into the body atlas, the cell texture
// uploaded with the bodies' cells left out, frame::compose -- and writes each as
// raw RGB, side by side in one image. What it is for: seeing that a body is drawn
// whole, turned, and where it is, rather than as cells a whole cell at a time.
//
// The `water` scene drops a stone and a wooden block into a pool instead: the
// cells a body leaves out should show the water it is in, not a sliver of
// backdrop around it.
//
// It composes every step, not only the shots, and prints how many screen pixels
// changed on each step a body went back to being cells, beside how many
// changed on the step before. A hand-back that jumps shows as the first number
// far above the second.
//
//   build/Release/preview_bodies bodies.raw [post|water]
//   python tools/rawpng.py bodies.raw bodies.png <width> <height>   (printed)
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "game/camera.h"
#include "game/run.h"
#include "present/present.h"
#include "render/frame.h"
#include "render/sdl_canvas.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr int W = 96, H = 64;  // the world, which is also the view
constexpr int SCALE = 4;
constexpr int SHOT_COUNT = 4;
constexpr int POST_SHOTS[SHOT_COUNT] = {14, 22, 30, 38};
constexpr int WATER_SHOTS[SHOT_COUNT] = {16, 24, 34, 60};
constexpr int RUN_ON = 240;  // steps past the last shot, to see the bodies stop

void fill(Grid& g, int x0, int y0, int x1, int y1, ElementType t) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) g.set_element(x, y, t);
}

} // namespace

int main(int argc, char** argv) {
    const char* out_path = argc > 1 ? argv[1] : "bodies.raw";
    const bool water = argc > 2 && std::strcmp(argv[2], "water") == 0;
    const int* shots = water ? WATER_SHOTS : POST_SHOTS;

    Run run(W, H, 99);
    Grid& g = run.grid;
    if (water) {
        fill(g, 0, 58, 95, 63, ElementType::Wall);  // the pool's floor
        fill(g, 0, 30, 1, 57, ElementType::Wall);   // and its walls
        fill(g, 94, 30, 95, 57, ElementType::Wall);
        fill(g, 2, 34, 93, 57, ElementType::Water);
        fill(g, 20, 6, 31, 11, ElementType::Wood);  // a 12x6 wooden block
        fill(g, 60, 6, 67, 11, ElementType::Wall);  // an 8x6 stone block
        g.set_element(25, 5, ElementType::Wall);
        g.set_element(25, 5, ElementType::Empty);
        g.set_element(63, 5, ElementType::Wall);
        g.set_element(63, 5, ElementType::Empty);
        run.player = Player(4, 30 - Player::HEIGHT);
    } else {
        fill(g, 0, 40, 40, 63, ElementType::Wall);   // a ledge
        fill(g, 41, 58, 95, 63, ElementType::Wall);  // low ground
        fill(g, 36, 12, 39, 39, ElementType::Wood);  // a post near its lip
        fill(g, 40, 12, 52, 17, ElementType::Wood);  // with a heavy head out over the drop
        g.set_element(37, 11, ElementType::Wall);
        g.set_element(37, 11, ElementType::Empty);
        run.player = Player(4, 40 - Player::HEIGHT);
    }

    SDL_Surface* surface =
        SDL_CreateRGBSurfaceWithFormat(0, W * SCALE, H * SCALE, 32, SDL_PIXELFORMAT_ARGB8888);
    SDL_Renderer* renderer = surface ? SDL_CreateSoftwareRenderer(surface) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "no software renderer: %s\n", SDL_GetError());
        return 1;
    }
    gfx::SdlCanvas canvas(renderer);
    const gfx::Texture cells =
        canvas.create_texture(W, H, gfx::Access::Streaming, gfx::Blend::Alpha);
    const gfx::Texture atlas = canvas.create_texture(present::BODY_ATLAS_W, present::BODY_ATLAS_H,
                                                     gfx::Access::Streaming, gfx::Blend::Alpha);

    Camera camera;
    camera.set_scale(SCALE);
    camera.follow(W / 2.0f, H / 2.0f, W, H, W, H);

    std::vector<uint32_t> atlas_pixels;
    std::vector<frame::BodySprite> sprites;
    std::vector<frame::HiddenCell> hidden;
    std::vector<frame::SprayDrop> spray;
    std::vector<uint32_t> view(static_cast<size_t>(W) * H);
    std::vector<unsigned char> img(static_cast<size_t>(W) * SCALE * SHOT_COUNT * H * SCALE * 3);

    std::vector<uint32_t> last_frame;
    int shot = 0, prev_changed = 0;
    const int last_step = shots[SHOT_COUNT - 1] + RUN_ON;
    for (int step = 1; step <= last_step; ++step) {
        const int bodies_before = g.tipping_count();
        run.step(Input{});
        // Halfway between the last two steps, as a frame usually is.
        present::paint_bodies(run, 0.5f, atlas_pixels, sprites, hidden);
        present::paint_spray(run, 0.5f, spray);
        if (water) {
            // Liquid is conserved across the grid and the spray together, and a
            // splash shows as drops in the air for a few steps.
            int64_t liquid = g.spray_mass();
            for (int i = 0; i < W * H; ++i)
                if (is_liquid(g.get_element(i % W, i / W).type))
                    liquid += g.get_element(i % W, i / W).mass;
            if (step == 1 || !spray.empty() || step == last_step)
                std::printf("step %d: %zu drop(s) in the air, liquid %lld\n", step, spray.size(),
                            static_cast<long long>(liquid));
        }
        for (const frame::BodySprite& s : sprites)
            canvas.update_texture(atlas, &s.src,
                                  atlas_pixels.data() +
                                      static_cast<size_t>(s.src.y) * present::BODY_ATLAS_W +
                                      s.src.x,
                                  present::BODY_ATLAS_W * static_cast<int>(sizeof(uint32_t)));
        view = g.get_pixels();
        for (const frame::HiddenCell& h : hidden) view[static_cast<size_t>(h.index)] = h.fill;
        canvas.update_texture(cells, nullptr, view.data(), W * static_cast<int>(sizeof(uint32_t)));

        frame::Params p;
        p.camera = &camera;
        p.padded_w = W;
        p.padded_h = H;
        p.cells = cells;
        p.body_atlas = atlas;
        p.bodies = &sprites;
        p.spray = &spray;
        frame::compose(canvas, p);

        const auto* px = static_cast<const uint32_t*>(surface->pixels);
        const int pitch = surface->pitch / 4;

        const size_t row = static_cast<size_t>(W) * SCALE;
        int changed = 0;
        if (!last_frame.empty())
            for (int y = 0; y < H * SCALE; ++y)
                for (int x = 0; x < W * SCALE; ++x)
                    changed += px[y * pitch + x] != last_frame[static_cast<size_t>(y) * row + x];
        last_frame.resize(row * H * SCALE);
        for (int y = 0; y < H * SCALE; ++y)
            std::memcpy(&last_frame[static_cast<size_t>(y) * row], px + y * pitch,
                        row * sizeof(uint32_t));
        if (g.tipping_count() < bodies_before)
            std::printf(
                "step %d: a body went back to cells; %d pixels changed (the step before: %d)\n",
                step, changed, prev_changed);
        prev_changed = changed;

        if (shot >= SHOT_COUNT || step != shots[shot]) continue;
        for (int y = 0; y < H * SCALE; ++y)
            for (int x = 0; x < W * SCALE; ++x) {
                const uint32_t c = px[y * pitch + x];
                unsigned char* o =
                    &img[(static_cast<size_t>(y) * W * SCALE * SHOT_COUNT + shot * W * SCALE + x) *
                         3];
                o[0] = static_cast<unsigned char>(c >> 16);
                o[1] = static_cast<unsigned char>(c >> 8);
                o[2] = static_cast<unsigned char>(c);
            }
        std::printf("step %d: %zu body sprite(s), %zu cells hidden\n", step, sprites.size(),
                    hidden.size());
        ++shot;
    }

    FILE* f = std::fopen(out_path, "wb");
    if (!f) return 1;
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    std::printf("python tools/rawpng.py %s out.png %d %d\n", out_path, W * SCALE * SHOT_COUNT,
                H * SCALE);
    return 0;
}
