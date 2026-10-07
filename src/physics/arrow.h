#pragma once
#include <array>
#include <cstdint>
#include "enemy.h"
#include "fixed.h"
#include "grid.h"

// One arrow: a point with a velocity, outside the grid like the player is.
//
// The tip is what matters, and it is a whole cell plus an `fx` remainder for the
// reason Player's position is -- collision only ever asks about whole cells, so an
// arrow is never a fraction of a cell into a wall.
struct Arrow {
    bool live = false;

    // Embedded in terrain and no longer moving. Kept, drawn, and counted down
    // rather than removed on impact, because an arrow that vanishes the moment it
    // hits the ground reads as a miss nobody saw.
    bool stuck = false;

    int x = 0;
    int y = 0;
    fx::v rem_x = 0;
    fx::v rem_y = 0;
    fx::v vel_x = 0;
    fx::v vel_y = 0;

    // Where the tip was at the start of this step, for the renderer's
    // interpolation. An arrow covers several cells a step; drawn only at whole
    // steps it strobes.
    int prev_x = 0;
    int prev_y = 0;
    fx::v prev_rem_x = 0;
    fx::v prev_rem_y = 0;

    // Steps left before a stuck arrow is cleared away.
    int life = 0;
};

// The bow and every arrow in the air. The player's second verb, after DigTool,
// and built the same way: a tool owns its own clock on the fixed step, takes a
// mutable Grid& because hitting an enemy writes sand into the world, and keeps
// Player free of any write.
//
// The arrows are a fixed pool, not a vector, because they are created on the step
// path and the step path allocates nothing. When every slot is taken the oldest
// arrow is recycled -- which in practice is one sticking out of a wall -- rather
// than the shot being refused: a bow that silently stops firing reads as broken.
class Quiver {
public:
    static constexpr int CAPACITY = 48;

    // Cells per second at release. Eight cells a step: fast enough that a shot
    // across the screen lands before the target has moved far, slow enough that
    // the flight is visible. Every one of those cells is visited -- see
    // update_arrows() -- so this is not a tunnelling risk at any value.
    static constexpr fx::v LAUNCH_SPEED = fx::from_int(480);

    // Well under the player's gravity. The cursor is the aim, and over the
    // distance a shot is usually taken at -- a body's width to half a screen --
    // the droop has to stay inside the target or aiming at a body stops meaning
    // hitting it. At this value a shot across 160 cells drops about eight, which
    // is still a visible arc and still lands in the chest when aimed at the head.
    static constexpr fx::v GRAVITY = fx::from_int(150);
    static constexpr fx::v MAX_FALL_SPEED = fx::from_int(500);

    // Fixed steps between shots while the key is held. A third of a second: a
    // held key is a steady volley, not a hose.
    static constexpr int DRAW_STEPS = 20;

    // How long an arrow stays stuck in a wall before it is cleared away.
    static constexpr int STUCK_STEPS = 4 * fx::STEPS_PER_SECOND;

    // The bite an arrow takes out of a body, in cells around the pixel it struck.
    // Radius 2 is a disc five cells across, which is wider than an arm (three) and
    // narrower than the torso (eight): a shot to a limb takes the limb through, a
    // shot to the chest takes a hole out of it. The severing fill in Enemy does
    // the rest, so a hit at the shoulder brings the whole arm down.
    static constexpr int BITE_RADIUS = 2;

    // Through water and oil an arrow keeps this share of its speed per step, in
    // percent. Without it a shot into a pond crosses the pond at full speed, which
    // is the one thing everyone knows an arrow does not do.
    static constexpr int FLUID_DRAG_PERCENT = 80;

    // Advances the draw and, if the key is held and the bow is drawn, looses an
    // arrow from (from_x, from_y) toward (aim_x, aim_y). Returns true on the step an
    // arrow leaves. Call once per fixed step whether or not the key is held, for the
    // reason DigTool::update gives.
    bool update_bow(bool held, int from_x, int from_y, int aim_x, int aim_y);

    // Moves every live arrow one step, cell by cell, stopping each at the first
    // surviving enemy pixel or solid cell on its path. Returns the number of enemy
    // pixels the arrows took out this step.
    int update_arrows(Grid& grid, Enemy* enemies, int enemy_count);

    const std::array<Arrow, CAPACITY>& arrows() const { return pool; }

    // Ready means the next held step fires.
    bool is_ready() const { return draw <= 0; }

private:
    std::array<Arrow, CAPACITY> pool{};
    int draw = 0;

    // Whether the arrow should stop in the cell it has just entered, and why.
    // Out of bounds reads as Wall through Grid::get_element, so the world's edge
    // catches arrows the same way it catches the player.
    void step_one(Arrow& a, Grid& grid, Enemy* enemies, int enemy_count, int& hit_pixels);
};

// The volley is meant to be a steady rhythm and not a single shot: an interval
// shorter than one step would be firing on every step, and one longer than a
// second is a reload rather than a draw.
static_assert(Quiver::DRAW_STEPS >= 2 && Quiver::DRAW_STEPS <= fx::STEPS_PER_SECOND,
              "the bow's draw is outside the range a held key reads as a volley");

// The bite has to sever a limb or the severing fill has nothing to do: an arm in
// enemy_art.h is three cells across, so the bite's diameter has to clear that.
static_assert(2 * Quiver::BITE_RADIUS + 1 > 3,
              "an arrow's bite is narrower than an arm, so a limb shot leaves the "
              "limb attached by a thread");
