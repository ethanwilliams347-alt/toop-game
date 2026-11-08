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

int Player::impact_damage(const BoxStrike& s) const {
    if (!s.hit || s.speed <= IMPACT_SAFE_SPEED || s.mass <= 0) return 0;
    // Whole cells a second over the threshold, as fall damage counts them, times
    // the body's share of the two masses: the share of the blow that is the
    // body's rather than the player's own give.
    const int64_t over = fx::trunc(s.speed - IMPACT_SAFE_SPEED);
    return static_cast<int>(over * s.mass / (s.mass + WEIGHT) / IMPACT_DAMAGE_DIVISOR);
}

void Player::press_bodies(const Grid& grid, const PlayerInput& input, bool was_on_ground,
                          fx::v impact_speed) {
    // A weight times the bodies' gravity, for one step: what resting on a body
    // presses it with (Grid::BODY_WEIGHT_ACCEL says why it is theirs).
    const int64_t per_step_weight = int64_t{fx::per_step(Grid::BODY_WEIGHT_ACCEL)};

    // Standing on one: its weight, every step, at the cell under its middle --
    // so a raft rides lower with the player on it and tips toward the end it
    // stands at. Landing on one: the fall's momentum as well, so a raft jumped
    // onto dips. Neither when nothing is moving: a piece at rest is not woken by
    // being stood on, which is Grid::queue_impulse's rule anyway, and skipping
    // the footing read is what keeps this free on still ground.
    if (body.on_ground && grid.tipping_count() > 0) {
        const BoxFooting f = body.footing(grid, BOX, body.y);
        if (f.body >= 0) {
            int64_t jy = WEIGHT * per_step_weight;
            if (!was_on_ground && impact_speed > 0) jy += int64_t{WEIGHT} * impact_speed;
            add_push(f.x, f.y, 0, jy);
        }
    }

    // A jump off one kicks it down: the jump's momentum is the body's, taken
    // from what it stood on. body.stood is what was underfoot before the move.
    if (did_flap && was_on_ground && body.stood.body >= 0)
        add_push(body.stood.x, body.stood.y, 0, int64_t{WEIGHT} * JUMP_SPEED);

    // Walking into one. Only with the feet on the ground -- a push needs
    // something to push from -- and only the way the walk key says, so a knock
    // that slams the body into a crate does not then lean on it.
    const int side = body.blocked;
    if (side == 0 || !body.on_ground || stagger > 0) return;
    if (!(side > 0 ? input.right && !input.left : input.left && !input.right)) return;

    // What stops it: the structure in the column just past the leading edge,
    // within the body's height. The push goes in at the middle of that, which
    // for a crate shorter than the player is the crate's middle -- it slides --
    // and for a post taller than it is the player's chest -- it may tip.
    const int cx = side > 0 ? body.x + WIDTH : body.x - 1;
    int rows[HEIGHT];
    int n = 0;
    for (int cy = body.y; cy < body.y + HEIGHT; ++cy)
        if (is_structural(grid.get_element(cx, cy).type)) rows[n++] = cy;
    if (n == 0) return;
    const int cy = rows[n / 2];

    // A motor, not a constant force: full strength from a standstill, nothing at
    // walking pace, so a crate the player is pushing never gets ahead of it.
    fx::v going = 0;
    const int b = grid.body_at(cx, cy);
    if (b >= 0) going = side * grid.body_velocity_at(b, cx, cy).vx;
    if (going >= MOVE_SPEED) return;
    const int64_t full = int64_t{PUSH_STRENGTH} * per_step_weight;
    const int64_t j = going <= 0 ? full : full * (MOVE_SPEED - going) / MOVE_SPEED;
    add_push(cx, cy, side * j, 0);
}

