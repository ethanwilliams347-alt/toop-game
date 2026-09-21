#include "player.h"
#include <algorithm>
#include <cstdlib>

Player::Player(int start_x, int start_y) : pos_x(start_x), pos_y(start_y) {}

bool Player::overlaps_solid(const Grid& grid, int px, int py) const {
    for (int cy = py; cy < py + HEIGHT; ++cy) {
        for (int cx = px; cx < px + WIDTH; ++cx) {
            // get_element() reads out-of-bounds as Wall, which is exactly right
            // here: the same border that seals the sand in also stops the player
            // walking out of the world.
            if (is_solid(grid.get_element(cx, cy).type)) return true;
        }
    }
    return false;
}

// The same box overlaps_solid walks, asking a different question of it. Kept as
// its own scan rather than folded into that one: they are called at different
// moments -- this once per step, that one several times on speculative positions
// the body may not end up in -- and a combined "is it solid and how hot is it"
// would have to be called with one of the answers discarded.
uint8_t Player::hottest_overlap(const Grid& grid) const {
    uint8_t hottest = 0;
    for (int cy = pos_y; cy < pos_y + HEIGHT; ++cy) {
        for (int cx = pos_x; cx < pos_x + WIDTH; ++cx) {
            const uint8_t t = grid.get_element(cx, cy).temperature;
            if (t > hottest) hottest = t;
        }
    }
    return hottest;
}

void Player::hurt(int amount) {
    if (amount <= 0) return;
    if (amount > hp) amount = hp;  // clamp before the subtraction, so the event matches the loss
    hp -= amount;
    hurt_this_step += amount;
}

// Blocked at foot height is not the same as blocked. Before calling it a wall,
// try lifting the whole body by up to MAX_STEP_HEIGHT and re-testing: if the
// body fits there, what we hit was a step. Testing the destination box rather
// than just the blocking cell is what makes this safe -- a position with no
// overlap anywhere cannot be inside geometry, so there is nothing to tunnel
// through.
//
// Grounded only, so the player cannot climb the side of a shaft by repeatedly
// nudging into it mid-air.
int Player::climb_for(const Grid& grid, int sign) const {
    if (!overlaps_solid(grid, pos_x + sign, pos_y)) return 0;
    if (!on_ground) return -1;

    for (int up = 1; up <= MAX_STEP_HEIGHT; ++up) {
        if (!overlaps_solid(grid, pos_x + sign, pos_y - up)) return up;
    }
    return -1;
}

void Player::move_x(const Grid& grid, int amount) {
    const int sign = (amount > 0) ? 1 : -1;

    for (int i = 0; i < std::abs(amount); ++i) {
        const int climbed = climb_for(grid, sign);

        if (climbed < 0) {
            // A real wall. Drop the leftover sub-cell motion too, otherwise it
            // accumulates while held against the wall and fires the instant the wall
            // is removed.
            vel_x = 0;
            rem_x = 0;
            return;
        }

        pos_x += sign;
        pos_y -= climbed;
    }
}

void Player::move_y(const Grid& grid, int amount) {
    const int sign = (amount > 0) ? 1 : -1;

    for (int i = 0; i < std::abs(amount); ++i) {
        if (overlaps_solid(grid, pos_x, pos_y + sign)) {
            // Landed on something, or hit a ceiling. Either way the fall (or the
            // jump) is over.
            vel_y = 0;
            rem_y = 0;
            return;
        }
        pos_y += sign;
    }
}

// The player is not a grid cell, so the grid does not know it is there and will
// happily drop sand into the cells the body occupies. That is the cost of the
// rigid-body split, and it means "body overlaps terrain" is a state that occurs
// in normal play -- being buried by a collapse, or spawning into a wall --
// rather than only through a bug.
//
// Without a way out of it, every move is blocked in every direction and the
// player is frozen for good. So: look outward for the nearest position the body
// does fit in and take it, preferring straight up. If nothing is open within the
// search radius the body is deeply buried, and it grinds upward one cell a step
// until the search can reach open air.
//
// A position the body fits in is not on its own a position the body may go to.
// Testing only the destination relocates a body buried against a wall to the
// nearest open ring cell even when that cell is on the far side of the wall, so
// being poured on becomes a way through terrain. escape_is_reachable is the
// missing half.
int Player::overlap_depth(const Grid& grid, int px, int py) const {
    int n = 0;
    for (int cy = py; cy < py + HEIGHT; ++cy)
        for (int cx = px; cx < px + WIDTH; ++cx)
            if (is_solid(grid.get_element(cx, cy).type)) ++n;
    return n;
}

