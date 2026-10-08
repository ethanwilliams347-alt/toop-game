// Backdrop sets: the backdrop.txt parser, and every set a shipped scene names
// held to the BMPs it describes.
//
// The second half is what boot_test (for the bg1 family) and rig_test (for
// bg_tarn) used to do against C++ tables. The numbers moved into text files
// next to the art; the checks moved here with them, and now run on every set
// whatever its model, so a property one set has is checked on every set that
// claims it.
//
// Runs from the source tree (WORKING_DIRECTORY), because it reads
// assets/scenes.txt and the art. Links no SDL: what a person has to judge --
// whether the lake reads as wide -- is preview_backdrop's job.
#include "game/display.h"
#include "render/backdrop_set.h"
#include "scene/bmp.h"
#include "scene/scene_list.h"
#include "test_util.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

const char* TMP = "test_backdrop_set_tmp.txt";

backdrop_set::Set parse(const std::string& text, std::string* error) {
    {
        std::ofstream out(TMP);
        out << text;
    }
    if (error) error->clear();
    backdrop_set::Set s = backdrop_set::load(TMP, "x/", error);
    std::remove(TMP);
    return s;
}

bool is_key(uint32_t p) { return (p & 0xFFFFFFu) == 0xFF00FFu; }

void test_parser() {
    std::string err;
    const backdrop_set::Set ok = parse("# comment\n"
                                       "size 100 50\n"
                                       "anchor standing\n"
                                       "rig 20 40 0.5   # horizon, contact, k\n"
                                       "ripple 0.6\n"
                                       "layer sky.bmp factor=0 opaque\n"
                                       "layer plane.bmp plane ripple=25:30\n"
                                       "layer glint.bmp factor=0 on_plane ripple=25:30\n"
                                       "layer hill.bmp foot=30\n"
                                       "layer cloud.bmp factor=0.01 drift=-0.6\n"
                                       "layer reeds.bmp factor=1.3 foreground\n",
                                       &err);
    check("parse: a well-formed set parses", err.empty() && ok.layers.size() == 6, err);
    check("parse: ...with its size, anchor, rig and ripple",
          ok.native_w == 100 && ok.native_h == 50 && ok.anchor == backdrop_set::Anchor::Standing &&
              ok.has_rig && ok.rig.horizon_row == 20 && ok.rig.contact_row == 40 &&
              ok.rig.vertical_strength == 0.5f && ok.ripple_amplitude == 0.6f);
    if (ok.layers.size() == 6) {
        check("parse: a foot row derives its factor from the rig",
              backdrop_set::factor_of(ok, ok.layers[3]) == 0.5f);
        check("parse: ...and its vertical factor too",
              backdrop_set::vertical_factor_of(ok, ok.layers[3]) == 0.75f);
        check("parse: flags and ranges land on their layer",
              ok.layers[0].opaque && ok.layers[1].plane && ok.layers[1].ripple_row0 == 25 &&
                  ok.layers[2].on_plane && ok.layers[4].drift == -0.6f && ok.layers[5].foreground &&
                  ok.layers[5].factor == 1.3f);
        check("parse: layers keep their line", ok.layers[0].line == 6 && ok.layers[5].line == 11);
    }

    const backdrop_set::Set banded =
        parse("size 10 20\nanchor corner\nlayer g.bmp bands=0:8:0.3,8:12:0.7,12:20:1\n", &err);
    check("parse: a banded corner set with no rig parses",
          err.empty() && banded.layers.size() == 1 && banded.layers[0].bands.size() == 3 &&
              banded.anchor == backdrop_set::Anchor::Corner && !banded.has_rig,
          err);
    if (banded.layers.size() == 1)
        check("parse: ...and with no rig the vertical is locked",
              backdrop_set::vertical_factor_of(banded, banded.layers[0]) == 1.0f);

    // Every malformed record rejects the whole set and names its line: a stack
    // missing the layer it could not read draws hills with no ground under them.
    struct Bad {
        const char* what;
        const char* text;
        int line;
    };
    const Bad bad[] = {
        {"a record before size", "layer a.bmp factor=1\nsize 10 10\n", 1},
        {"a second size", "size 10 10\nsize 10 10\n", 2},
        {"an unknown record", "size 10 10\nhorizon 4\n", 2},
        {"an unknown anchor", "size 10 10\nanchor middle\n", 2},
        {"a rig with horizon below contact", "size 10 10\nrig 8 4 0\n", 2},
        {"a rig strength above 1", "size 10 10\nrig 2 8 1.5\n", 2},
        {"a layer that moves two ways", "size 10 10\nlayer a.bmp factor=1 plane\n", 2},
        {"a layer that does not say how it moves", "size 10 10\nlayer a.bmp opaque\n", 2},
        {"a foot row with no rig", "size 10 10\nlayer a.bmp foot=4\n", 2},
        {"a plane with no rig", "size 10 10\nlayer a.bmp plane\n", 2},
        {"a foot row outside the art", "size 10 10\nrig 2 8 0\nlayer a.bmp foot=12\n", 3},
        {"bands with a gap", "size 10 10\nlayer a.bmp bands=0:4:0.3,5:10:1\n", 2},
        {"bands that stop short", "size 10 10\nlayer a.bmp bands=0:4:0.3,4:8:1\n", 2},
        {"bands under vertical parallax", "size 10 10\nrig 2 8 0.5\nlayer a.bmp bands=0:10:1\n", 3},
        {"on_plane with no ripple rows", "size 10 10\nrig 2 8 0\nlayer a.bmp factor=0 on_plane\n",
         3},
        {"a file outside the set's directory", "size 10 10\nlayer ../a.bmp factor=1\n", 2},
        {"a file that is not a bmp", "size 10 10\nlayer a.png factor=1\n", 2},
        {"a factor with a unit on it", "size 10 10\nlayer a.bmp factor=0.3x\n", 2},
        {"an unknown flag", "size 10 10\nlayer a.bmp factor=1 shiny\n", 2},
        {"a stray field after a record", "size 10 10 4\n", 1},
        {"a width of no cells", "size 10 10\nlayer a.bmp factor=0 width=0\n", 2},
        {"a width with a unit on it", "size 10 10\nlayer a.bmp factor=0 width=5px\n", 2},
    };
    for (const Bad& b : bad) {
        const backdrop_set::Set s = parse(b.text, &err);
        const std::string where = ":" + std::to_string(b.line) + ":";
        check((std::string("parse: ") + b.what + " rejects the set at its line").c_str(),
              s.layers.empty() && err.find(where) != std::string::npos, err);
    }

    const backdrop_set::Set tiled =
        parse("size 40 10\nlayer sky.bmp factor=0 width=8 opaque\nlayer hill.bmp factor=1\n", &err);
    check("parse: width= makes a layer a narrower tile, and the rest keep the set's width",
          err.empty() && tiled.layers.size() == 2 &&
              backdrop_set::width_of(tiled, tiled.layers[0]) == 8 &&
              backdrop_set::width_of(tiled, tiled.layers[1]) == 40,
          err);

    parse("size 10 10\n", &err);
    check("parse: a set with no layers is refused", !err.empty(), err);
    err.clear();
    backdrop_set::load("no_such_backdrop.txt", "x/", &err);
    check("parse: a missing file is an error, not an empty backdrop", !err.empty());
}

