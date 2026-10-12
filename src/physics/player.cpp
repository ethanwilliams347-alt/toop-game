#include "player.h"
#include <algorithm>
#include <cstdlib>

Player::Player(int start_x, int start_y) {
    body.x = start_x;
    body.y = start_y;
}

bool Player::overlaps_solid(const Grid& grid, int px, int py) const {
    return body.overlaps(grid, BOX, px, py);
}

// The same box overlaps_solid walks, asking a different question of it. Kept as
// its own scan rather than folded into that one: they are called at different
// moments -- this once per step, that one several times on speculative positions
// the body may not end up in -- and a combined "is it solid and how hot is it"
// would have to be called with one of the answers discarded.
uint8_t Player::hottest_overlap(const Grid& grid) const {
    uint8_t hottest = 0;
    for (int cy = body.y; cy < body.y + HEIGHT; ++cy) {
        for (int cx = body.x; cx < body.x + WIDTH; ++cx) {
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
// until the search can reach open air -- but never up into static matter it was
// not already inside, which it has to dig through instead.
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
    int depth = overlap_depth(grid, body.x, body.y);

    for (int k = 1; k <= steps; ++k) {
        const int here = overlap_depth(grid, body.x + (dx * k) / steps, body.y + (dy * k) / steps);
        if (here > depth) return false;
        depth = here;
    }
    return true;
}

bool Player::would_enter_static(const Grid& grid) const {
    const auto is_static = [&](int x, int y) {
        const ElementType t = grid.get_element(x, y).type;
        return t != ElementType::Empty && material_of(t).move == MoveKind::Static;
    };
    // Column by column: a static cell above a column whose top cell is not static
    // is the edge of a mass the body would be pushing into. Above a column that is
    // already static, the body is inside that mass and rising through it.
    for (int cx = body.x; cx < body.x + WIDTH; ++cx)
        if (is_static(cx, body.y - 1) && !is_static(cx, body.y)) return true;
    return false;
}

bool Player::resolve_overlap(const Grid& grid) {
    if (!overlaps_solid(grid, body.x, body.y)) return false;

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
                if (overlaps_solid(grid, body.x + dx, body.y + dy)) continue;
                if (!escape_is_reachable(grid, dx, dy)) continue;

                body.x += dx;
                body.y += dy;
                body.rem_x = 0;
                body.rem_y = 0;
                body.vel_y = 0;
                did_teleport = true;
                return true;
            }
        }
    }

    // Buried deeper than the search reaches. Grind upward a cell a step -- through
    // whatever the body is already buried in, but never up into static matter it
    // was not inside. A body entombed in rubble grinds up through the rubble,
    // which is the case this exists for: a collapse dropping a hillside on the
    // player. A body buried in sand under a ceiling stops at the ceiling, because
    // rising through it is walking through a wall by another name -- the same
    // crossing escape_is_reachable refuses the ring search above. Stopped there,
    // the dig tool still works from inside the burial, so the player digs out. The
    // top of the world stops it too.
    if (body.y > 0 && !would_enter_static(grid)) body.y -= 1;
    return true;
}

void Player::update(const Grid& grid, const PlayerInput& input) {
    // Cleared before the early return below, not after it. did_flap is a one-step
    // event and the overlap path returns without ever reaching the flap code, so
    // leaving the reset down there would latch the last beat true for every step
    // the body spent buried.
    did_flap = false;
    did_teleport = false;
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
    const bool was_on_ground = body.on_ground;

    // No acceleration curve: horizontal speed is a direct function of input.
    // Barebones on purpose -- acceleration, friction and air control are feel work,
    // and feel work is worth doing once there is something to feel.
    body.vel_x = 0;
    if (input.left) body.vel_x -= MOVE_SPEED;
    if (input.right) body.vel_x += MOVE_SPEED;

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
        if (body.on_ground) {
            body.vel_y = -JUMP_SPEED;
        } else {
            body.vel_y -= FLAP_IMPULSE;
            if (body.vel_y < -FLAP_MAX_CLIMB) body.vel_y = -FLAP_MAX_CLIMB;
        }
        flap_timer = FLAP_INTERVAL_STEPS;
        did_flap = true;
    }

    // The box's motion, shared with every enemy (box_body.h). What comes back is
    // the speed the body was travelling at when it arrived, read before the move
    // that zeroes it -- to within the one step of gravity just applied.
    const fx::v impact_speed = body.integrate(grid, BOX, GRAVITY, MAX_FALL_SPEED);

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
    // mercy. Run builds the body in open air because it is constructed before any
    // terrain exists. A scene then normally stands it on the ground
    // (boot::stand_player_on_ground, which calls settle() so this step does not
    // mistake the placement for a landing), and the free landing is kept for the
    // first real fall. When there is no ground under the spawn column the body
    // is left in the air and falls the rest of the world's height, and this is
    // what keeps that fall from costing most of the health bar.
    if (!was_on_ground && body.on_ground) {
        if (has_landed && impact_speed > SAFE_FALL_SPEED) {
            hurt(fx::trunc(impact_speed - SAFE_FALL_SPEED) / FALL_DAMAGE_DIVISOR);
        }
        has_landed = true;
    }
}