void Player::update(const Grid& grid, const PlayerInput& input) {
    // Cleared before the early return below, not after it. did_flap is a one-step
    // event and the overlap path returns without ever reaching the flap code, so
    // leaving the reset down there would latch the last beat true for every step
    // the body spent buried.
    did_flap = false;
    did_teleport = false;
    hurt_this_step = 0;
    push_n = 0;
    strike = BoxStrike{};
    immersed = 0;

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

    // A loose body that moved into the box this step -- the grid moved it without
    // knowing the body was there -- pushes it out along its motion and throws it
    // (BoxBody::shove), and hurts it if it came fast. Ahead of the dig-out
    // search, which would otherwise take the body out of a raft rising under it
    // as a burial: a jump, drawn as one (teleported()), a cell up every few
    // steps. Crushed against something, it is left for the search after all.
    //
    // What the body gained, the body that shoved it lost: pushed back on, at the
    // cell that struck, so a raft nudging the player is slowed by its weight and
    // a slab is not.
    strike = body.shove(grid, BOX, body.push_x + body.carry_x);
    if (strike.hit) {
        hurt(impact_damage(strike));
        if (strike.dvx != 0 || strike.dvy != 0)
            add_push(strike.x, strike.y, -int64_t{WEIGHT} * strike.dvx,
                     -int64_t{WEIGHT} * strike.dvy);
    }

    // Running the normal movement code while inside terrain would just find every
    // direction blocked, so digging out replaces this step entirely.
    if (resolve_overlap(grid)) return;

    // A blow's throw, now that the body is free to take it (see knock()).
    if (knock_x != 0 || knock_y != 0) {
        body.push_x += knock_x;
        body.vel_y += knock_y;
        knock_x = 0;
        knock_y = 0;
    }

    // Standing on a body that has dropped a cell out from under the feet since
    // last step: follow it down, before anything asks whether the body is on the
    // ground (BoxBody::stick).
    body.stick(grid, BOX);

    // Where the feet were at the start of the step, which is the only way to
    // recognise a landing. See the fall-damage block at the bottom of this function
    // for why the obvious test -- did the downward move get blocked -- is not the
    // one used.
    const bool was_on_ground = body.on_ground;

    // No acceleration curve: horizontal speed is a direct function of input.
    // Barebones on purpose -- acceleration, friction and air control are feel work,
    // and feel work is worth doing once there is something to feel.
    //
    // Except just after a knock: the legs give the throw a moment before they
    // fight it (KNOCK_STAGGER_STEPS).
    body.vel_x = 0;
    if (stagger > 0) {
        --stagger;
    } else {
        if (input.left) body.vel_x -= MOVE_SPEED;
        if (input.right) body.vel_x += MOVE_SPEED;
    }

    // How deep in water or oil the body is, for the stroke below and the drag
    // after it. Zero out of water, which leaves both exactly as they were.
    const Immersion wet = body.immersion(grid, BOX);
    immersed = wet.share;

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
    //
    // Underwater the air beat is a stroke (SWIM_IMPULSE, SWIM_MAX_CLIMB), by the
    // share of the body that is under: wings coming clear of the surface get
    // their full beat back, and that is how a bird climbs out of a pool. Out of
    // water the share is zero and these are the flight constants exactly.
    if (flap_timer > 0) --flap_timer;
    if (input.jump && flap_timer == 0) {
        if (body.on_ground) {
            body.vel_y = -JUMP_SPEED;
        } else {
            const fx::v beat = FLAP_IMPULSE + fx::mul(SWIM_IMPULSE - FLAP_IMPULSE, wet.share);
            const fx::v climb =
                FLAP_MAX_CLIMB + fx::mul(SWIM_MAX_CLIMB - FLAP_MAX_CLIMB, wet.share);
            body.vel_y -= beat;
            if (body.vel_y < -climb) body.vel_y = -climb;
        }
        flap_timer = FLAP_INTERVAL_STEPS;
        did_flap = true;
    }

    // What it stands on moving under it (BoxBody::ride), and the water it is in
    // (BoxBody::wade): both nothing on still ground in air.
    body.ride(grid, BOX);
    body.wade(wet, GRAVITY, DENSITY);

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

    // And what the body did to loose bodies this step, for Run to queue.
    press_bodies(grid, input, was_on_ground, impact_speed);
}
