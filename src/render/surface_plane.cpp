#include "render/surface_plane.h"

#include <algorithm>
#include <vector>

namespace surface_plane {

namespace {

// The grid stores Empty as 0x00000000 -- the one row in MATERIALS whose colour
// is fully transparent. So the alpha byte is the whole test for "is there matter
// here", and it is the same test the renderer relies on to let the backdrop show
// through.
inline bool is_matter(uint32_t px) { return (px >> 24) != 0u; }

inline uint8_t blend(uint8_t from, int to, int w) {
    return static_cast<uint8_t>((static_cast<int>(from) * (255 - w) + to * w) / 255);
}

} // namespace

void average_rows(const uint32_t* pixels, int w, int h, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(h > 0 ? h : 0) * 3, 0);
    if (!pixels || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; ++y) {
        uint32_t r = 0, g = 0, b = 0;
        const uint32_t* row = pixels + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            r += (row[x] >> 16) & 0xFF;
            g += (row[x] >> 8) & 0xFF;
            b += row[x] & 0xFF;
        }
        out[static_cast<size_t>(y) * 3 + 0] = static_cast<uint8_t>(r / static_cast<uint32_t>(w));
        out[static_cast<size_t>(y) * 3 + 1] = static_cast<uint8_t>(g / static_cast<uint32_t>(w));
        out[static_cast<size_t>(y) * 3 + 2] = static_cast<uint8_t>(b / static_cast<uint32_t>(w));
    }
}

void depth_map(const uint32_t* grid_pixels, int grid_w, int grid_h,
               const View& v, int* depth, int* run_scratch) {
    if (!grid_pixels || !depth || v.w <= 0 || v.h <= 0) return;

    int start = v.view_y - DEPTH_END;
    if (start < 0) start = 0;

    // The running counter, one per visible column. DEPTH_END doubles as "at
    // least this deep", which is all the caller can use it for.
    //
    // Caller-owned when it can be: this runs once a frame at window width, and
    // a fresh vector each time is an allocation on the render path for a
    // buffer whose contents never outlive the call.
    std::vector<int> local_run;
    int* run = run_scratch;
    if (run == nullptr) {
        local_run.assign(static_cast<size_t>(v.w), -1);
        run = local_run.data();
    } else {
        std::fill(run, run + v.w, -1);
    }

    for (int y = start; y < v.view_y + v.h; ++y) {
        if (y < 0 || y >= grid_h) {
            if (y >= v.view_y) {
                int* row = depth + static_cast<size_t>(y - v.view_y) * v.w;
                for (int x = 0; x < v.w; ++x) row[x] = -1;
            }
            continue;
        }
        const uint32_t* src = grid_pixels + static_cast<size_t>(y) * grid_w;
        for (int x = 0; x < v.w; ++x) {
            const int gx = v.view_x + x;
            const bool matter = gx >= 0 && gx < grid_w && is_matter(src[gx]);
            int& r = run[x];
            if (!matter) r = -1;
            else if (y == start) r = DEPTH_END;  // air above never looked at
            else if (r < 0) r = 0;  // this cell is the surface
            else if (r < DEPTH_END) r = r + 1;
        }
        if (y >= v.view_y) {
            int* row = depth + static_cast<size_t>(y - v.view_y) * v.w;
            for (int x = 0; x < v.w; ++x) row[x] = run[x];
        }
    }
}

void apply(const uint32_t* grid_pixels, int grid_w, int grid_h,
           const View& v,
           const TileRows& rows, const int* src_row_for, const int* row_scale,
           int grade_r, int grade_g, int grade_b,
           int* scratch, uint32_t* out) {
    if (!grid_pixels || !out || !scratch || v.w <= 0 || v.h <= 0) return;

    // One pass over the window, before any pixel is touched. See the note at
    // depth_map.
    //
    // The run buffer is the last row of `scratch`, which costs nothing and is safe
    // for one reason only: depth_map writes each output row on the single pass that
    // produces it, top to bottom, so row v.h - 1 is not touched until the final
    // iteration -- at which point that row's copy is `run` to itself and `run` is
    // dead immediately after. The aliasing is exact, not approximate, and it is why
    // depth_map's loop must stay top-to-bottom and must keep writing each row once.
    // Anything that reorders it has to give this its own buffer.
    depth_map(grid_pixels, grid_w, grid_h, v, scratch,
              scratch + static_cast<size_t>(v.h - 1) * v.w);

    for (int wy = 0; wy < v.h; ++wy) {
        const int gy = v.view_y + wy;
        uint32_t* dst = out + static_cast<size_t>(wy) * v.w;

        // Rows off the grid are cleared rather than copied from nowhere. The
        // camera clamps, so this is a guard and not a case the game reaches.
        if (gy < 0 || gy >= grid_h) {
            for (int wx = 0; wx < v.w; ++wx) dst[wx] = 0u;
            continue;
        }
        const uint32_t* src = grid_pixels + static_cast<size_t>(gy) * grid_w + v.view_x;

        // A row this window row does not meet the plane on is a straight copy,
        // which is what the upload did before this pass existed -- so the
        // untouched case costs one copy loop and nothing else.
        const int tile_row = src_row_for ? src_row_for[wy] : -1;
        const int scale = row_scale ? row_scale[wy] : 255;
        if (tile_row < 0 || tile_row >= rows.count || !rows.rgb || scale <= 0) {
            for (int wx = 0; wx < v.w; ++wx) dst[wx] = src[wx];
            continue;
        }

        // The plane's colour for this whole window row, computed once. Decision 2
        // is what makes that legal: nothing here varies with x.
        const uint8_t* t = rows.rgb + static_cast<size_t>(tile_row) * 3;
        const int pr = (static_cast<int>(t[0]) * grade_r) / 255;
        const int pg = (static_cast<int>(t[1]) * grade_g) / 255;
        const int pb = (static_cast<int>(t[2]) * grade_b) / 255;

        for (int wx = 0; wx < v.w; ++wx) {
            const uint32_t px = src[wx];
            if (!is_matter(px)) { dst[wx] = px; continue; }

            const int w = weight_at_depth(scratch[static_cast<size_t>(wy) * v.w + wx]) *
                          scale / 255;
            if (w <= 0) { dst[wx] = px; continue; }

            const uint8_t r = blend(static_cast<uint8_t>((px >> 16) & 0xFF), pr, w);
            const uint8_t g = blend(static_cast<uint8_t>((px >> 8) & 0xFF), pg, w);
            const uint8_t b = blend(static_cast<uint8_t>(px & 0xFF), pb, w);
            dst[wx] = (px & 0xFF000000u) | (static_cast<uint32_t>(r) << 16) |
                      (static_cast<uint32_t>(g) << 8) | b;
        }
    }
}

} // namespace surface_plane
