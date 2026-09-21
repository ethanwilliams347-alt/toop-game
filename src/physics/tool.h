#pragma once
#include "grid.h"
#include "player.h"  // RANGE and RADIUS are stated as ratios of the body size

// The player's verbs -- the things it does to the world, as opposed to the
// things the world does to it.
//
// Deliberately not methods on Player. Keeping Player free of any grid write is
// what makes it trivially correct against the rule that every mutation goes
// through set_element / swap_elements: a class holding only a const Grid& cannot
// break that rule by accident. Tools take a mutable Grid& and are the only
// player-side code that does.
class DigTool {
public:
    // How far the dig reaches from the player's centre, in cells. Roughly three
    // body-heights: far enough to clear a path ahead, short enough that the player
    // has to commit to a position rather than deleting the level from across the
    // screen. Stated as a multiple of Player::HEIGHT so the ratio survives a change
    // to the body size.
    static constexpr int RANGE = 3 * Player::HEIGHT;

    // Radius of the hole taken out at the impact point. Three quarters of the
    // body's width, so the bite is one and a half bodies across: clearly visible
    // without being an explosion. Stated as a ratio for the same reason RANGE is.
    static constexpr int RADIUS = 3 * Player::WIDTH / 4;

    // A swing, not a cooldown. A bare rate limit has no notion of a swing at all,
    // which leaves the swing's length owned by the animation table while the digs it
    // portrays arrive on a different clock -- two clocks in two files that nobody
    // compares, so a held dig restarts the animation before it can reach its second
    // frame.
    //
    // There is one clock and it lives here, on the fixed step, because rendering
    // must never drive simulation. The animation is told where in the swing the tool
    // is (see swing_progress) and owns none of the timing.
    //
    // Deliberately longer than the walk cycle, so a swing reads as heavier than a
    // stride. That slows digging considerably; the dig radius is the knob if it
    // reads as feeble, not this.
    static constexpr int SWING_STEPS = 36;

    // The hole comes out on the first step of the swing, and the rest of the swing
    // is follow-through. Putting the impact two thirds in, where a real swing's arc
    // bottoms out, is the more literal reading of the motion, but it costs the
    // player half a second between pressing the button and the world changing on a
    // tool used constantly -- and a dig that lands late reads as input lag rather
    // than as weight.
    //
    // Two consequences, both the opposite of what a deferred impact would need. The
    // aim used is the aim at the moment of the press, which is exactly right when
    // there is no time in between for it to have moved, so there is no second ray
    // march and no window in which the world can change under the swing. And the ray
    // that decides whether a swing starts at all is the same ray that digs.

    // Advances the swing and digs from (from_x, from_y) toward (aim_x, aim_y).
    // Returns true on the step the hole is taken out, which is the step a swing
    // begins -- so at most once per SWING_STEPS.
    //
    // Call once per fixed step, whether or not the button is held: the swing only
    // advances when this is called, so skipping the call while not digging would
    // leave a swing suspended halfway through.
    bool update(Grid& grid, bool held, int from_x, int from_y, int aim_x, int aim_y);

    // Where the swing is, as a fraction from 0 (just started) up to but never
    // reaching 1, or -1 when no swing is in progress.
    //
    // A fraction rather than a step count, so the swing's length is not
    // reconstructible by the consumer. "Step 14 of 36" requires the consumer to hold
    // 36, and the moment two files hold the swing's length one of them can be
    // retuned alone. A fraction is complete on its own, and the animation divides it
    // by its own frame count without ever learning this clock's units.
    float swing_progress() const {
        return swing < 0 ? -1.0f : static_cast<float>(swing) / SWING_STEPS;
    }

    // Where the current aim would land, for drawing a cursor. Reports the impact
    // point if the ray hits something, otherwise the end of its range. Read-only:
    // takes a const Grid& and shares the march with update().
    void aim_point(const Grid& grid, int from_x, int from_y, int aim_x, int aim_y,
                   int& out_x, int& out_y) const;

    // Ready means not mid-swing: the tool will act on the button this step.
    bool is_ready() const { return swing < 0; }

private:
    // The step reached within the current swing, or -1 for idle.
    int swing = -1;

    // Marches one cell at a time from the origin toward the aim and reports where it
    // stops. One cell per step for exactly the reason the player's movement
    // sub-steps: a ray that skips cells digs through a wall into whatever is behind
    // it, and the wall it skipped is the one the player was standing behind.
    void march(const Grid& grid, int from_x, int from_y, int aim_x, int aim_y,
               int& out_x, int& out_y, bool& out_hit) const;
};
