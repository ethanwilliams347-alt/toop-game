// What share of the frame the receding ground plane actually gets, and what is
// standing in front of it, in numbers.
//
// Not a registered test, for the same reason the other probes are not: what it
// reports is an input to a composition decision, and a test pinning today's
// composition would be a copy of the answer rather than a check on it.
//
// It takes the shipped camera, the shipped parallax factors and the shipped
// fixture, and reports where the plane's band lands on screen and how much of it
// the world's own terrain covers up -- so a composition target is checked for
// reachability before anything is authored against it.
//
// It links no SDL. backdrop_wrap.h is arithmetic by design and the camera is a
// game-side class, so the only thing missing is the draw call, which cannot
// change where anything lands.
//
// Four numbers per camera position, all as a share of the cells in the window:
//   PLANE - open world whose screen row is inside the plane's band. This is the
//           plane you can actually see.
//   WORLD - cells the fixture fills. Solid terrain occludes every backdrop
//           layer, so this is the budget PLANE competes against.
//   ABOVE - open world above the horizon: sky, mountains, treeline.
//   BELOW - open world past the plane's near edge, which is the flat fill colour
//           frame.cpp paints under the band.
// and the same four again for the region below the player's feet, which is the
// one the reference frames its subject against.
#include "game/camera.h"
#include "physics/grid.h"
#include "physics/player.h"
#include "render/backdrop_layers.h"
#include "render/backdrop_wrap.h"
#include "render/surface_plane.h"
#include "scene/legend.h"
#include "scene/scene.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr char CHR_NL[] = {10, 0};

// The 1920x1080 viewport in cells. Stated here rather than included so the probe
// does not drag the SDL shell's header in for two integers.
constexpr int VIEW_W = 1920 / Camera::DEFAULT_SCALE;
constexpr int VIEW_H = 1080 / Camera::DEFAULT_SCALE;

// The shipped mountains BMP's height. ground_horizon_y in frame.cpp reads it off
// the loaded texture; here there is no texture, so it comes from the generated
// header -- the same number main.cpp warns about a mismatch against at startup.
constexpr int MOUNTAIN_H = backdrop_layers::MOUNTAINS.height;

// Minimal 24-bit BMP reader, the same one rim_probe carries and for the same
// reason: the probes are headless like the suites are.
bool read_bmp24(const char* path, int& w, int& h, std::vector<uint32_t>& out) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "cannot open %s\n", path); return false; }
    std::vector<unsigned char> buf;
    std::fseek(f, 0, SEEK_END);
    buf.resize(static_cast<size_t>(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size() || buf.size() < 54 || buf[0] != 'B' || buf[1] != 'M') {
        std::fprintf(stderr, "%s is not a readable BMP\n", path);
        return false;
    }
    auto u32 = [&](size_t o) {
        return static_cast<uint32_t>(buf[o]) | (buf[o + 1] << 8) | (buf[o + 2] << 16) |
               (static_cast<uint32_t>(buf[o + 3]) << 24);
    };
    const uint32_t offset = u32(10);
    const int width = static_cast<int>(u32(18));
    const int height_raw = static_cast<int>(u32(22));
    const int bpp = buf[28] | (buf[29] << 8);
    if (bpp != 24) { std::fprintf(stderr, "%s: only 24bpp supported\n", path); return false; }
    const bool top_down = height_raw < 0;
    w = width;
    h = height_raw < 0 ? -height_raw : height_raw;
    const int row_size = (w * 3 + 3) & ~3;
    out.assign(static_cast<size_t>(w) * h, 0);
    for (int row = 0; row < h; ++row) {
        const int file_row = top_down ? row : (h - 1 - row);
        const size_t base = offset + static_cast<size_t>(file_row) * row_size;
        for (int x = 0; x < w; ++x) {
            const unsigned char b = buf[base + x * 3 + 0];
            const unsigned char g = buf[base + x * 3 + 1];
            const unsigned char r = buf[base + x * 3 + 2];
            out[static_cast<size_t>(row) * w + x] =
                0xFF000000u | (static_cast<uint32_t>(r) << 16) | (g << 8) | b;
        }
    }
    return true;
}

