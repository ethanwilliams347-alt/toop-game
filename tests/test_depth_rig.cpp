// The perspective rig: render/depth_rig.h's arithmetic, and every set in
// render/rig_backdrop.h held to the BMPs it describes.
//
// Links no SDL. The draw path in render/frame.cpp is a loop over the functions
// tested here, which is the same split backdrop_test has with backdrop_wrap.h.
// What a person has to judge -- whether the lake reads as wide -- is
// preview_backdrop's job, not this file's.

#include "render/depth_rig.h"
#include "render/rig_backdrop.h"
#include "scene/bmp.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

bool near_eq(float a, float b, float tol = 0.01f) { return std::fabs(a - b) < tol; }

bool is_key(uint32_t p) { return (p & 0xFFFFFFu) == 0xFF00FFu; }

// The three display modes, as padded viewports at scale 10 (game/display.h).
// Repeated rather than included: display.h pulls in camera.h and settings I/O,
// and this suite is about the rig's numbers, not the shell's.
struct View { int w, h; };
constexpr View VIEWS[] = {{193, 109}, {257, 145}, {345, 145}};
constexpr int SCALE = 10;
constexpr int BODY_H = 26;            // Player::HEIGHT
constexpr float VERTICAL_ANCHOR = 0.80f;  // Camera::VERTICAL_ANCHOR

void test_pinhole() {
    const depth_rig::Rig rig{200, 264, 0.5f};
    check("the horizon row scrolls at 0", depth_rig::factor_at(rig, 200.0f) == 0.0f);
    check("the contact row scrolls with the world, at 1",
          depth_rig::factor_at(rig, 264.0f) == 1.0f);
    check("halfway down the plane is halfway in factor - linear in the row",
          near_eq(depth_rig::factor_at(rig, 232.0f), 0.5f));
    check("the plane clamps to [0, 1]",
          depth_rig::plane_factor(rig, 150.0f) == 0.0f &&
              depth_rig::plane_factor(rig, 280.0f) == 1.0f);
    check("a foreground below the contact row comes out nearer than the world",
          depth_rig::factor_at(rig, 280.0f) > 1.0f);

    // The claim depth_rig.h makes about Ethan's bg1: its two hand-chosen contacts,
    // 0.30 at row 73 and 0.70 at row 89, are one pinhole camera with its horizon at
    // row 61 -- two rows above where bg1_08_ground starts being painted.
    const depth_rig::Rig bg1{61, 101, 1.0f};
    check("bg1's hand-tuned contacts are a pinhole camera (horizon 61, contact 101)",
          near_eq(depth_rig::factor_at(bg1, 73.0f), 0.30f) &&
              near_eq(depth_rig::factor_at(bg1, 89.0f), 0.70f));

    check("vertical strength 1 is the honest camera, 0 is bg1's locked vertical",
          depth_rig::vertical_factor(depth_rig::Rig{0, 1, 1.0f}, 0.3f) == 0.3f &&
              depth_rig::vertical_factor(depth_rig::Rig{0, 1, 0.0f}, 0.3f) == 1.0f);
}

