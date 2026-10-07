// The camera's view arithmetic: where the followed point lands on screen, and
// what happens at the world's edges.
//
// Links no engine sources at all -- camera.h is an SDL-free header.
//
// The framing check below is written against Camera::VERTICAL_ANCHOR rather than
// against the number spelled twice, so retuning the composition is one edit;
// what is pinned here is that the constant reaches the screen at all.

#include "game/camera.h"
#include "render/depth_rig.h"
#include <string>
#include "test_util.h"

#include <cmath>

namespace {

constexpr int VIEWPORT_W = 480;
constexpr int VIEWPORT_H = 270;  // the shipped mode's viewport height in cells
constexpr int WORLD_W = 1920;
constexpr int WORLD_H = 1080;

bool near(float a, float b, float tol = 0.001f) { return std::fabs(a - b) <= tol; }

// Where the followed point lands on screen, as a fraction of viewport height.
// The framing is asserted on this rather than on view_y(), which is the same
// claim one step further from what a player actually sees: a framing that the
// world-edge clamp cannot honour looks correct in view_y() terms and wrong on
// screen.
float on_screen_fraction(const Camera& camera, float center_y) {
    return (center_y - (static_cast<float>(camera.view_y()) + camera.frac_y())) /
           static_cast<float>(VIEWPORT_H);
}

} // namespace

