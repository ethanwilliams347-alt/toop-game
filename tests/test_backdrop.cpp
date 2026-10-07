// The wrapping backdrop layer's arithmetic.
//
// This suite links nothing. src/render/backdrop_wrap.h is a header of pure
// arithmetic precisely so that the part of a wrapping layer that is easy to get
// wrong can be checked without a window, the same split render/player_anim.cpp
// has with the sprite sheet.
//
// The two properties at the bottom are the point of the file. The named cases
// above them are the ones a person would think to write, and every one of them
// passes against an implementation that is wrong at a camera position nobody
// types by hand. The properties sweep positions the game actually reaches after
// a few minutes of walking -- hundreds of thousands of pixels from the origin,
// where a float's spacing is no longer 1 -- and they are what would catch a
// one-pixel gap at the window edge.

#include "render/backdrop_wrap.h"
#include "test_util.h"

#include <cmath>
#include <string>

using backdrop_wrap::wrap_axis;
using backdrop_wrap::Tiling;

namespace {

// Does the tiling actually cover [0, window)? `first` must not leave a strip at
// the near edge, and the last copy must reach past the far edge.
bool covers(Tiling t, int tile, int window) {
    if (t.first > 0.0f) return false;  // gap at the near edge
    if (t.first <= -static_cast<float>(tile)) return false;  // a wholly wasted copy
    return t.first + static_cast<float>(t.count) * tile >= static_cast<float>(window);
}

// Screen pixels, so the tolerance is a hundredth of one: these are float
// positions that get handed to SDL_FRect, not values anybody expects to be bit
// equal.
bool near_eq(float a, float b) { return std::fabs(a - b) < 0.01f; }

std::string describe(Tiling t) {
    return "first=" + std::to_string(t.first) + " count=" + std::to_string(t.count);
}

} // namespace

int main() {
    // --- the aligned case, which must not gain a redundant copy --------------
    //
    // first == 0 is legitimate and has to stay 0. An implementation that normalises
    // into [-tile, 0) instead of (-tile, 0] pushes it to -tile and draws one extra
    // copy entirely off-screen on every frame the layer happens to align. Invisible,
    // permanent, and exactly the kind of cost that is never found because nothing
    // looks wrong.
    {
        Tiling t = wrap_axis(0.0f, 256, 1920);
        check("aligned origin keeps first at 0", t.first == 0.0f, describe(t));
        check("aligned origin needs ceil(window/tile) copies", t.count == 8, describe(t));
    }

    // An exact multiple of the tile is the same state reached by walking.
    {
        Tiling t = wrap_axis(-2560.0f, 256, 1920);
        check("exact multiple wraps back to aligned", t.first == 0.0f && t.count == 8,
              describe(t));
    }

    // --- the ordinary cases -------------------------------------------------
    {
        Tiling t = wrap_axis(-10.0f, 256, 1920);
        check("small negative origin", t.first == -10.0f && t.count == 8, describe(t));
    }
    {
        // A positive origin happens whenever the camera is left of where the layer
        // was placed. fmod keeps its left operand's sign, so this is the branch that
        // needs the subtraction -- without it a column of the layer behind shows at
        // the window edge.
        Tiling t = wrap_axis(10.0f, 256, 1920);
        check("positive origin is pulled below zero", t.first == -246.0f, describe(t));
        check("positive origin still covers", covers(t, 256, 1920), describe(t));
    }
    {
        // Window not a whole number of tiles: the count has to round up.
        Tiling t = wrap_axis(0.0f, 500, 1920);
        check("ragged window rounds the count up", t.count == 4, describe(t));
    }
    {
        // A tile wider than the window is one copy, not zero.
        Tiling t = wrap_axis(-100.0f, 4096, 1920);
        check("tile wider than window is a single copy", t.count == 1, describe(t));
    }

    // --- degenerate input ---------------------------------------------------
    //
    // A zero-width tile is what a failed BMP load looks like from here. It must
    // produce zero copies rather than a division by zero or a loop that never ends
    // -- the same reasoning frame.cpp's draw_layer applies to a null texture.
    {
        Tiling t = wrap_axis(-10.0f, 0, 1920);
        check("zero-width tile tiles nothing", t.count == 0, describe(t));
    }

    // --- property 1: coverage, over the positions the game reaches ----------
    //
    // A near layer's origin sweeps tens of thousands of pixels across the world, and
    // a long session takes it further. Stepping in 7s and 13s rather than round
    // numbers is deliberate: a stride that shares a factor with the tile width only
    // ever visits the aligned cases.
    {
        bool all = true;
        std::string first_bad;
        const int tiles[] = {64, 256, 300, 512, 1024};
        const int windows[] = {1920, 2560, 3440};
        for (int tile : tiles) {
            for (int window : windows) {
                for (int i = -60000; i <= 60000; i += 7) {
                    const float origin = static_cast<float>(i) + 0.37f;
                    Tiling t = wrap_axis(origin, tile, window);
                    if (!covers(t, tile, window)) {
                        all = false;
                        if (first_bad.empty())
                            first_bad = "tile=" + std::to_string(tile) +
                                        " window=" + std::to_string(window) +
                                        " origin=" + std::to_string(origin) + " " +
                                        describe(t);
                    }
                }
            }
        }
        check("coverage holds across the pan range", all, first_bad);
    }

    // --- property 2: no copy is wasted --------------------------------------
    //
    // Coverage alone is satisfied by drawing a hundred copies. The count must also
    // be minimal, because this runs every frame for every wrapping band and the
    // whole argument for wrapping layers is that they are cheap.
    {
        bool all = true;
        std::string first_bad;
        for (int tile : {64, 256, 300, 512}) {
            for (int i = -40000; i <= 40000; i += 13) {
                const float origin = static_cast<float>(i) + 0.5f;
                Tiling t = wrap_axis(origin, tile, 1920);
                Tiling fewer{t.first, t.count - 1};
                if (covers(fewer, tile, 1920)) {
                    all = false;
                    if (first_bad.empty())
                        first_bad = "tile=" + std::to_string(tile) +
                                    " origin=" + std::to_string(origin) + " " + describe(t);
                }
            }
        }
        check("the copy count is minimal", all, first_bad);
    }

    return report();
}