// The escape has to be somewhere the body can get to, and "no solid on the way"
// is not the test -- a buried body is surrounded by solid, so a path test
// written that way rejects every escape and freezes exactly the case
// resolve_overlap() exists for. What separates climbing out of a metre of sand
// from stepping through a wall is not whether the path is solid but which way
// the burial goes: grinding up out of sand leaves the body less buried at every
// cell along the way, while crossing a wall means burrowing into a mass the body
// was not in before and only coming out the other side.
//
// So the rule is that being stuck may not get worse on the way out. Walk the
// straight line to the candidate a cell at a time and reject it the moment the
// body is deeper in solid than it was the cell before. A wall the body is flush
// against is a spike in that number at the first step; the sand above it is a
// slope down to zero.
//
// It costs a body-sized scan per cell of path, which is why it is called only on
// candidates the ring has already found to be open, and only on the steps where
// the body is buried at all.
bool Player::escape_is_reachable(const Grid& grid, int dx, int dy) const {
    const int steps = std::max(std::abs(dx), std::abs(dy));
    int depth = overlap_depth(grid, pos_x, pos_y);

    for (int k = 1; k <= steps; ++k) {
        const int here = overlap_depth(grid, pos_x + (dx * k) / steps,
                                       pos_y + (dy * k) / steps);
        if (here > depth) return false;
        depth = here;
    }
    return true;
}

bool Player::resolve_overlap(const Grid& grid) {
    if (!overlaps_solid(grid, pos_x, pos_y)) return false;

    for (int r = 1; r <= MAX_UNSTUCK_RADIUS; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int i = 0; i <= 2 * r; ++i) {
                // Walk the ring row outward from the centre -- 0, -1, 1, -2, 2 -- rather
                // than left to right. Both orders find a spot equally fast, but scanning
                // from the left edge takes a diagonal escape whenever a straight-up one
                // of the same distance exists, which reads on screen as the player being
                // flicked sideways for no reason.
                const int dx = (i % 2 == 0) ? (i / 2) : -(i / 2 + 1);

                if (std::max(std::abs(dx), std::abs(dy)) != r) continue;  // ring only
                if (overlaps_solid(grid, pos_x + dx, pos_y + dy)) continue;
                if (!escape_is_reachable(grid, dx, dy)) continue;

                pos_x += dx;
                pos_y += dy;
                rem_x = 0;
                rem_y = 0;
                vel_y = 0;
                return true;
            }
        }
    }

    // Buried deeper than the search reaches. Climb, unless doing so would push the
    // body out through the top of the world.
    if (pos_y > 0) pos_y -= 1;
    return true;
}

