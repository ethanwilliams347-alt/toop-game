// Headless preview of what the screen actually looks like: backdrop, cells, and
// the light field composited the way main.cpp composites them. It exists because
// the defect it is chasing is a look, and the only honest way to check a look is
// to look at it.
//
// Writes raw RGB to a file; `python tools/rawpng.py out.raw out.png 804 604`
// wraps it into a PNG.
#include "physics/grid.h"
#include "render/light.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr int SCALE = 4;
constexpr int VIEW_W = 201, VIEW_H = 151;
constexpr int OUT_W = VIEW_W * SCALE, OUT_H = VIEW_H * SCALE;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Bilinear sample of the light field, matching what SDL_ScaleModeLinear does
// when the texture is stretched over cols()*BLOCK by rows()*BLOCK cells.
void sample_light(const LightField& L, float cell_x, float cell_y, float out[3]) {
    const float tx = cell_x / LightField::BLOCK - 0.5f;
    const float ty = cell_y / LightField::BLOCK - 0.5f;
    const int x0 = static_cast<int>(std::floor(tx)), y0 = static_cast<int>(std::floor(ty));
    const float fx = tx - x0, fy = ty - y0;

    auto at = [&](int bx, int by, int ch) -> float {
        bx = bx < 0 ? 0 : (bx >= L.cols() ? L.cols() - 1 : bx);
        by = by < 0 ? 0 : (by >= L.rows() ? L.rows() - 1 : by);
        const uint32_t t = L.pixels()[static_cast<size_t>(by) * L.cols() + bx];
        return static_cast<float>((t >> (16 - 8 * ch)) & 0xFF);
    };
    for (int ch = 0; ch < 3; ++ch) {
        const float a = at(x0, y0, ch) * (1 - fx) + at(x0 + 1, y0, ch) * fx;
        const float b = at(x0, y0 + 1, ch) * (1 - fx) + at(x0 + 1, y0 + 1, ch) * fx;
        out[ch] = a * (1 - fy) + b * fy;
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* out_path = argc > 1 ? argv[1] : "preview.raw";
    const int steps = argc > 2 ? std::atoi(argv[2]) : 90;

    // A scene shaped like the screenshot that prompted this: two heavy wooden
    // ledges over open space in a walled cavern, well alight.
    constexpr int GW = 260, GH = 200;
    Grid g(GW, GH, 4242);

    for (int x = 0; x < GW; ++x)
        for (int y = GH - 8; y < GH; ++y) g.set_element(x, y, ElementType::Wall);
    for (int y = 0; y < GH; ++y)
        for (int x = 0; x < 6; ++x) g.set_element(x, y, ElementType::Wall);

    // Upper-right ledge and lower-left ledge, both wood, both lit.
    for (int i = 0; i < 90; ++i)
        for (int t = 0; t < 7; ++t) g.set_element(120 + i, 40 + i / 6 + t, ElementType::Wood);
    for (int i = 0; i < 70; ++i)
        for (int t = 0; t < 8; ++t) g.set_element(10 + i, 95 + i / 5 + t, ElementType::Wood);
    for (int y = 100; y < GH - 8; ++y) g.set_element(30, y, ElementType::Wood);  // a post

    // Light it: seed the exposed top row of each ledge as Charred, which is what
    // ignition produces.
    for (int i = 0; i < 90; ++i) g.set_element(120 + i, 40 + i / 6, ElementType::Charred);
    for (int i = 0; i < 70; ++i) g.set_element(10 + i, 95 + i / 5, ElementType::Charred);

    for (int i = 0; i < steps; ++i) g.update();

    const int view_x = 20, view_y = 30;
    LightField light(VIEW_W, VIEW_H);
    light.update(g, view_x, view_y);

    std::printf("preview: %d steps, any_light=%d, fire cells in view=", steps, light.any_light() ? 1 : 0);
    int nfire = 0;
    for (int y = 0; y < VIEW_H; ++y)
        for (int x = 0; x < VIEW_W; ++x)
            if (g.get_element(view_x + x, view_y + y).type == ElementType::Fire) ++nfire;
    std::printf("%d\n", nfire);

    // Report the light field's own peak, which is the number the "blown out"
    // complaint is really about.
    int peak = 0; long long sum = 0; int nonzero = 0;
    for (uint32_t t : light.pixels()) {
        const int r = (t >> 16) & 0xFF, gg = (t >> 8) & 0xFF, b = t & 0xFF;
        const int m = r > gg ? (r > b ? r : b) : (gg > b ? gg : b);
        if (m > peak) peak = m;
        if (m > 0) { sum += m; ++nonzero; }
    }
    std::printf("light: peak channel %d, %d/%d blocks lit (%.0f%%), mean lit %.0f\n",
                peak, nonzero, static_cast<int>(light.pixels().size()),
                100.0 * nonzero / light.pixels().size(),
                nonzero ? static_cast<double>(sum) / nonzero : 0.0);

    std::vector<unsigned char> img(static_cast<size_t>(OUT_W) * OUT_H * 3);
    const std::vector<uint32_t>& px = g.get_pixels();

    for (int sy = 0; sy < OUT_H; ++sy) {
        for (int sx = 0; sx < OUT_W; ++sx) {
            const int cx = view_x + sx / SCALE, cy = view_y + sy / SCALE;

            // Backdrop: a vertical gradient in 64 bands.
            const int band = sy * 64 / OUT_H;
            const int tt = band * 255 / 63;
            float r = 18 + (46 - 18) * tt / 255.0f;
            float gg = 20 + (52 - 20) * tt / 255.0f;
            float b = 34 + (74 - 34) * tt / 255.0f;

            // Cell, alpha-composited over it.
            if (cx >= 0 && cx < GW && cy >= 0 && cy < GH) {
                const uint32_t c = px[static_cast<size_t>(cy) * GW + cx];
                const float a = ((c >> 24) & 0xFF) / 255.0f;
                r = r * (1 - a) + ((c >> 16) & 0xFF) * a;
                gg = gg * (1 - a) + ((c >> 8) & 0xFF) * a;
                b = b * (1 - a) + (c & 0xFF) * a;
            }

            // Light, added.
            float L[3];
            sample_light(light, static_cast<float>(sx) / SCALE, static_cast<float>(sy) / SCALE, L);
            r += L[0]; gg += L[1]; b += L[2];

            const size_t o = (static_cast<size_t>(sy) * OUT_W + sx) * 3;
            img[o + 0] = static_cast<unsigned char>(clampf(r, 0, 255));
            img[o + 1] = static_cast<unsigned char>(clampf(gg, 0, 255));
            img[o + 2] = static_cast<unsigned char>(clampf(b, 0, 255));
        }
    }

    // How much of the frame is clipped to near-white? That is "overexposed", stated
    // as a number instead of an impression.
    int blown = 0;
    for (size_t i = 0; i < img.size(); i += 3)
        if (img[i] > 250 && img[i + 1] > 250 && img[i + 2] > 250) ++blown;
    std::printf("frame: %.1f%% of pixels are clipped to white\n",
                100.0 * blown / (OUT_W * OUT_H));

    std::FILE* f = std::fopen(out_path, "wb");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    std::printf("wrote %s (%dx%d rgb)\n", out_path, OUT_W, OUT_H);
    return 0;
}
