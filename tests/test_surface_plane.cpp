// The pass that gives the near terrain the plane's own value at its own depth.
//
// This suite exists because golden_frame_test cannot see the thing it is
// testing: the pass runs between the grid's pixel buffer and SDL_UpdateTexture,
// and the golden frame is composed from a texture that is already uploaded -- so
// the checksum does not move when this lands and will not move if it breaks. The
// alternative was to grow a second compositor inside the golden test, which is
// refused. So the coverage is here, headless, against the arithmetic itself.
//
// The three properties at the bottom are the point of the file. The named cases
// above them are the ones a person would think to write; the properties are what
// would catch the pass writing outside its buffer, or brightening a cell that is
// nowhere near a surface, at a camera position nobody types by hand.

#include "render/surface_plane.h"
#include "test_util.h"

#include <string>
#include <vector>

using surface_plane::DEPTH_END;
using surface_plane::FULL_END;
using surface_plane::SKIN_CELLS;
using surface_plane::TileRows;
using surface_plane::depth_map;
using surface_plane::View;
using surface_plane::weight_at_depth;

namespace {

constexpr uint32_t EMPTY = 0x00000000u;
constexpr uint32_t ROCK  = 0xFF201810u;  // a dark world cell, near the fixture's

inline int lum(uint32_t px) {
    // The same crude luminance plane_probe reports in, so a number here and a
    // number there mean the same thing.
    const int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
    return (r * 30 + g * 59 + b * 11) / 100;
}

// A grid with a flat terrain surface at row `surface`, open above it, solid to
// the bottom.
std::vector<uint32_t> flat_world(int w, int h, int surface) {
    std::vector<uint32_t> g(static_cast<size_t>(w) * h, EMPTY);
    for (int y = surface; y < h; ++y)
        for (int x = 0; x < w; ++x) g[static_cast<size_t>(y) * w + x] = ROCK;
    return g;
}

// A tile whose rows run dark at the top (the far edge) to bright at the bottom
// (the near edge), which is the shipped tile's direction.
std::vector<uint8_t> ramp_tile(int rows) {
    std::vector<uint8_t> t(static_cast<size_t>(rows) * 3);
    for (int y = 0; y < rows; ++y) {
        const uint8_t v = static_cast<uint8_t>((y * 255) / (rows - 1));
        t[static_cast<size_t>(y) * 3 + 0] = v;
        t[static_cast<size_t>(y) * 3 + 1] = v;
        t[static_cast<size_t>(y) * 3 + 2] = v;
    }
    return t;
}

std::string n(int v) { return std::to_string(v); }

} // namespace

