#include "render/cell_pack.h"

#include <algorithm>

namespace cell_pack {

void CellPack::pack(const Grid& grid, int x0, int y0, int w, int h,
                    std::span<const frame::HiddenCell> hidden) {
    pack(grid.cell_data(), grid.get_width(), grid.get_height(), x0, y0, w, h, hidden);
}

void CellPack::pack(const Element* cells, int grid_w, int grid_h, int x0, int y0, int w, int h,
                    std::span<const frame::HiddenCell> hidden) {
    if (w <= 0 || h <= 0) {
        buf_w = buf_h = 0;
        buf.clear();
        return;
    }
    buf_w = w;
    buf_h = h;
    // resize, not assign: a window the size of last frame's writes nothing here,
    // and every texel is written below anyway. Allocates only on growth.
    buf.resize(static_cast<size_t>(w) * static_cast<size_t>(h));

    // The columns of the window that are on the grid, the same for every row:
    // [lo, hi) in window coordinates. Everything either side is padding. Clamped
    // so a window wholly off one side gives an empty range rather than a negative
    // one.
    const int lo = std::clamp(-x0, 0, w);
    const int hi = std::clamp(grid_w - x0, lo, w);

    for (int wy = 0; wy < h; ++wy) {
        uint32_t* out = buf.data() + static_cast<size_t>(wy) * static_cast<size_t>(w);
        const int gy = y0 + wy;
        if (gy < 0 || gy >= grid_h) {
            std::fill_n(out, w, PAD_TEXEL);
            continue;
        }
        std::fill_n(out, lo, PAD_TEXEL);
        const Element* row = cells + static_cast<size_t>(gy) * static_cast<size_t>(grid_w);
        for (int wx = lo; wx < hi; ++wx) out[wx] = texel_of(row[x0 + wx]);
        std::fill_n(out + hi, w - hi, PAD_TEXEL);
    }

    // Hidden cells over the top. A few hundred at most (one loose body's worth
    // each), so a pass of their own is cheaper than a lookup per cell above.
    const int total = grid_w * grid_h;
    for (const frame::HiddenCell& c : hidden) {
        if (c.index < 0 || c.index >= total) continue;
        // Cell index to column and row; the division is meant to truncate.
        const int wx = c.index % grid_w - x0, wy = c.index / grid_w - y0;
        if (wx < 0 || wy < 0 || wx >= w || wy >= h) continue;
        buf[static_cast<size_t>(wy) * static_cast<size_t>(w) + static_cast<size_t>(wx)] =
            hidden_texel(c.fill_type, cells[c.index].temperature);
    }
}

} // namespace cell_pack