enum class Band { Plane, World, Above, Below };

struct Share { long plane = 0, world = 0, above = 0, below = 0; };

void add(Share& s, Band b) {
    switch (b) {
        case Band::Plane: s.plane++; break;
        case Band::World: s.world++; break;
        case Band::Above: s.above++; break;
        case Band::Below: s.below++; break;
    }
}

void report(const char* label, const Share& s) {
    const long total = s.plane + s.world + s.above + s.below;
    if (total <= 0) { std::printf("  %-22s (no cells)\n", label); return; }
    auto pct = [&](long v) { return 100.0 * static_cast<double>(v) / static_cast<double>(total); };
    std::printf("  %-22s PLANE %5.1f%%   WORLD %5.1f%%   ABOVE %5.1f%%   BELOW %5.1f%%\n",
                label, pct(s.plane), pct(s.world), pct(s.above), pct(s.below));
}

// One camera position, fully classified.
void sample(const Grid& g, float center_x, float center_y, int feet_y, const char* label) {
    Camera camera;
    camera.follow(center_x, center_y, VIEW_W, VIEW_H, g.get_width(), g.get_height());

    const float horizon = camera.parallax_origin_y(backdrop_layers::MOUNTAINS.parallax_y) +
                          static_cast<float>(MOUNTAIN_H) * backdrop_layers::MOUNTAINS_SKYLINE_MAX;
    const backdrop_wrap::Plane plane = backdrop_wrap::plane_geometry(
        horizon, backdrop_layers::GROUND.height,
        backdrop_layers::GROUND.parallax_x, backdrop_layers::GROUND_NEAR_X);

    Share whole, under;
    for (int vy = 0; vy < VIEW_H; ++vy) {
        const int wy = camera.view_y() + vy;
        // The screen row of this cell's centre, which is what decides the band it
        // falls in. Whole cells rather than pixels: a cell is a few pixels square
        // and the question here is composition, not a pixel-exact clip.
        const float sy = camera.world_to_screen_y(static_cast<float>(wy) + 0.5f);
        const Band band = sy < plane.horizon_y ? Band::Above
                        : (sy < plane.bottom_y ? Band::Plane : Band::Below);
        for (int vx = 0; vx < VIEW_W; ++vx) {
            const int wx = camera.view_x() + vx;
            const bool solid = wx >= 0 && wx < g.get_width() && wy >= 0 && wy < g.get_height() &&
                               is_solid(g.get_element(wx, wy).type);
            const Band what = solid ? Band::World : band;
            add(whole, what);
            if (wy > feet_y) add(under, what);
        }
    }

    std::printf("\n%s\n", label);
    std::printf("  camera view (%d, %d), feet at world row %d\n",
                camera.view_x(), camera.view_y(), feet_y);
    std::printf("  horizon at screen y %.1f, plane near edge at %.1f (the window is 1080 tall)\n",
                plane.horizon_y, plane.bottom_y);
    report("whole window:", whole);
    report("below the feet:", under);
}


// --- the band ladder ------------------------------------------------------
//
// The shares above answer "what is on screen"; this answers "at what value",
// which is the quantity a report of "a separate shelf sitting in front of a
// painted backdrop" is about.
//
// Read it beside the same measurement on the reference: row-mean luminance down
// the frame, in bands. What the reference does is put its brightest band at the
// surface the character stands on and darken in both directions from there. A
// ladder that runs the other way at the junction -- a bright plane above a
// near-black world -- is the shelf, and no per-layer Grade can turn it over,
// because a Grade is uniform and this is a ramp inside one band.
//
// Rows above the horizon are excluded and reported as such rather than sampled.
// Composing sky and mountains here would mean reproducing two more parallax
// stacks for bands this pass does not change. The junction is below the horizon
// and so is everything the ladder is being asked about.
//
// Two approximations, both stated because a probe that quietly rounds is worse
// than no probe: the plane's texel row is taken straight from the depth mapping
// rather than through plane_src_row's strip quantisation, and a cell is
// classified whole rather than at pixel precision.
struct Rows {
    double world_sum = 0.0;  long world_n = 0;
    double plane_sum = 0.0;  long plane_n = 0;
    double after_sum = 0.0;  // world_sum again, with the near-ground pass applied
    long above_n = 0;
};

