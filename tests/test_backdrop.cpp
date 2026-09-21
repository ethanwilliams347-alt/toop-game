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
using backdrop_wrap::Plane;
using backdrop_wrap::Strip;
using backdrop_wrap::plane_strip;

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
    // -- the same reasoning draw_backdrop_layer applies to a null texture.
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

    // --- the ground plane strips -----------------------------------------
    //
    // The half of the plane that does not need a window. Four properties, and the
    // reason they are properties rather than a table of expected numbers is that the
    // strip arithmetic has no right answer to assert against -- it has relationships
    // that must hold, and any set of them that does is a legitimate tuning.
    //
    // The fixture is the shipped geometry.
    const Plane PLANE{594.0f, 1080.0f, 0.28f, 0.52f, 256};
    constexpr int N = 24;

    // Property 3: the strips tile the band exactly. A gap is a row of whatever was
    // drawn behind the plane showing through it; an overlap is a row drawn twice at
    // two different depths, which on a graded layer is a visible seam.
    {
        bool contiguous = true;
        std::string first_bad;
        for (int i = 0; i < N; ++i) {
            const Strip s = plane_strip(PLANE, i, N);
            const float want_y = i == 0
                ? PLANE.horizon_y
                : plane_strip(PLANE, i - 1, N).dst_y + plane_strip(PLANE, i - 1, N).dst_h;
            if (std::fabs(s.dst_y - want_y) > 0.01f) {
                contiguous = false;
                if (first_bad.empty())
                    first_bad = "strip " + std::to_string(i) + " starts at " +
                                std::to_string(s.dst_y) + ", previous ends at " +
                                std::to_string(want_y);
            }
        }
        const Strip last = plane_strip(PLANE, N - 1, N);
        check("the strips tile the band with no gap and no overlap", contiguous, first_bad);
        check("the last strip reaches the near edge",
              std::fabs(last.dst_y + last.dst_h - PLANE.bottom_y) < 0.01f,
              "ends at " + std::to_string(last.dst_y + last.dst_h));
    }

    // Property 4: the source rows meet, cover the whole tile, and stay inside it.
    // The tile is laid out in world distance from far edge to near, so the plane
    // uses it exactly once from top to bottom -- a strip sampling outside it is
    // reading a neighbouring row of the same tile and repeating a mark where the
    // geometry says there is none.
    {
        bool ok = true;
        std::string first_bad;
        float cursor = 0.0f;
        for (int i = 0; i < N; ++i) {
            const Strip s = plane_strip(PLANE, i, N);
            if (std::fabs(s.src_y - cursor) > 0.01f || s.src_y < -0.01f ||
                s.src_y + s.src_h > static_cast<float>(PLANE.tile_h) + 0.01f) {
                ok = false;
                if (first_bad.empty())
                    first_bad = "strip " + std::to_string(i) + " samples " +
                                std::to_string(s.src_y) + "+" + std::to_string(s.src_h) +
                                ", expected to start at " + std::to_string(cursor);
            }
            cursor = s.src_y + s.src_h;
        }
        check("the source rows meet and stay inside the tile", ok, first_bad);
        check("the plane uses the whole tile from far edge to near",
              std::fabs(cursor - static_cast<float>(PLANE.tile_h)) < 0.01f,
              "the last strip ends at source row " + std::to_string(cursor));
    }

    // Property 5: the factors run from far to near and never backwards. This is what
    // makes it a plane rather than a wall -- a strip that scrolls slower than the one
    // above it is nearer and further at once, and the recession inverts locally.
    {
        bool monotonic = true;
        std::string first_bad;
        for (int i = 1; i < N; ++i) {
            const float a = plane_strip(PLANE, i - 1, N).factor;
            const float b = plane_strip(PLANE, i, N).factor;
            if (!(b > a)) {
                monotonic = false;
                if (first_bad.empty())
                    first_bad = "strip " + std::to_string(i) + " scrolls at " +
                                std::to_string(b) + ", the one above it at " +
                                std::to_string(a);
            }
        }
        check("each strip scrolls faster than the one further away", monotonic, first_bad);

        const Strip far_s = plane_strip(PLANE, 0, N);
        const Strip near_s = plane_strip(PLANE, N - 1, N);
        check("every strip's factor is inside the declared range",
              far_s.factor > PLANE.far_factor && near_s.factor < PLANE.near_factor,
              "far " + std::to_string(far_s.factor) + ", near " +
                  std::to_string(near_s.factor));
    }

    // Property 6: the texture gradient, and the reason the source mapping is a curve
    // rather than a straight line.
    //
    // A linear source mapping passes every property above and fails this one, which
    // is why it is here: strips of equal screen height sampling equal source height
    // is a self-consistent, contiguous, in-bounds mapping of a plane that is not
    // receding at all. The near strip must be magnified relative to the far one, and
    // by a lot.
    {
        const Strip far_s = plane_strip(PLANE, 0, N);
        const Strip near_s = plane_strip(PLANE, N - 1, N);
        const double far_zoom = far_s.dst_h / far_s.src_h;
        const double near_zoom = near_s.dst_h / near_s.src_h;
        const std::string detail =
            "near strip is magnified " + std::to_string(near_zoom) +
            "x, far strip " + std::to_string(far_zoom) + "x";
        check("marks widen toward the viewer", near_zoom > far_zoom * 1.3, detail);
        check("no strip samples backwards or nothing",
              far_s.src_h > 0.0f && near_s.src_h > 0.0f, detail);
    }

    // Property 7: the integer source rows meet, which property 4 says nothing about
    // and which is where the shipped defect actually was.
    //
    // SDL_Rect is integer, so the float rows property 4 checks have to be rounded
    // before they reach a draw call, and rounding each strip's start and its height
    // independently does not preserve the meeting. Property 4 passes the whole time,
    // because the floats do meet; nothing asks whether their roundings do.
    //
    // The general shape worth recognising: when a continuous quantity is checked for
    // a property and then quantised before use, the property has to be re-checked on
    // the quantised value -- the test that passes is measuring something the
    // renderer never sees.
    //
    // The assertion is conservation: the integer heights the draw call builds must
    // sum to the tile, exactly. That is what a gap and an overlap both break, in
    // opposite directions.
    {
        int total = 0;
        bool bounded = true;
        std::string first_bad;
        for (int i = 0; i < N; ++i) {
            const int a = backdrop_wrap::plane_src_row(PLANE, i, N);
            const int b = backdrop_wrap::plane_src_row(PLANE, i + 1, N);
            total += b - a;
            if (a < 0 || b > PLANE.tile_h || b < a) {
                bounded = false;
                if (first_bad.empty())
                    first_bad = "strip " + std::to_string(i) + " samples rows " +
                                std::to_string(a) + ".." + std::to_string(b) +
                                " of a " + std::to_string(PLANE.tile_h) + "-row tile";
            }
        }
        check("the integer source rows partition the tile exactly",
              total == PLANE.tile_h,
              "the strips' integer heights sum to " + std::to_string(total) +
                  ", the tile is " + std::to_string(PLANE.tile_h));
        check("no integer source rect leaves the tile", bounded, first_bad);
        check("the first strip starts at row 0 and the last ends at the tile's end",
              backdrop_wrap::plane_src_row(PLANE, 0, N) == 0 &&
                  backdrop_wrap::plane_src_row(PLANE, N, N) == PLANE.tile_h,
              "row(0)=" + std::to_string(backdrop_wrap::plane_src_row(PLANE, 0, N)) +
                  " row(N)=" + std::to_string(backdrop_wrap::plane_src_row(PLANE, N, N)));
    }

    // Property 8: the same, swept over every plane geometry the running game
    // produces. The horizon moves with the camera, so the band is not one number --
    // the strips compress as the plane thins, and the near-horizon strips are the
    // first to collapse below a texel. A rounding that meets at the shipped geometry
    // and not at a thinner one is a defect that appears at one camera height.
    {
        bool all = true;
        std::string first_bad;
        for (int horizon = -400; horizon <= 1000; horizon += 7) {
            const Plane p{static_cast<float>(horizon), 1080.0f, 0.28f, 0.52f, 256};
            for (int i = 0; i < N; ++i) {
                const int a = backdrop_wrap::plane_src_row(p, i, N);
                const int b = backdrop_wrap::plane_src_row(p, i + 1, N);
                if (b < a || a < 0 || b > p.tile_h) {
                    all = false;
                    if (first_bad.empty())
                        first_bad = "horizon " + std::to_string(horizon) + " strip " +
                                    std::to_string(i) + " rows " + std::to_string(a) +
                                    ".." + std::to_string(b);
                }
            }
        }
        check("the integer rows stay ordered and in bounds at every horizon",
              all, first_bad);
    }

    // Property 9: the plane translates, it does not rescale.
    //
    // The plane's far edge is parallaxed; a near edge nailed to the window's bottom
    // is a constant. That gives the near end an effective vertical parallax of zero
    // while the far end has the mountains', which is the nearer end of one layer
    // less parallaxed than its further end -- the one thing a parallax stack may
    // never do. What it looks like is the whole tile squeezed into a shrinking band
    // and never leaving the frame.
    //
    // Stated as three things that must not change with the horizon rather than as a
    // formula, because the formula is the fix and a test that restates its
    // implementation checks nothing. A plane that translates has a constant band
    // height, a constant scale, and a near edge that moves exactly as far as its far
    // edge. All three are independent of what the constant happens to be.
    //
    // The sweep is the camera's real vertical range, and wider, so a taller window
    // mode cannot find an untested horizon.
    {
        const Plane base = backdrop_wrap::plane_geometry(300.0f, 256, 0.28f, 0.52f);
        const float base_band = base.bottom_y - base.horizon_y;

        bool constant_band = true, constant_scale = true, translates = true;
        std::string first_bad;
        for (int horizon = 200; horizon <= 800; horizon += 3) {
            const float h = static_cast<float>(horizon);
            const Plane q = backdrop_wrap::plane_geometry(h, 256, 0.28f, 0.52f);
            const float band = q.bottom_y - q.horizon_y;

            if (!near_eq(band, base_band)) {
                constant_band = false;
                if (first_bad.empty())
                    first_bad = "horizon " + std::to_string(horizon) + " band " +
                                std::to_string(band) + " vs " + std::to_string(base_band);
            }
            if (!near_eq(band / static_cast<float>(q.tile_h),
                         base_band / static_cast<float>(base.tile_h)))
                constant_scale = false;
            // The near edge must move with the far edge, not against it and not less
            // than it. Equality is what "translates" means.
            if (!near_eq(q.bottom_y - base.bottom_y, q.horizon_y - base.horizon_y))
                translates = false;
        }

        check("the plane's band height does not change with the horizon",
              constant_band, first_bad);
        check("the plane's scale does not change with the horizon",
              constant_scale, "pixels per texel moved as the camera climbed");
        check("the plane's near edge moves exactly as far as its far edge",
              translates, "the near end is less parallaxed than the far end");
    }

    // Degenerate input, the same contract wrap_axis has: a failed BMP load is a
    // zero-height tile, and it must produce a strip that draws nothing rather than a
    // division by zero.
    {
        const Plane broken{594.0f, 1080.0f, 0.28f, 0.52f, 0};
        const Strip s = plane_strip(broken, 3, N);
        check("a zero-height tile draws nothing",
              s.dst_h == 0.0f && s.src_h == 0.0f, "got dst_h " + std::to_string(s.dst_h));
        const Strip none = plane_strip(PLANE, 0, 0);
        check("zero strips draw nothing", none.dst_h == 0.0f,
              "got dst_h " + std::to_string(none.dst_h));
    }

    return report();
}