void test_placement() {
    const depth_rig::Rig rig{200, 264, 0.5f};
    const int world_w = 688, world_h = 288;

    for (const View& v : VIEWS) {
        const std::string tag = std::to_string(v.w) + "x" + std::to_string(v.h) + ": ";
        const float ay = depth_rig::standing_anchor_y(rig, BODY_H, v.h, VERTICAL_ANCHOR, world_h);
        const float ax = 0.5f * static_cast<float>(world_w - v.w);
        const float max_y = static_cast<float>(world_h - v.h);
        const float max_x = static_cast<float>(world_w - v.w);
        check((tag + "the standing anchor is inside the camera's range").c_str(),
              ay >= 0.0f && ay <= max_y, std::to_string(ay));

        // At the anchor, every factor puts the art where the world is: the stack is
        // the painting there, whatever its depths.
        bool exact = true;
        for (float f = 0.0f; f <= 1.3f; f += 0.05f)
            exact = exact && near_eq(depth_rig::origin(ay, ay, f, SCALE), -ay * SCALE) &&
                    near_eq(depth_rig::origin(ax, ax, f, SCALE), -ax * SCALE);
        check((tag + "at the anchor every layer is placed where the world is").c_str(), exact);

        // The glue, swept over every camera position the game can reach: a layer
        // standing on plane row r lands its feet on the plane's own row r, on both
        // axes. This is the property the bg1 model had to give up the vertical
        // axis to keep.
        bool glued = true;
        std::string where;
        for (float cy = 0.0f; cy <= max_y; cy += 3.7f) {
            for (float cx = 0.0f; cx <= max_x; cx += 41.3f) {
                for (int r = rig.horizon_row; r <= rig.contact_row; ++r) {
                    const float f = depth_rig::factor_at(rig, static_cast<float>(r));
                    const float obj_y = static_cast<float>(r * SCALE) +
                                        depth_rig::origin(cy, ay, depth_rig::vertical_factor(rig, f), SCALE);
                    const float plane_y = depth_rig::plane_edge_y(rig, r, cy, ay, SCALE);
                    const float obj_x = depth_rig::origin(cx, ax, f, SCALE);
                    const float plane_x = depth_rig::origin(
                        cx, ax, depth_rig::plane_factor(rig, static_cast<float>(r)), SCALE);
                    if (!near_eq(obj_y, plane_y, 0.05f) || !near_eq(obj_x, plane_x, 0.05f)) {
                        glued = false;
                        where = "row " + std::to_string(r) + " at camera (" +
                                std::to_string(cx) + ", " + std::to_string(cy) + ")";
                    }
                }
            }
        }
        check((tag + "feet stay on the plane row they stand on, at every camera").c_str(),
              glued, where);

        // No row of the plane ever has zero or negative height. A camera far enough
        // below the anchor would fold the plane over its own horizon; this is the
        // check that the world's floor stops it first.
        bool forward = true;
        for (float cy = 0.0f; cy <= max_y; cy += 1.3f)
            for (int r = rig.horizon_row; r < world_h; ++r)
                forward = forward && depth_rig::plane_edge_y(rig, r + 1, cy, ay, SCALE) >
                                         depth_rig::plane_edge_y(rig, r, cy, ay, SCALE);
        check((tag + "every plane row keeps a positive height at every camera height").c_str(),
              forward);

        // Coverage: a world-tall opaque layer at any factor in [0, 1] covers the
        // window at every reachable camera -- the convex-combination argument.
        bool covered = true;
        for (float f = 0.0f; f <= 1.0f; f += 0.125f) {
            const float g = depth_rig::vertical_factor(rig, f);
            for (float cy = 0.0f; cy <= max_y; cy += 1.0f) {
                const float top = depth_rig::origin(cy, ay, g, SCALE);
                covered = covered && top <= 0.0f &&
                          top + static_cast<float>(world_h * SCALE) >=
                              static_cast<float>(v.h * SCALE);
            }
        }
        check((tag + "a world-tall opaque layer always covers the window").c_str(), covered);
    }
}

