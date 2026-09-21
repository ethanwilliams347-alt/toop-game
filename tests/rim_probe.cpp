// How much of the authored rim highlight survives the world settling, in
// numbers. Not a registered test, for the same reason the other probes are not:
// what it reports is an input to a taste decision.
//
// The rim light (tools/pixel_art.py's apply_rim_light) is baked into each cell's
// colour, so a lit top edge travels with the cell. That is obviously fine for
// Wall and Wood, which never move. It is not obviously fine for Sand, which is
// authored as a blocky slope and slumps the moment the simulation starts, or for
// Water. If a large share of the rim ends up somewhere that is no longer a
// surface, the highlight reads as scattered noise through the body of the
// material and the rim should be restricted to static materials. If almost all
// of it stays put, the concern is theoretical.
//
// Two numbers per material, and both are needed to decide:
//   SMEAR - rim-coloured cells that no longer have Empty above them. The
//           highlight ending up inside the material.
//   GAPS  - surface cells that carry no rim colour. A rim that stays put but
//           whose surface has moved out from under it leaves the new surface
//           unlit, which reads as an eaten-away edge.
#include "physics/grid.h"
#include "scene/scene.h"
#include "scene/legend.h"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

// The two rim colours from tools/pixel_art.py's PALETTE, as the ARGB the scene
// loader stores (alpha forced opaque, same as main.cpp does).
constexpr uint32_t RIM_GRASS = 0xFF69783A;
constexpr uint32_t RIM_WATER = 0xFF2E4955;

// Minimal 24-bit BMP reader. Hand-rolled rather than reached for through SDL
// because this probe is headless like the suites are. It only has to read what
// tools/pixel_art.py writes: 24bpp, uncompressed, top-down.
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

    auto u32 = [&](size_t o) { return static_cast<uint32_t>(buf[o]) | (buf[o+1] << 8) | (buf[o+2] << 16) | (static_cast<uint32_t>(buf[o+3]) << 24); };
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

bool is_rim(uint32_t c) { return c == RIM_GRASS || c == RIM_WATER; }

struct Tally { int rim_total = 0, rim_on_surface = 0, rim_displaced = 0, surface_total = 0, surface_rimmed = 0; };

// A cell is on a surface if the cell directly above it is Empty -- the same test
// apply_rim_light used to decide where to put the highlight in the first place,
// so the before and after are measured against one definition.
void measure(const Grid& g, Tally by_type[static_cast<int>(ElementType::Count)]) {
    for (int y = 0; y < g.get_height(); ++y) {
        for (int x = 0; x < g.get_width(); ++x) {
            const Element e = g.get_element(x, y);
            if (e.type == ElementType::Empty) continue;
            const bool surface = (y == 0) || g.get_element(x, y - 1).type == ElementType::Empty;
            Tally& t = by_type[static_cast<int>(e.type)];
            if (is_rim(e.color)) {
                t.rim_total++;
                if (surface) {
                    t.rim_on_surface++;
                } else {
                    // A rim cell sitting directly under another rim cell is the dithered
                    // second row, not a displaced highlight. apply_rim_light writes a band
                    // `rim_depth` deep, so a sizeable share of all rim cells are below the
                    // surface by design -- counting those as smear reports a third of the
                    // rim damaged before a single step has run.
                    const Element above = g.get_element(x, y - 1);
                    if (!(y > 0 && above.type != ElementType::Empty && is_rim(above.color)))
                        t.rim_displaced++;
                }
            }
            if (surface) { t.surface_total++; if (is_rim(e.color)) t.surface_rimmed++; }
        }
    }
}

void report(const char* label, Tally by_type[static_cast<int>(ElementType::Count)]) {
    std::printf("\n%s\n", label);
    std::printf("  %-9s %6s %8s %7s   %8s %8s %7s\n",
                "material", "rim", "displaced", "SMEAR", "surface", "rimmed", "GAPS");
    for (int i = 0; i < static_cast<int>(ElementType::Count); ++i) {
        const Tally& t = by_type[i];
        if (t.rim_total == 0 && t.surface_total == 0) continue;
        const double smear = t.rim_total ? 100.0 * t.rim_displaced / t.rim_total : 0.0;
        const double gaps = t.surface_total ? 100.0 * (t.surface_total - t.surface_rimmed) / t.surface_total : 0.0;
        std::printf("  %-9s %6d %8d %6.1f%%   %8d %8d %6.1f%%\n",
                    MATERIALS[i].name, t.rim_total, t.rim_displaced, smear,
                    t.surface_total, t.surface_rimmed, gaps);
    }
}

} // namespace

int main(int argc, char** argv) {
    const int steps = argc > 1 ? std::atoi(argv[1]) : 600;

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
    int unmatched = 0;
    for (size_t i = 0; i < mat_px.size(); ++i) {
        ElementType t = ElementType::Empty;
        if (!element_from_legend(mat_px[i], t)) ++unmatched;
        scene.materials[i] = t;
        scene.albedo[i] = alb_px[i];
    }
    if (unmatched) std::printf("WARNING: %d material pixels matched no legend entry\n", unmatched);

    Grid g(mw, mh, 1234);
    const int placed = load_scene(g, scene, 0, 0);
    std::printf("rim_probe: %dx%d scene, %d cells placed, settling for %d steps\n",
                mw, mh, placed, steps);

    Tally before[static_cast<int>(ElementType::Count)];
    measure(g, before);
    report("at load (t=0):", before);

    for (int i = 0; i < steps; ++i) g.update();

    Tally after[static_cast<int>(ElementType::Count)];
    measure(g, after);
    char label[128];
    std::snprintf(label, sizeof(label), "after %d steps (%d chunks still awake):",
                  steps, g.active_chunk_count());
    report(label, after);

    // --- phase 2: the case the settle test does not cover ---
    //
    // Settling alone barely disturbs the authored scene, which makes it the easy
    // half of the question. What the rim actually has to survive is a player, and
    // the two materials in doubt are exactly the two a player interacts with most:
    // dig the snowbank and the slope slumps into the hole; breach the channel wall
    // and the pool drains across the floor. Both are done through set_element, the
    // ordinary write path, so the wake rule applies exactly as it would to a dig.
    auto carve = [&](int cx, int cy, int r) {
        for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
                if (dx * dx + dy * dy <= r * r) g.set_element(cx + dx, cy + dy, ElementType::Empty);
    };
    // These are in world cells and must stay in step with the fixture. A
    // disturbance that misses reports a clean number rather than an error, so a
    // probe that perturbs the world should assert it changed something. Left as a
    // note rather than built, because what to assert is a design question.
    carve(200, 915, 22);  // straight through the snowbank
    carve(1505, 860, 18);  // breach the water channel's right wall
    for (int i = 0; i < steps; ++i) g.update();

    Tally disturbed[static_cast<int>(ElementType::Count)];
    measure(g, disturbed);
    std::snprintf(label, sizeof(label),
                  "after digging the snowbank + breaching the channel, %d more steps (%d awake):",
                  steps, g.active_chunk_count());
    report(label, disturbed);

    std::printf("\nSMEAR: share of rim cells that are neither on a surface nor part of the\n"
                "       authored dither band under one - i.e. highlight genuinely adrift\n"
                "       inside the material. This is the number the item turns on.\n"
                "GAPS:  share of surface cells carrying no rim - new top edge exposed by\n"
                "       movement, which reads as a broken highlight rather than as noise.\n");
    return 0;
}