struct Shipped {
    std::string name;  // the backdrop directory
    backdrop_set::Set set;
};

std::vector<Shipped> shipped_sets() {
    std::vector<Shipped> out;
    std::string err;
    const std::vector<scene_list::SceneDef> scenes =
        scene_list::load_scene_list("assets/scenes.txt", &err);
    check("shipped: the scene list loads", !scenes.empty(), err);
    for (const scene_list::SceneDef& def : scenes) {
        if (def.backdrop.empty()) continue;
        bool seen = false;
        for (const Shipped& s : out) seen |= s.name == def.backdrop;
        if (seen) continue;
        const std::string dir = "assets/" + def.backdrop + "/";
        err.clear();
        Shipped s{def.backdrop, backdrop_set::load(dir + "backdrop.txt", dir, &err)};
        check(("shipped: " + def.backdrop + "'s backdrop.txt loads").c_str(), err.empty(), err);
        if (!err.empty()) continue;
        // One art pixel is one world cell: a scene that states a size states its
        // backdrop's, or the painting and the terrain stop lining up.
        if (def.custom_width > 0)
            check(("shipped: scene '" + def.name + "' is its backdrop's native size").c_str(),
                  def.custom_width == s.set.native_w && def.custom_height == s.set.native_h);
        // A layer narrower than its set must still never show itself twice. Over a
        // walk from one end of the world to the other, a layer at factor f shows
        // V + f * (W - V) of its columns through a window V cells wide, and that
        // grows with V, so the widest window the game offers is the one to hold it
        // to. Drifting layers are exempt: drift laps any tile eventually, and
        // clouds are made to be seen lapping. So is a layer faster than the world
        // (a foreground above 1.00), which shows more columns than the world has
        // and so repeats at any width; its art has to be sparse enough not to
        // be caught doing it.
        if (s.set.anchor == backdrop_set::Anchor::Standing && def.scale > 0) {
            int widest = 0;
            for (const DisplayMode& m : DISPLAY_MODES)
                widest = std::max(widest, m.padded_w(def.scale));
            const int world_w = def.custom_width > 0 ? def.custom_width : s.set.native_w;
            for (const backdrop_set::Layer& l : s.set.layers) {
                if (l.width <= 0 || l.plane || l.drift != 0.0f || !l.bands.empty()) continue;
                const float f = backdrop_set::factor_of(s.set, l);
                if (f > 1.0f) continue;
                const float shown = static_cast<float>(widest) +
                                    f * static_cast<float>(std::max(0, world_w - widest));
                check(("shipped: " + def.backdrop + ": " + l.file +
                       " is wide enough never to repeat on screen")
                          .c_str(),
                      static_cast<float>(l.width) >= shown,
                      "width " + std::to_string(l.width) + ", shows " +
                          std::to_string(static_cast<int>(shown)));
            }
        }
        out.push_back(s);
    }
    check("shipped: at least one scene names a backdrop", !out.empty());
    return out;
}

