// The perspective rig: render/depth_rig.h's arithmetic. The shipped sets are
// held to their BMPs by backdrop_set_test.
//
// Links nothing at all. The draw path in render/frame.cpp is a loop over the functions
// tested here, which is the same split backdrop_test has with backdrop_wrap.h.
// What a person has to judge -- whether the lake reads as wide -- is
// preview_backdrop's job, not this file's.

#include "render/depth_rig.h"
#include "test_util.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {

bool near_eq(float a, float b, float tol = 0.01f) { return std::fabs(a - b) < tol; }

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
        for (int i = 0; i <= 26; ++i) {
            const float f = 0.05f * static_cast<float>(i);
            exact = exact && near_eq(depth_rig::origin(ay, ay, f, SCALE), -ay * SCALE) &&
                    near_eq(depth_rig::origin(ax, ax, f, SCALE), -ax * SCALE);
        }
        check((tag + "at the anchor every layer is placed where the world is").c_str(), exact);

        // The glue, swept over every camera position the game can reach: a layer
        // standing on plane row r lands its feet on the plane's own row r, on both
        // axes. This is the property the bg1 model had to give up the vertical
        // axis to keep.
        bool glued = true;
        std::string where;
        for (int cy_i = 0; static_cast<float>(cy_i) * 3.7f <= max_y; ++cy_i) {
            const float cy = static_cast<float>(cy_i) * 3.7f;
            for (int cx_i = 0; static_cast<float>(cx_i) * 41.3f <= max_x; ++cx_i) {
                const float cx = static_cast<float>(cx_i) * 41.3f;
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
        for (int cy_i = 0; static_cast<float>(cy_i) * 1.3f <= max_y; ++cy_i) {
            const float cy = static_cast<float>(cy_i) * 1.3f;
            for (int r = rig.horizon_row; r < world_h; ++r)
                forward = forward && depth_rig::plane_edge_y(rig, r + 1, cy, ay, SCALE) >
                                         depth_rig::plane_edge_y(rig, r, cy, ay, SCALE);
        }
        check((tag + "every plane row keeps a positive height at every camera height").c_str(),
              forward);

        // Coverage: a world-tall opaque layer at any factor in [0, 1] covers the
        // window at every reachable camera -- the convex-combination argument.
        bool covered = true;
        for (int f_i = 0; static_cast<float>(f_i) * 0.125f <= 1.0f; ++f_i) {
            const float f = static_cast<float>(f_i) * 0.125f;
            const float g = depth_rig::vertical_factor(rig, f);
            for (int cy_i = 0; static_cast<float>(cy_i) * 1.0f <= max_y; ++cy_i) {
                const float cy = static_cast<float>(cy_i) * 1.0f;
                const float top = depth_rig::origin(cy, ay, g, SCALE);
                covered = covered && top <= 0.0f &&
                          top + static_cast<float>(world_h * SCALE) >=
                              static_cast<float>(v.h * SCALE);
            }
        }
        check((tag + "a world-tall opaque layer always covers the window").c_str(), covered);
    }
}

} // namespace

int main() {
    test_pinhole();
    test_placement();
    return report();
}