int main() {
    // --- the depth ramp -----------------------------------------------------
    check("the exposed surface cell keeps its own colour", weight_at_depth(0) == 0,
          n(weight_at_depth(0)));
    check("full strength arrives at the end of the skin",
          weight_at_depth(SKIN_CELLS) == 255, n(weight_at_depth(SKIN_CELLS)));
    check("and holds to the end of the full band",
          weight_at_depth(FULL_END) == 255, n(weight_at_depth(FULL_END)));
    check("the fade reaches zero exactly at DEPTH_END",
          weight_at_depth(DEPTH_END) == 0 && weight_at_depth(DEPTH_END - 1) > 0,
          n(weight_at_depth(DEPTH_END - 1)) + " then " + n(weight_at_depth(DEPTH_END)));
    check("a cell with no surface above it is untouched", weight_at_depth(-1) == 0);

    // Monotone up then down, with no step bigger than one skin's worth. A ramp that
    // reversed anywhere would put a band of the wrong value across the world at a
    // fixed depth, which is the defect the fade exists to avoid.
    {
        bool rising = true, ok = true;
        int prev = weight_at_depth(0);
        for (int d = 1; d <= DEPTH_END; ++d) {
            const int w = weight_at_depth(d);
            if (rising && w < prev) rising = false;
            if (!rising && w > prev) { ok = false; break; }
            prev = w;
        }
        check("the weight ramp rises once and falls once", ok);
    }

    // --- the depth map -----------------------------------------------------
    //
    // The quantity here is distance to the air directly above, not the row of the
    // column's topmost cell. Measured from the top of the column, every cell under a
    // trunk or an overhang comes out past DEPTH_END and keeps its raw colour --
    // invisible in every case a person would have thought to write. The overhang
    // case below is that defect, pinned.
    {
        const int W = 8, H = 100 + DEPTH_END + 120;
        const std::vector<uint32_t> g = flat_world(W, H, 100);
        const View v{0, 90, W, 60};
        std::vector<int> d(static_cast<size_t>(v.w) * v.h, -99);
        depth_map(g.data(), W, H, v, d.data());
        auto at = [&](int x, int y) { return d[static_cast<size_t>(y) * v.w + x]; };

        check("air is -1", at(3, 0) == -1 && at(3, 9) == -1, n(at(3, 9)));
        check("the first cell of matter is depth 0", at(3, 10) == 0, n(at(3, 10)));
        check("and it counts down from there", at(3, 20) == 10, n(at(3, 20)));

        // A window whose top is more than DEPTH_END below the surface cannot know
        // how deep it is, and says so with the value that has weight 0.
        const View deep{0, 100 + DEPTH_END + 40, W, 20};
        std::vector<int> dd(static_cast<size_t>(deep.w) * deep.h, -99);
        depth_map(g.data(), W, H, deep, dd.data());
        check("a window far below the surface reports the zero-weight depth",
              dd[0] == DEPTH_END && weight_at_depth(dd[0]) == 0, n(dd[0]));
    }

    // --- the row scale, which is the other half of the bound -----------------
    //
    // Depth says whether a cell is near-surface ground; this says whether the window
    // row is on the plane at all. Underground it is the only one of the two that can
    // answer, because every cell down there is deep and every screen row is past the
    // plane's near edge.
    check("a row on the plane is at full scale",
          surface_plane::row_scale_at(0.0f) == 255 &&
          surface_plane::row_scale_at(1.0f) == 255);
    check("a row above the horizon is off", surface_plane::row_scale_at(-0.01f) == 0);
    check("and one well past the near edge is off",
          surface_plane::row_scale_at(2.0f) == 0 &&
          surface_plane::row_scale_at(40.0f) == 0);
    {
        // It has to fade rather than cut, or the pass draws a horizontal line across
        // the terrain wherever the near edge lands.
        const int mid = surface_plane::row_scale_at(1.12f);
        check("and it fades across the quarter-depth past it", mid > 0 && mid < 255, n(mid));
    }

    // An overhang: a slab floating high above the ground, with open air between.
    // Every cell of the ground below it must still measure from its own air.
    {
        const int W = 8, H = 600;
        std::vector<uint32_t> g = flat_world(W, H, 500);
        for (int y = 100; y < 104; ++y)  // the trunk, far above the ground
            for (int x = 2; x < 4; ++x) g[static_cast<size_t>(y) * W + x] = ROCK;

        const View v{0, 480, W, 60};
        std::vector<int> d(static_cast<size_t>(v.w) * v.h, -99);
        depth_map(g.data(), W, H, v, d.data());
        auto at = [&](int x, int y) { return d[static_cast<size_t>(y) * v.w + x]; };
        const int gy20 = 520 - v.view_y;
        check("a column under an overhang measures from its own air, not the slab",
              at(3, gy20) == at(6, gy20) && at(3, gy20) == 20,
              "under slab " + n(at(3, gy20)) + ", clear " + n(at(6, gy20)));
    }

    // A cave: air below matter resets the count, so the cave's floor is a surface of
    // its own. Without the reset the roof's depth would leak down through the void
    // and the floor would read as deep rock.
    {
        const int W = 4, H = 400;
        std::vector<uint32_t> g = flat_world(W, H, 100);
        for (int y = 140; y < 160; ++y)
            for (int x = 0; x < W; ++x) g[static_cast<size_t>(y) * W + x] = EMPTY;
        const View v{0, 120, W, 80};
        std::vector<int> d(static_cast<size_t>(v.w) * v.h, -99);
        depth_map(g.data(), W, H, v, d.data());
        auto at = [&](int x, int y) { return d[static_cast<size_t>(y) * v.w + x]; };
        check("a cave roof keeps counting from the terrain surface",
              at(0, 139 - v.view_y) == 39, n(at(0, 139 - v.view_y)));
        check("the cave's void is air", at(0, 150 - v.view_y) == -1);
        check("and its floor is a surface of its own",
              at(0, 160 - v.view_y) == 0, n(at(0, 160 - v.view_y)));
    }

    // --- the pass -----------------------------------------------------------
    //
    // The shape of the spawn frame, reduced: terrain surface partway down the
    // window, the plane's rows running dark-to-bright toward the viewer, and the
    // whole window on the plane.
    {
        const int GW = 64, GH = 400, TILE = 64;
        const int SURF = 200;
        const std::vector<uint32_t> g = flat_world(GW, GH, SURF);
        const std::vector<uint8_t> tile = ramp_tile(TILE);

        const View v{0, SURF - 20, GW, 120};
        std::vector<int> src(static_cast<size_t>(v.h));
        for (int i = 0; i < v.h; ++i) src[static_cast<size_t>(i)] = (i * (TILE - 1)) / (v.h - 1);

        std::vector<uint32_t> out(static_cast<size_t>(v.w) * v.h, 0xDEADBEEFu);
        std::vector<int> scratch(static_cast<size_t>(v.w) * v.h, -1);
        surface_plane::apply(g.data(), GW, GH, v, TileRows{tile.data(), TILE},
                             src.data(), nullptr, 255, 255, 255, scratch.data(), out.data());

        auto at = [&](int wx, int wy) { return out[static_cast<size_t>(wy) * v.w + wx]; };

        check("open sky above the surface is copied through untouched",
              at(0, 0) == EMPTY && at(GW - 1, 5) == EMPTY);

        // The band the whole item is about: rows below the feet must rise toward the
        // viewer, where without the pass they are flat ROCK.
        const int surf_wy = SURF - v.view_y;
        const int a = lum(at(0, surf_wy + SKIN_CELLS));
        const int b = lum(at(0, surf_wy + 60));
        const int c = lum(at(0, v.h - 1));
        check("the near terrain rises toward the viewer instead of staying flat",
              a < b && b < c, n(lum(ROCK)) + " flat -> " + n(a) + " / " + n(b) + " / " + n(c));
        check("and every one of those rows is brighter than the raw cell",
              a > lum(ROCK) && c > lum(ROCK), n(lum(ROCK)) + " -> " + n(c));

        check("the cell at the surface itself is still its own material",
              at(0, surf_wy) == ROCK);

        // Alpha is identity, always: the alpha byte is what lets a dug hole show the
        // backdrop, and a pass that touched it would fill the world in.
        {
            bool ok = true;
            for (uint32_t px : out) if ((px >> 24) != 0u && (px >> 24) != 0xFFu) { ok = false; break; }
            check("alpha is never altered", ok);
        }

        // A window row that is not on the plane is a straight copy.
        {
            std::vector<int> none(static_cast<size_t>(v.h), -1);
            std::vector<uint32_t> plain(static_cast<size_t>(v.w) * v.h, 0u);
            surface_plane::apply(g.data(), GW, GH, v, TileRows{tile.data(), TILE},
                                 none.data(), nullptr, 255, 255, 255, scratch.data(), plain.data());
            bool same = true;
            for (int wy = 0; wy < v.h && same; ++wy)
                for (int wx = 0; wx < v.w; ++wx)
                    if (plain[static_cast<size_t>(wy) * v.w + wx] !=
                        g[static_cast<size_t>(v.view_y + wy) * GW + wx]) { same = false; break; }
            check("a window off the plane is the copy the upload used to do", same);
        }

        // A missing tile disables the pass rather than colouring the world with
        // whatever was in the buffer. This is the case main.cpp warns about at
        // startup, and it must degrade to the old frame exactly.
        {
            std::vector<uint32_t> plain(static_cast<size_t>(v.w) * v.h, 0u);
            surface_plane::apply(g.data(), GW, GH, v, TileRows{nullptr, 0},
                                 src.data(), nullptr, 255, 255, 255, scratch.data(), plain.data());
            bool same = true;
            for (int wy = 0; wy < v.h && same; ++wy)
                for (int wx = 0; wx < v.w; ++wx)
                    if (plain[static_cast<size_t>(wy) * v.w + wx] !=
                        g[static_cast<size_t>(v.view_y + wy) * GW + wx]) { same = false; break; }
            check("no tile means an untextured frame, not a coloured one", same);
        }

        // The grade is applied here rather than by SDL, because the cell texture
        // carries the world's own row and not the ground's. Halving it must halve
        // what the deepest row converges on.
        {
            std::vector<uint32_t> graded(static_cast<size_t>(v.w) * v.h, 0u);
            surface_plane::apply(g.data(), GW, GH, v, TileRows{tile.data(), TILE},
                                 src.data(), nullptr, 128, 128, 128, scratch.data(), graded.data());
            const int full = lum(at(0, v.h - 1));
            const int half = lum(graded[static_cast<size_t>(v.h - 1) * v.w]);
            check("the ground grade darkens what the blend converges on", half < full,
                  n(full) + " at 255 -> " + n(half) + " at 128");
        }
    }

    // --- properties ---------------------------------------------------------
    //
    // A dug hole is the one interaction this pass has with the game's only verb, so
    // it gets a property rather than a case: whatever the depth ramp does, an Empty
    // cell must come out Empty, or digging stops showing what is behind it.
    {
        const int GW = 32, GH = 300, TILE = 32;
        std::vector<uint32_t> g = flat_world(GW, GH, 100);
        for (int y = 100; y < 160; ++y)  // a shaft, six cells wide
            for (int x = 10; x < 16; ++x) g[static_cast<size_t>(y) * GW + x] = EMPTY;
        const std::vector<uint8_t> tile = ramp_tile(TILE);
        const View v{0, 90, GW, 160};
        std::vector<int> src(static_cast<size_t>(v.h));
        for (int i = 0; i < v.h; ++i) src[static_cast<size_t>(i)] = (i * (TILE - 1)) / (v.h - 1);
        std::vector<uint32_t> out(static_cast<size_t>(v.w) * v.h, 0u);
        std::vector<int> scratch(static_cast<size_t>(v.w) * v.h, -1);
        surface_plane::apply(g.data(), GW, GH, v, TileRows{tile.data(), TILE},
                             src.data(), nullptr, 255, 255, 255, scratch.data(), out.data());

        bool holes_kept = true;
        for (int wy = 0; wy < v.h && holes_kept; ++wy)
            for (int wx = 0; wx < v.w; ++wx) {
                const uint32_t in = g[static_cast<size_t>(v.view_y + wy) * GW + wx];
                const uint32_t got = out[static_cast<size_t>(wy) * v.w + wx];
                if ((in == EMPTY) != (got == EMPTY)) { holes_kept = false; break; }
            }
        check("a dug shaft stays open through the pass", holes_kept);

        // The shaft's own floor is a surface, so the columns through it are
        // measured from theirs rather than from the terrain far above. The point of
        // the property is that nothing crashes or reads out of bounds when a
        // column's surface is not the terrain's.
        std::vector<int> d(static_cast<size_t>(v.w) * v.h, -99);
        depth_map(g.data(), GW, GH, v, d.data());
        check("a shaft's floor is a surface of its own",
              d[static_cast<size_t>(160 - v.view_y) * v.w + 12] == 0,
              n(d[static_cast<size_t>(160 - v.view_y) * v.w + 12]));
    }

    // Sweeping the window down through a world: the pass must never write outside
    // `out`, and must leave the guard word past the end alone.
    {
        const int GW = 24, GH = 500, TILE = 16;
        const std::vector<uint32_t> g = flat_world(GW, GH, 120);
        const std::vector<uint8_t> tile = ramp_tile(TILE);
        bool ok = true;
        for (int top = 0; top + 60 < GH; top += 7) {
            const View v{0, top, GW, 60};
            std::vector<int> src(static_cast<size_t>(v.h));
            for (int i = 0; i < v.h; ++i) src[static_cast<size_t>(i)] = i % TILE;
            std::vector<uint32_t> out(static_cast<size_t>(v.w) * v.h + 1, 0u);
            out.back() = 0xA5A5A5A5u;
            std::vector<int> scratch(static_cast<size_t>(v.w) * v.h, -1);
            surface_plane::apply(g.data(), GW, GH, v, TileRows{tile.data(), TILE},
                                 src.data(), nullptr, 255, 255, 255, scratch.data(), out.data());
            if (out.back() != 0xA5A5A5A5u) { ok = false; break; }
        }
        check("sweeping the window never writes past the buffer", ok);
    }

    // --- the aliased run buffer ---------------------------------------------
    //
    // apply hands depth_map the last row of its own scratch as the running counter,
    // which is only correct because the pass writes each output row once, top to
    // bottom. Nothing else in this suite exercises that path -- the cases above all
    // pass nullptr and get a private buffer -- so the aliasing is pinned here
    // against the unaliased answer, sweeping the window so the partly-off-grid and
    // far-below-surface shapes are both covered.
    {
        const int GW = 24, GH = 500;
        const std::vector<uint32_t> g = flat_world(GW, GH, 120);
        bool same = true;
        for (int top = -20; top + 60 < GH && same; top += 7) {
            const View v{0, top, GW, 60};
            const size_t n_cells = static_cast<size_t>(v.w) * v.h;
            std::vector<int> plain(n_cells, -99), aliased(n_cells, -99);
            depth_map(g.data(), GW, GH, v, plain.data());
            depth_map(g.data(), GW, GH, v, aliased.data(),
                      aliased.data() + static_cast<size_t>(v.h - 1) * v.w);
            same = (plain == aliased);
        }
        check("the aliased run buffer gives the same depths as a private one", same);
    }

    return report();
}