// Properties every set has, whatever its model.
void test_every_set(const Shipped& sh) {
    const backdrop_set::Set& set = sh.set;
    const std::string tag = sh.name + ": ";
    const int n = static_cast<int>(set.layers.size());

    // Back to front, in strict numeric-descending filename order, each named for
    // its set. A row copied between two files and left pointing at the other set's
    // image fails here rather than at a launch nobody is watching.
    const std::string prefix = sh.name + "_";
    bool descending = true;
    int previous = 1 << 30;
    for (const backdrop_set::Layer& l : set.layers) {
        if (l.file.compare(0, prefix.size(), prefix) != 0 || l.file.size() < prefix.size() + 2) {
            descending = false;
            break;
        }
        const int index = std::atoi(l.file.substr(prefix.size(), 2).c_str());
        descending = descending && index > 0 && index < previous;
        previous = index;
    }
    check((tag + "the layers are named for the set and listed back to front").c_str(), descending);

    // The sky is the only layer painted edge to edge, and it is the backmost.
    // Anything opaque in front of it hides everything behind it.
    int opaque = 0, foreground = 0;
    for (const backdrop_set::Layer& l : set.layers) {
        opaque += l.opaque;
        foreground += l.foreground;
    }
    check((tag + "exactly one opaque layer, and it is the backmost").c_str(),
          opaque == 1 && set.layers[0].opaque);
    // A foreground layer with anything listed after it would be drawn over by a
    // layer that is meant to be behind the player.
    check((tag + "exactly one foreground layer, and it is the frontmost").c_str(),
          foreground == 1 && set.layers[static_cast<size_t>(n - 1)].foreground);

    for (const backdrop_set::Layer& l : set.layers) {
        const std::string ltag = tag + l.file + ": ";
        bmp::Image img;
        std::string err;
        const bool ok = bmp::read((set.dir + l.file).c_str(), img, &err);
        check((ltag + "reads").c_str(), ok, err);
        if (!ok) continue;
        // One art pixel is one world cell, so a layer of a different size is not a
        // scaling question -- it no longer lines up with the others.
        // A narrower tile is stated (width=), never discovered from the BMP.
        const int want_w = backdrop_set::width_of(set, l);
        check((ltag + "is the set's height and its stated width").c_str(),
              img.width == want_w && img.height == set.native_h,
              std::to_string(img.width) + "x" + std::to_string(img.height));
        if (img.width != want_w || img.height != set.native_h) continue;

        const auto px = [&](int x, int y) {
            return img.pixels[static_cast<size_t>(y) * static_cast<size_t>(img.width) +
                              static_cast<size_t>(x)];
        };
        int lowest = -1, highest = img.height;
        for (int y = 0; y < img.height; ++y)
            for (int x = 0; x < img.width; ++x)
                if (!is_key(px(x, y))) {
                    lowest = y;
                    if (highest == img.height) highest = y;
                }

        if (l.opaque) {
            bool full = true;
            for (int y = 0; y < img.height && full; ++y)
                for (int x = 0; x < img.width && full; ++x) full = !is_key(px(x, y));
            check((ltag + "the opaque layer has no transparent pixel").c_str(), full);
        }

        // The foot IS the factor. If the art's lowest painted row and the file
        // disagree, the layer scrolls at the speed of some other depth and its
        // feet slide on the plane.
        if (l.foot_row >= 0)
            check((ltag + "the lowest painted row is the file's foot row").c_str(),
                  lowest == l.foot_row,
                  "art says " + std::to_string(lowest) + ", file says " +
                      std::to_string(l.foot_row));

        // The plane is transparent above the horizon and solid from it down: the
        // renderer starts its row loop at the horizon, so paint above it would
        // never be drawn and a hole below it would show the sky through the ground.
        if (l.plane) {
            bool shaped = true;
            for (int y = 0; y < img.height && shaped; ++y)
                for (int x = 0; x < img.width && shaped; ++x)
                    shaped = is_key(px(x, y)) == (y < set.rig.horizon_row);
            check((ltag + "the plane starts exactly at the horizon and has no holes").c_str(),
                  shaped);
        }

        // Paint on the plane needs the plane under it.
        if (l.on_plane)
            check((ltag + "paint on the plane lies below the horizon").c_str(),
                  l.ripple_row0 >= set.rig.horizon_row);

        // A rippled object layer is only drawn over its ripple rows.
        if (!l.plane && l.ripple_row1 > l.ripple_row0)
            check((ltag + "all of a rippled layer's paint is inside its ripple rows").c_str(),
                  highest >= l.ripple_row0 && lowest < l.ripple_row1,
                  "painted " + std::to_string(highest) + ".." + std::to_string(lowest));

        // Bands are cut on flat paint. Each band scrolls at its own rate, so a
        // boundary is a horizontal step in scroll offset, invisible only where the
        // rows either side of it are one colour across every column and the same
        // colour as each other. Moving a boundary two rows costs nothing and shows
        // nothing until somebody walks, which is why this is checked.
        if (!l.bands.empty()) {
            bool ordered = true;
            for (size_t i = 0; i < l.bands.size(); ++i) {
                const float f = l.bands[i].parallax_x;
                ordered =
                    ordered && f > 0.0f && f <= 1.0f && (i == 0 || f > l.bands[i - 1].parallax_x);
            }
            check((ltag + "the bands' factors increase toward the viewer and cap at 1.0").c_str(),
                  ordered);

            auto uniform_colour = [&](int row, uint32_t& out) {
                out = px(0, row);
                for (int x = 1; x < img.width; ++x)
                    if (px(x, row) != out) return false;
                return true;
            };
            for (size_t i = 1; i < l.bands.size(); ++i) {
                const int cut = l.bands[i].row0;
                uint32_t above = 0, below = 0;
                const bool a = uniform_colour(cut - 1, above);
                const bool b = uniform_colour(cut, below);
                const bool flat = a && b && above == below;
                check((ltag + "every band boundary falls on flat paint").c_str(), flat,
                      flat ? std::string()
                           : "art row " + std::to_string(cut) + ": " +
                                 (!a ? "the row above the cut is not uniform"
                                     : (!b ? "the row below the cut is not uniform"
                                           : "the two rows differ in colour")));
            }
        }

        // A standing-anchored set wraps every layer, so its last column runs into
        // its first. Not provable pixel for pixel; what is checkable is that the
        // seam is no harsher than the harshest column boundary inside the image.
        if (set.anchor == backdrop_set::Anchor::Standing) {
            const auto differ = [&](int a, int b) {
                int d = 0;
                for (int y = 0; y < img.height; ++y) d += px(a, y) != px(b, y);
                return d;
            };
            int worst = 0;
            for (int x = 0; x + 1 < img.width; ++x) worst = std::max(worst, differ(x, x + 1));
            const int seam = differ(img.width - 1, 0);
            check((ltag + "the wrap seam is no harsher than any column boundary inside it").c_str(),
                  seam <= worst,
                  "seam " + std::to_string(seam) + " vs worst " + std::to_string(worst));
        }
    }
}