double luma(uint32_t argb) {
    const double r = (argb >> 16) & 0xFF, g = (argb >> 8) & 0xFF, b = argb & 0xFF;
    return 0.299 * r + 0.587 * g + 0.114 * b;
}

// The GROUND row of frame.cpp's layer table, which is a multiply.
constexpr double GROUND_GRADE = 135.0 / 255.0;

void band_ladder(const Grid& g, float center_x, float center_y, int feet_y,
                 const std::vector<uint32_t>& tile, int tile_w, int tile_h,
                 int bands) {
    // The tile's rows reduced to one graded luminance each, once. The tile
    // dithers horizontally between two flat tones, so a row mean is the right
    // sample of it -- the same thing the eye does at this size.
    std::vector<double> row_lum(static_cast<size_t>(tile_h), 0.0);
    for (int ty = 0; ty < tile_h; ++ty) {
        double sum = 0.0;
        for (int tx = 0; tx < tile_w; ++tx) sum += luma(tile[static_cast<size_t>(ty) * tile_w + tx]);
        row_lum[static_cast<size_t>(ty)] = sum / tile_w * GROUND_GRADE;
    }

    Camera camera;
    camera.follow(center_x, center_y, VIEW_W, VIEW_H, g.get_width(), g.get_height());
    const float horizon = camera.parallax_origin_y(backdrop_layers::MOUNTAINS.parallax_y) +
                          static_cast<float>(MOUNTAIN_H) * backdrop_layers::MOUNTAINS_SKYLINE_MAX;
    const backdrop_wrap::Plane plane = backdrop_wrap::plane_geometry(
        horizon, backdrop_layers::GROUND.height,
        backdrop_layers::GROUND.parallax_x, backdrop_layers::GROUND_NEAR_X);
    const float band_h = plane.bottom_y - plane.horizon_y;

    // The near-ground pass, applied to the same sample, so the ladder can be
    // read before and after in one table. The weight and the surface scan come
    // from render/surface_plane.cpp rather than from a copy here -- a probe that
    // reimplements the thing it measures agrees with itself and with nothing
    // else. Blending luminance is the same as blending the channels and taking
    // luma afterwards, because luma is linear in them.
    const std::vector<uint32_t>& px = g.get_pixels();
    const surface_plane::View view{camera.view_x(), camera.view_y(), VIEW_W, VIEW_H};
    std::vector<int> depth(static_cast<size_t>(VIEW_W) * VIEW_H, -1);
    surface_plane::depth_map(px.data(), g.get_width(), g.get_height(), view, depth.data());

    std::vector<Rows> band(static_cast<size_t>(bands));
    for (int vy = 0; vy < VIEW_H; ++vy) {
        const int wy = camera.view_y() + vy;
        const float sy = camera.world_to_screen_y(static_cast<float>(wy) + 0.5f);
        const size_t bi = static_cast<size_t>(vy) * bands / VIEW_H;
        // Which tile row this screen row samples, through plane_src_at. A linear
        // map from screen row to tile row is the degenerate case that function
        // returns when the two parallax factors collapse to one depth; the shipped
        // draw uses the inverse-depth relation, so a linear probe reports the plane
        // at rows the renderer never puts there.
        //
        // Below the plane's near edge frame.cpp continues the tile's last row, so
        // that is what is sampled there -- not a new colour.
        const float t_row = band_h > 0.0f ? (sy - plane.horizon_y) / band_h : 0.0f;
        int texel = t_row >= 1.0f
                        ? tile_h - 1
                        : static_cast<int>(backdrop_wrap::plane_src_at(plane, t_row) + 0.5f);
        if (texel < 0) texel = 0;
        if (texel >= tile_h) texel = tile_h - 1;
        for (int vx = 0; vx < VIEW_W; ++vx) {
            const int wx = camera.view_x() + vx;
            const bool inside = wx >= 0 && wx < g.get_width() && wy >= 0 && wy < g.get_height();
            if (inside && is_solid(g.get_element(wx, wy).type)) {
                const double raw = luma(g.get_element(wx, wy).color);
                band[bi].world_sum += raw;
                band[bi].world_n++;
                const int w = surface_plane::weight_at_depth(
                                  depth[static_cast<size_t>(vy) * VIEW_W + vx]) *
                              surface_plane::row_scale_at(t_row) / 255;
                band[bi].after_sum +=
                    (raw * (255 - w) + row_lum[static_cast<size_t>(texel)] * w) / 255.0;
            } else if (sy < plane.horizon_y) {
                band[bi].above_n++;
            } else {
                band[bi].plane_sum += row_lum[static_cast<size_t>(texel)];
                band[bi].plane_n++;
            }
        }
    }

    // --- what the pass actually reaches -------------------------------------
    //
    // The ladder above says what the near band came out at; this says how much of
    // it the pass touched at all, and the two are different questions. A depth map
    // measured from the top of each column leaves a large share of the band
    // untouched, which the ladder shows only as a flat number a little too low --
    // exactly the kind of reading that gets explained away as a tuning problem. A
    // census cannot be explained away.
    {
        long n = 0, full = 0, none = 0, wsum = 0;
        int deepest = 0;
        for (int vy = 0; vy < VIEW_H; ++vy) {
            const int wy = camera.view_y() + vy;
            if (wy <= feet_y) continue;
            for (int vx = 0; vx < VIEW_W; ++vx) {
                const int wx = camera.view_x() + vx;
                if (wx < 0 || wx >= g.get_width() || wy >= g.get_height()) continue;
                if (!is_solid(g.get_element(wx, wy).type)) continue;
                const int d = depth[static_cast<size_t>(vy) * VIEW_W + vx];
                const float sy2 = camera.world_to_screen_y(static_cast<float>(wy) + 0.5f);
                const int w = surface_plane::weight_at_depth(d) *
                              surface_plane::row_scale_at(
                                  band_h > 0.0f ? (sy2 - plane.horizon_y) / band_h : 0.0f) / 255;
                n++; wsum += w;
                if (w >= 255) full++;
                if (w <= 0) { none++; if (d > deepest) deepest = d; }
            }
        }
        std::printf("%snear-ground blend below feet (%ld solid cells):%s", CHR_NL, n, CHR_NL);
        std::printf("  mean weight %5.1f of 255   full %5.1f%%   untouched %5.1f%%   deepest untouched %d%s",
                    n ? static_cast<double>(wsum) / n : 0.0,
                    n ? 100.0 * full / n : 0.0,
                    n ? 100.0 * none / n : 0.0, deepest, CHR_NL);
    }

    const int feet_band = (feet_y - camera.view_y()) * bands / VIEW_H;
    std::printf("\nthe band ladder at the spawn (luminance 0-255, %d bands down the window):\n", bands);
    std::printf("  band   %% down   world   plane   frame   solid%%    blend frame+blend\n");
    for (int i = 0; i < bands; ++i) {
        const Rows& r = band[static_cast<size_t>(i)];
        const long n = r.world_n + r.plane_n + r.above_n;
        if (n <= 0) continue;
        const double frame_sum = r.world_sum + r.plane_sum;
        const long frame_n = r.world_n + r.plane_n;
        char w[16], pl[16], fr[16], af[16], fa[16];
        auto put = [](char* dst, double sum, long cnt) {
            if (cnt > 0) std::snprintf(dst, 16, "%6.1f", sum / cnt);
            else std::snprintf(dst, 16, "     -");
        };
        put(w, r.world_sum, r.world_n);
        put(pl, r.plane_sum, r.plane_n);
        put(fr, frame_sum, frame_n);
        put(af, r.after_sum, r.world_n);
        put(fa, r.after_sum + r.plane_sum, frame_n);
        std::printf("  %4d  %5.0f%%   %s  %s  %s   %5.1f%%  %s  %s%s%s\n",
                    i, 100.0 * i / bands, w, pl, fr,
                    100.0 * static_cast<double>(r.world_n) / static_cast<double>(n), af, fa,
                    i == feet_band ? "   <- the feet" : "",
                    r.above_n > 0 ? "   (above the horizon, not sampled)" : "");
    }
}

} // namespace