int main() {
    // Somewhere the view is unclamped on both axes, so the framing is the camera's
    // decision -- at a clamp the world edge decides instead.
    const float cy = 540.0f;

    {
        Camera camera;
        camera.follow(960.0f, cy, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H);
        check("the player is drawn at the vertical anchor, not at mid screen",
              near(on_screen_fraction(camera, cy), Camera::VERTICAL_ANCHOR),
              std::to_string(on_screen_fraction(camera, cy)));
        check("and the anchor is not the centring V23b restored",
              Camera::VERTICAL_ANCHOR != 0.5f, "");
        check("and centred horizontally too",
              near((960.0f - (static_cast<float>(camera.view_x()) + camera.frac_x())) /
                       static_cast<float>(VIEWPORT_W),
                   0.5f),
              "");
    }

    // The clamps. The view must never walk off the world, or the renderer uploads
    // from outside the grid -- and the clamp is also what makes a low framing
    // undeliverable near the floor, so it is worth pinning at both edges rather
    // than assuming symmetry.
    {
        Camera at_floor;
        at_floor.follow(960.0f, 1075.0f, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H);
        check("the view clamps at the world's bottom edge",
              at_floor.view_y() == WORLD_H - VIEWPORT_H, std::to_string(at_floor.view_y()));
        // The clamp beats the anchor. Near the floor the framing cannot be
        // honoured, so the player rides below the anchor rather than sitting at it:
        // the constant states an intent, not a guarantee. The spawn is close enough
        // to the floor for this to be visible.
        check("near the floor the clamp overrides the framing",
              on_screen_fraction(at_floor, 1075.0f) > Camera::VERTICAL_ANCHOR,
              std::to_string(on_screen_fraction(at_floor, 1075.0f)));

        Camera at_ceiling;
        at_ceiling.follow(960.0f, 4.0f, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H);
        check("the view clamps at the world's top edge", at_ceiling.view_y() == 0, "");

        Camera at_left;
        at_left.follow(4.0f, cy, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H);
        check("the view clamps at the world's left edge", at_left.view_x() == 0, "");

        Camera at_right;
        at_right.follow(1916.0f, cy, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H);
        check("the view clamps at the world's right edge",
              at_right.view_x() == WORLD_W - VIEWPORT_W, std::to_string(at_right.view_x()));

        // A world no larger than the viewport does not scroll at all, rather than
        // clamping to a negative view.
        Camera tiny;
        tiny.follow(100.0f, 100.0f, VIEWPORT_W, VIEWPORT_H, 200, 200);
        check("a world smaller than the viewport pins at the origin",
              tiny.view_x() == 0 && tiny.view_y() == 0, "");
    }

    // The whole view is fractional and only the index is floored, because a camera
    // that can sit only on whole cells reads as ghosting. Floored and not truncated,
    // so a negative intermediate picks the cell on the correct side.
    {
        Camera camera;
        camera.follow(960.25f, cy, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H);
        check("the leftover sub-cell offset survives as frac", near(camera.frac_x(), 0.25f),
              std::to_string(camera.frac_x()));
        check("frac is never negative", camera.frac_x() >= 0.0f && camera.frac_y() >= 0.0f, "");

        // Round trip through both conversions, at a fractional view: a screen pixel
        // resolves to the cell it is drawn from.
        const int cell = camera.screen_to_world_x(0);
        check("screen_to_world floors into the view's own cell", cell == camera.view_x(),
              std::to_string(cell) + " vs " + std::to_string(camera.view_x()));
    }

    // Parallax origins are a function of the view and nothing else. The check that
    // matters is the pair of endpoints: a factor of 1 locks a layer to the world,
    // and 0 pins it to the window.
    {
        Camera camera;
        camera.follow(960.0f, cy, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H);
        check("a factor of 0 pins a layer to the window",
              near(camera.parallax_origin_x(0.0f), 0.0f) &&
                  near(camera.parallax_origin_y(0.0f), 0.0f),
              "");
        // A factor of 1 puts the layer's origin exactly where world cell 0 lands,
        // which is what "locked to the world" means.
        check("a factor of 1 locks a layer to the world",
              near(camera.parallax_origin_x(1.0f), camera.world_to_screen_x(0.0f)) &&
                  near(camera.parallax_origin_y(1.0f), camera.world_to_screen_y(0.0f)),
              std::to_string(camera.parallax_origin_x(1.0f)) + " vs " +
                  std::to_string(camera.world_to_screen_x(0.0f)));
    }

    // --- the two scene paradigms ----------------------------------------
    //
    // follow and follow_mode(..., false) must be the same framing, and that is
    // asserted rather than assumed because follow is what every recorded session and
    // the golden fixture go through.
    {
        Camera a, b;
        a.follow(960.0f, cy, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H);
        b.follow_mode(960.0f, cy, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H, false);
        check("follow is follow_mode with the bounded rule",
              near(a.view_fx(), b.view_fx()) && near(a.view_fy(), b.view_fy()), "");
    }

    // An infinite scene drops the horizontal clamp and only that one. Both halves
    // are checked at once, because the failure worth catching is the vertical clamp
    // being dropped along with it -- the texture upload reads rows out of the grid,
    // so a view above the world is a read outside it.
    {
        Camera far_left, far_right, high;
        far_left.follow_mode(-5000.0f, cy, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H, true);
        check("an infinite scene does not clamp at the left border",
              far_left.view_fx() < 0.0f, std::to_string(far_left.view_fx()));

        far_right.follow_mode(50000.0f, cy, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H, true);
        check("nor at the right one",
              far_right.view_fx() > static_cast<float>(WORLD_W - VIEWPORT_W),
              std::to_string(far_right.view_fx()));

        high.follow_mode(960.0f, -900.0f, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H, true);
        check("but the world still has a ceiling", near(high.view_fy(), 0.0f),
              std::to_string(high.view_fy()));

        Camera low;
        low.follow_mode(960.0f, 100000.0f, VIEWPORT_W, VIEWPORT_H, WORLD_W, WORLD_H, true);
        check("and a floor",
              near(low.view_fy(), static_cast<float>(WORLD_H - VIEWPORT_H)),
              std::to_string(low.view_fy()));
    }

    // --- a world-sized authored layer never runs out ---------------------
    //
    // The one inequality the authored backdrop rests on, and the reason its art is
    // exactly world-sized rather than merely large. For a bounded world of W cells
    // shown through a viewport of V, a layer W cells wide drawn at the corner
    // anchor, depth_rig::origin(cam, 0, f) -- what frame.cpp draws a
    // corner-anchored set at -- covers the whole viewport at every reachable camera
    // position exactly when f <= 1 -- so nothing has to tile, and nothing gaps.
    //
    // Checked at every camera position rather than at the extremes alone: the
    // binding case is the far edge, but a formula that was right at both ends and
    // wrong in between is exactly what an extremes-only test reports as passing.
    // f = 1.0 is included on purpose -- it is the cap the foreground sits at, so it
    // is the case with no slack in it at all.
    //
    // If this fails, the fix is the art or the factor, never a wider tolerance: a
    // gap here is the clear colour showing through a backdrop.
    {
        constexpr int SCALE = 10;  // the authored scene's scale
        constexpr int W = 344;  // world cells, and the art's own width
        constexpr int V = 193;  // padded viewport at 1920x1080
        const float factors[] = {0.04f, 0.12f, 0.20f, 0.42f, 0.70f, 1.00f};

        bool covered = true;
        std::string detail;
        for (float f : factors) {
            for (int centre = 0; centre <= W; ++centre) {
                Camera c;
                c.set_scale(SCALE);
                c.follow(static_cast<float>(centre), 0.0f, V, V, W, W);
                const float left  = depth_rig::origin(c.view_fx(), 0.0f, f, SCALE);
                const float right = left + static_cast<float>(W * SCALE);
                if (left > 0.001f || right < static_cast<float>(V * SCALE) - 0.001f) {
                    covered = false;
                    detail = "factor " + std::to_string(f) + " at centre " +
                             std::to_string(centre) + ": layer spans " +
                             std::to_string(left) + ".." + std::to_string(right) +
                             " for a window of 0.." + std::to_string(V * SCALE);
                    break;
                }
            }
            if (!covered) break;
        }
        check("a world-sized authored layer at factor <= 1 covers the viewport everywhere",
              covered, detail);
    }

    // The other half of the same fact, stated as the thing that would go wrong:
    // above 1.0 it does gap, which is why the foreground is capped at 1.00 rather
    // than given the art README's wider range -- for a corner-anchored set, whose
    // art does not tile. A standing-anchored set wraps and has no cap. A test that only proved the safe case
    // would leave "so raise it a bit" looking free.
    {
        constexpr int SCALE = 10;
        constexpr int W = 344;
        constexpr int V = 193;
        Camera c;
        c.set_scale(SCALE);
        c.follow(static_cast<float>(W), 0.0f, V, V, W, W);  // hard against the right edge
        const float right =
            depth_rig::origin(c.view_fx(), 0.0f, 1.20f, SCALE) + static_cast<float>(W * SCALE);
        check("and a factor above 1 gaps there, which is why 1.00 is the cap",
              right < static_cast<float>(V * SCALE),
              "right edge " + std::to_string(right) + " vs window " + std::to_string(V * SCALE));
    }

    return report();
}