// The corner model: art painted as one world-sized picture, which does not tile.
void test_corner_set(const Shipped& sh) {
    const backdrop_set::Set& set = sh.set;
    const std::string tag = sh.name + ": ";

    // Nearer is faster and nothing exceeds 1.0. At the corner anchor a
    // world-sized layer covers the window from its first copy exactly when
    // f <= 1; above it the second copy shows, and this art does not tile.
    bool ladder = true;
    float last = -1.0f;
    for (const backdrop_set::Layer& l : set.layers) {
        if (!l.bands.empty()) continue;  // checked per band above
        const float f = backdrop_set::factor_of(set, l);
        ladder = ladder && l.has_factor && f >= 0.0f && f <= 1.0f && f > last;
        last = f;
    }
    check((tag + "the factors increase toward the viewer and cap at 1.0").c_str(), ladder);
    check((tag + "a corner set is locked vertically").c_str(),
          !set.has_rig || set.rig.vertical_strength == 0.0f);
}

// The rig model.
void test_rig_set(const Shipped& sh) {
    const backdrop_set::Set& set = sh.set;
    const std::string tag = sh.name + ": ";
    int planes = 0;
    float last_foot_factor = -1.0f;
    bool ladder = true;
    for (const backdrop_set::Layer& l : set.layers) {
        planes += l.plane;
        if (l.foot_row >= 0) {
            const float f = backdrop_set::factor_of(set, l);
            ladder = ladder && f > last_foot_factor;
            last_foot_factor = f;
        }
    }
    check((tag + "exactly one plane").c_str(), planes == 1);
    check((tag + "standing layers are listed back to front: feet descend the plane").c_str(),
          ladder);
}