void test_sets() {
    check("there is at least one rig set", rig_backdrop::SET_COUNT > 0);
    check("a scene with no rig set finds none",
          rig_backdrop::find("bg1") == nullptr && rig_backdrop::find(nullptr) == nullptr);

    for (int s = 0; s < rig_backdrop::SET_COUNT; ++s) {
        const rig_backdrop::Set& set = rig_backdrop::SETS[s];
        const std::string tag = std::string(set.scene) + ": ";
        const int n = set.layer_count;
        check((tag + "find returns the set for its own name").c_str(),
              rig_backdrop::find(set.scene) == &set);
        check((tag + "the horizon is above the contact row").c_str(),
              set.rig.horizon_row < set.rig.contact_row);

        int opaque = 0, planes = 0, foreground = 0;
        float last_foot_factor = -1.0f;
        bool ladder = true;
        for (int i = 0; i < n; ++i) {
            const rig_backdrop::Layer& l = set.layers[i];
            opaque += l.opaque;
            planes += l.line_scroll;
            foreground += l.is_foreground;
            if (l.foot_row >= 0) {
                const float f = rig_backdrop::factor_of(set, l);
                ladder = ladder && f > last_foot_factor;
                last_foot_factor = f;
            }
        }
        check((tag + "exactly one opaque layer, and it is the backmost").c_str(),
              opaque == 1 && set.layers[0].opaque);
        check((tag + "exactly one line-scrolled plane").c_str(), planes == 1);
        check((tag + "exactly one foreground layer, and it is the frontmost").c_str(),
              foreground == 1 && set.layers[n - 1].is_foreground);
        check((tag + "standing layers are listed back to front: feet descend the plane").c_str(),
              ladder);

        for (int i = 0; i < n; ++i) {
            const rig_backdrop::Layer& l = set.layers[i];
            const std::string ltag = tag + l.file + ": ";
            bmp::Image img;
            std::string err;
            const std::string path = std::string(set.dir) + l.file;
            const bool ok = bmp::read(path.c_str(), img, &err);
            check((ltag + "reads").c_str(), ok, err);
            if (!ok) continue;
            check((ltag + "is the set's native size").c_str(),
                  img.width == set.native_w && img.height == set.native_h,
                  std::to_string(img.width) + "x" + std::to_string(img.height));
            if (img.width != set.native_w || img.height != set.native_h) continue;

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

            // The foot IS the factor. If the art's lowest painted row and the table
            // disagree, the layer scrolls at the speed of some other depth and its
            // feet slide on the plane.
            if (l.foot_row >= 0)
                check((ltag + "the lowest painted row is the table's foot row").c_str(),
                      lowest == l.foot_row,
                      "art says " + std::to_string(lowest) + ", table says " +
                          std::to_string(l.foot_row));

            if (l.opaque) {
                bool full = true;
                for (int y = 0; y < img.height && full; ++y)
                    for (int x = 0; x < img.width && full; ++x) full = !is_key(px(x, y));
                check((ltag + "the opaque layer has no transparent pixel").c_str(), full);
            }

            // The plane is transparent above the horizon and solid from it down: the
            // renderer starts its row loop at the horizon, so paint above it would
            // never be drawn and a hole below it would show the sky through the
            // ground.
            if (l.line_scroll) {
                bool shaped = true;
                for (int y = 0; y < img.height && shaped; ++y)
                    for (int x = 0; x < img.width && shaped; ++x)
                        shaped = is_key(px(x, y)) == (y < set.rig.horizon_row);
                check((ltag + "the plane starts exactly at the horizon and has no holes").c_str(),
                      shaped);
            }

            // Paint on the plane needs the plane under it: a row it covers that the
            // plane does not would be drawn at a placement nothing else shares.
            if (l.on_plane)
                check((ltag + "paint on the plane lies below the horizon").c_str(),
                      l.ripple_row0 >= set.rig.horizon_row && l.ripple_row1 > l.ripple_row0);

            // A rippled object layer is only drawn over its ripple rows.
            if (!l.line_scroll && l.ripple_row1 > l.ripple_row0)
                check((ltag + "all of a rippled layer's paint is inside its ripple rows").c_str(),
                      highest >= l.ripple_row0 && lowest < l.ripple_row1,
                      "painted " + std::to_string(highest) + ".." + std::to_string(lowest));

            // Every rig layer wraps, so its last column runs into its first. Not
            // provable pixel for pixel; what is checkable is that the wrap seam is no
            // harsher than the harshest ordinary column boundary in the same image.
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

} // namespace

int main() {
    test_pinhole();
    test_placement();
    test_sets();
    return report();
}