int main(int argc, char** argv) {
    int mw = 0, mh = 0, aw = 0, ah = 0;
    std::vector<uint32_t> mat_px, alb_px;
    if (!read_bmp24("assets/test_material.bmp", mw, mh, mat_px)) return 1;
    if (!read_bmp24("assets/test_albedo.bmp", aw, ah, alb_px)) return 1;
    if (mw != aw || mh != ah) { std::fprintf(stderr, "scene BMP sizes differ\n"); return 1; }

    Scene scene;
    scene.width = mw;
    scene.height = mh;
    scene.materials.assign(static_cast<size_t>(mw) * mh, ElementType::Empty);
    scene.albedo.assign(static_cast<size_t>(mw) * mh, 0);
    for (size_t i = 0; i < mat_px.size(); ++i) {
        ElementType t = ElementType::Empty;
        element_from_legend(mat_px[i], t);
        scene.materials[i] = t;
        scene.albedo[i] = alb_px[i];
    }

    Grid g(mw, mh, 1234);
    const int placed = load_scene(g, scene, 0, 0);
    std::printf("plane_probe: %dx%d scene, %d cells placed, viewport %dx%d cells\n",
                mw, mh, placed, VIEW_W, VIEW_H);

    // Where the player comes to rest at the spawn column, found the way boot.h's
    // planter finds ground: the highest first-solid row across the body's footprint,
    // since that is the one the body lands on first. An argument overrides the
    // column, because which column the body lands in is exactly what the reading
    // turns on.
    const int spawn_x = argc > 1 ? std::atoi(argv[1]) : mw / 2;
    int feet = mh;
    for (int x = spawn_x; x < spawn_x + Player::WIDTH && x < mw; ++x) {
        for (int y = 0; y < mh; ++y) {
            if (is_solid(g.get_element(x, y).type)) { if (y < feet) feet = y; break; }
        }
    }
    if (feet >= mh) { std::fprintf(stderr, "the spawn column is open all the way down\n"); return 1; }

    const float cx = static_cast<float>(spawn_x) + Player::WIDTH / 2.0f;
    const float cy = static_cast<float>(feet) - Player::HEIGHT / 2.0f;
    sample(g, cx, cy, feet, "at the spawn, standing:");

    // The same reading at three heights of the flight the tester actually flies,
    // because a composition that only works at one camera position is a defect
    // rather than a fix.
    for (int up : {60, 160, 320}) {
        char label[96];
        std::snprintf(label, sizeof(label), "%d cells above the spawn:", up);
        sample(g, cx, cy - static_cast<float>(up), feet, label);
    }

    // The value ladder at the spawn. The tile is read here rather than inside the
    // function so a missing backdrop fails once, next to the other two asset loads.
    int gw = 0, gh = 0;
    std::vector<uint32_t> ground_px;
    if (read_bmp24("assets/backdrop_ground.bmp", gw, gh, ground_px)) {
        band_ladder(g, cx, cy, feet, ground_px, gw, gh, 20);
    }

    // A column walk at the spawn, for the question the shares cannot answer: where
    // the world stops.
    std::printf("\nwhat stands between the feet and the bottom of the window:\n");
    Camera camera;
    camera.follow(cx, cy, VIEW_W, VIEW_H, g.get_width(), g.get_height());
    for (int i = 0; i < 8; ++i) {
        const int wx = camera.view_x() + i * VIEW_W / 8;
        int solid_rows = 0;
        for (int vy = 0; vy < VIEW_H; ++vy) {
            const int wy = camera.view_y() + vy;
            if (wy > feet && wx >= 0 && wx < mw && wy >= 0 && wy < mh &&
                is_solid(g.get_element(wx, wy).type)) solid_rows++;
        }
        std::printf("  column %5d: %3d solid rows below the feet\n", wx, solid_rows);
    }
    return 0;
}