// An extended set is the same nine depths of the same place: the one claim its
// file makes that is not about its own art. A factor that drifts makes it a
// different landscape in the same palette, and nothing about a wrong ladder
// fails to load.
void test_bg1_ext_is_bg1(const std::vector<Shipped>& sets) {
    const Shipped* base = nullptr;
    const Shipped* ext = nullptr;
    for (const Shipped& s : sets) {
        if (s.name == "bg1") base = &s;
        if (s.name == "bg1_ext") ext = &s;
    }
    check("bg1_ext: both sets are shipped", base && ext);
    if (!base || !ext) return;
    const backdrop_set::Set& a = base->set;
    const backdrop_set::Set& b = ext->set;

    check("bg1_ext: the world is exactly twice bg1's on both axes",
          b.native_w == a.native_w * 2 && b.native_h == a.native_h * 2);
    bool same = a.layers.size() == b.layers.size();
    for (size_t i = 0; same && i < a.layers.size(); ++i) {
        const backdrop_set::Layer& la = a.layers[i];
        const backdrop_set::Layer& lb = b.layers[i];
        same = la.factor == lb.factor && la.opaque == lb.opaque && la.foreground == lb.foreground &&
               la.bands.empty() == lb.bands.empty();
    }
    check("bg1_ext: every layer carries bg1's factor and bg1's flags", same);

    // The bands moved down by the frame's growth and did not change shape: the
    // vertical rule the art was generated under, which makes the extension's
    // standing view the same rows as the base set's.
    const int shift = b.native_h - a.native_h;
    bool shifted = same;
    for (size_t i = 0; shifted && i < a.layers.size(); ++i) {
        const std::vector<backdrop_set::Band>& ba = a.layers[i].bands;
        const std::vector<backdrop_set::Band>& bb = b.layers[i].bands;
        shifted = ba.size() == bb.size();
        for (size_t k = 0; shifted && k < ba.size(); ++k)
            shifted = bb[k].parallax_x == ba[k].parallax_x && bb[k].row1 == ba[k].row1 + shift &&
                      (k == 0 ? bb[k].row0 == 0 : bb[k].row0 == ba[k].row0 + shift);
    }
    check("bg1_ext: the ground bands are bg1's, at the same factors, moved down by the "
          "frame's growth",
          shifted);

    // Same colours, layer by layer. The generator paints only with what it found
    // in each source layer, so this holds by construction; the check is for the
    // day somebody edits the generator. The colour key is "no pixel", not paint.
    auto palette_of = [](const bmp::Image& img) {
        std::set<uint32_t> out;
        for (uint32_t p : img.pixels)
            if (!is_key(p)) out.insert(p & 0xFFFFFFu);
        return out;
    };
    for (size_t i = 0; i < a.layers.size() && i < b.layers.size(); ++i) {
        bmp::Image ia, ib;
        if (!bmp::read((a.dir + a.layers[i].file).c_str(), ia, nullptr) ||
            !bmp::read((b.dir + b.layers[i].file).c_str(), ib, nullptr))
            continue;  // the read itself is checked per set
        const std::set<uint32_t> want = palette_of(ia), got = palette_of(ib);
        check(("bg1_ext: " + b.layers[i].file + " is painted in exactly the colours of " +
               a.layers[i].file)
                  .c_str(),
              want == got,
              std::to_string(got.size()) + " colours against " + std::to_string(want.size()));
    }
}

} // namespace

int main() {
    test_parser();
    const std::vector<Shipped> sets = shipped_sets();
    for (const Shipped& s : sets) {
        test_every_set(s);
        if (s.set.anchor == backdrop_set::Anchor::Corner) test_corner_set(s);
        if (s.set.has_rig) test_rig_set(s);
    }
    test_bg1_ext_is_bg1(sets);
    return report();
}