void Player::update(const Grid& grid, const PlayerInput& input) {
    // Cleared before the early return below, not after it. did_flap is a one-step
    // event and the overlap path returns without ever reaching the flap code, so
    // leaving the reset down there would latch the last beat true for every step
    // the body spent buried.
    did_flap = false;
    hurt_this_step = 0;

    // The burn rule, deliberately above the overlap early return rather than below
    // it. A body buried in burning terrain is exactly the case where being on fire
    // matters most, and putting this under the return would make being dug out of a
    // fire free -- a rule that switches itself off in the situation it is for.
    //
    // The timer is decremented before it is read, so the interval between ticks is
    // BURN_INTERVAL_STEPS and not one more than it, and a body that steps out of the
    // heat clears it rather than pausing it.
    if (burn_timer > 0) --burn_timer;
    if (hottest_overlap(grid) >= BURN_TEMPERATURE) {
        if (burn_timer == 0) {
            hurt(BURN_DAMAGE);
            burn_timer = BURN_INTERVAL_STEPS;
        }
    } else {
        burn_timer = 0;
    }

    // Running the normal movement code while inside terrain would just find every
    // direction blocked, so digging out replaces this step entirely.
    if (resolve_overlap(grid)) return;

    // Where the feet were at the start of the step, which is the only way to
    // recognise a landing. See the fall-damage block at the bottom of this function
    // for why the obvious test -- did the downward move get blocked -- is not the
    // one used.
    const bool was_on_ground = on_ground;

    // No acceleration curve: horizontal speed is a direct function of input.
    // Barebones on purpose -- acceleration, friction and air control are feel work,
    // and feel work is worth doing once there is something to feel.
    vel_x = 0;
    if (input.left)  vel_x -= MOVE_SPEED;
    if (input.right) vel_x += MOVE_SPEED;

    // Flapping, not jumping: the key beats wings on a fixed interval whether the
    // body is grounded or not.
    //
    // The ground beat and the air beat differ, and the difference is not a special
    // case. Leaving the ground is one hard downstroke against a body at rest -- it
    // sets vel_y outright, keeps JUMP_SPEED, and is allowed straight past
    // FLAP_MAX_CLIMB, which is what preserves the standing jump. A beat in the air
    // is a correction to a body already moving, so it subtracts from whatever the
    // velocity currently is and is capped.
    //
    // The timer is what makes this read as wingbeats rather than as thrust.
    // Applying a fraction of the impulse every step would produce the same altitude
    // curve and none of the rhythm, and the rhythm is the only thing that tells the
    // player, at this scale, that the bird is working.
    if (flap_timer > 0) --flap_timer;
    if (input.jump && flap_timer == 0) {
        if (on_ground) {
            vel_y = -JUMP_SPEED;
        } else {
            vel_y -= FLAP_IMPULSE;
            if (vel_y < -FLAP_MAX_CLIMB) vel_y = -FLAP_MAX_CLIMB;
        }
        flap_timer = FLAP_INTERVAL_STEPS;
        did_flap = true;
    }

    // Standing on the floor otherwise lets gravity pile up unbounded, which makes
    // velocity_y() meaningless and gives a one-frame lurch when the floor is
    // removed.
    //
    // rem_y has to go with it. The remainder is pending, collision-untested motion,
    // so cancelling the velocity that produced it while leaving it in place keeps
    // exactly the movement that was just decided against. Standing still, that is a
    // slow sink: gravity re-adds a step's worth every step, rem_y grows, and move_y
    // is not called at all until it crosses a whole cell -- at which point the floor
    // test finally runs, blocks, and snaps the body back. The result is a continuous
    // bob, invisible only while the renderer truncates the fraction away.
    if (on_ground && vel_y > 0) {
        vel_y = 0;
        rem_y = 0;
    }

    // One step's worth of GRAVITY, folded at compile time. Writing GRAVITY * dt
    // recomputes the same product every step; this is the same number, decided once,
    // and it is exactly the same number on every machine -- which the product was
    // not.
    vel_y += fx::per_step(GRAVITY);
    if (vel_y > MAX_FALL_SPEED) vel_y = MAX_FALL_SPEED;

    // The same rule on the other three sides, applied before the remainder is
    // accumulated rather than after it has already carried the body into geometry.
    // Without these, a body held against a surface drifts up to a full cell into it
    // before a whole-cell step is ever attempted, which is seen as phasing into
    // walls, sand and wood. climb_for rather than a bare overlap test, so walking up
    // a one-cell step is still a move rather than a wall.
    if (vel_x != 0 && climb_for(grid, vel_x > 0 ? 1 : -1) < 0) {
        vel_x = 0;
        rem_x = 0;
    }
    if (vel_y < 0 && overlaps_solid(grid, pos_x, pos_y - 1)) {
        vel_y = 0;
        rem_y = 0;
    }

    // Axes are resolved separately, horizontal first, so that sliding along a
    // surface works: being blocked vertically must not also cancel the horizontal
    // move. fx::trunc truncates toward zero, which is what the remainder scheme
    // needs in both directions and is why it is not a shift.
    rem_x += fx::per_step(vel_x);
    const int step_x = fx::trunc(rem_x);
    rem_x = fx::frac(rem_x);
    if (step_x != 0) move_x(grid, step_x);

    // Read before the move, because the move is what destroys it: move_y zeroes
    // vel_y the moment it hits something. This is the speed the body was actually
    // travelling at when it arrived, to within the one step of gravity applied
    // above.
    const fx::v impact_speed = vel_y;

    rem_y += fx::per_step(vel_y);
    const int step_y = fx::trunc(rem_y);
    rem_y = fx::frac(rem_y);
    if (step_y != 0) move_y(grid, step_y);

    // Asked once, at the end, against the world the body actually ended up in.
    // Deriving it from "did the downward move get blocked" instead would report
    // false on any step slow enough not to attempt a whole cell of movement.
    on_ground = overlaps_solid(grid, pos_x, pos_y + 1);

    // Fall damage. A landing is on_ground going from false to true, not move_y
    // reporting a block, and the difference is a whole class of missed landings: a
    // body falling six cells a step onto a floor exactly six cells below walks the
    // full distance without ever being blocked, ends the step flush on the ground
    // with its velocity intact, and has vel_y quietly zeroed by the resting rule at
    // the top of the next step. Written the obvious way, terminal-velocity falls do
    // no damage whenever the arithmetic happens to come out even.
    //
    // Damage is linear in the speed over a safe landing; fx::trunc takes both sides
    // to whole cells per second first, so the subtraction and the divide are plain
    // integers rather than fixed-point ones.
    //
    // The first landing of a run is free, and that is about the spawn rather than
    // mercy. Run puts the body high up in open air on purpose, so every run opens
    // with a fall that reaches terminal velocity -- priced by the rule above, most
    // of the health bar for doing nothing. It is a property of how the world is set
    // up, so it is spent here rather than worked around by moving the spawn, which
    // Run cannot place on terrain it does not know about.
    if (!was_on_ground && on_ground) {
        if (has_landed && impact_speed > SAFE_FALL_SPEED) {
            hurt(fx::trunc(impact_speed - SAFE_FALL_SPEED) / FALL_DAMAGE_DIVISOR);
        }
        has_landed = true;
    }
}
