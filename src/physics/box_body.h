#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include "fixed.h"
#include "fixed_trig.h"
#include "grid.h"
#include "material.h"

// A body that moves through the grid as an axis-aligned box: the player, every
// enemy, and whatever walks next. The box is not in the grid -- the grid does not
// know it is there -- so this is the code that asks the grid where the box may
// go.
//
// It used to be written twice. Player and Enemy each had overlaps_solid,
// climb_for, move_x, move_y and the same gravity-then-remainder sequence, and
// enemy.cpp said so itself ("from here on it is Player::update's motion, minus
// the wings"). The copies already differed on purpose -- enemies ignore powder
// above their feet, trolls wade through it -- and those differences are now the
// one thing a body states about itself: its BoxRule. Everything else is shared,
// so a fix to how a box meets a step is a fix for every body at once.
//
// What stays with each body is what it decides: the player's flaps, burn and
// fall damage and its dig-out search; the enemy's AI, wading and pose. Those set
// vel_x and vel_y, and then integrate() moves the box exactly as both bodies
// moved it before.
//
// --- what the box does with the loose bodies, and with water ---
//
// The rest of this file is the box meeting the two parts of the world that
// move under it: loose bodies (Grid's "coupling" block) and liquids. Each is a
// thing the box asks of the grid and a velocity it adds to its own, and each is
// zero -- exactly zero, so the arithmetic of a step is the arithmetic it always
// was -- when there is no body and no water. Walking, jumping and flying feel
// the same until something new acts on the box.
//
//   push_x   a velocity of the box's own that is not its legs': a knock from a
//            blow, a shove from a body, the motion it kept stepping off a raft.
//            vel_x is set from input or AI every step, so a velocity that has to
//            outlast a step cannot live there; this one decays instead, quickly
//            on the ground and slowly in the air (KNOCK_GROUND_DECEL,
//            KNOCK_AIR_DRAG).
//   carry_*  the motion of the loose body the box stands on, this step: added
//            to the box's own so it rides a raft or a sliding slab rather than
//            having it slide out from under it.
//   shove()  a loose body that has moved into the box pushes it out along the
//            way the body was going, rather than burying it, and says how hard
//            it hit.
//   wade()   buoyancy and drag in a liquid.

// How a body's box meets the grid.
struct BoxRule {
    int width;
    int height;
    // Rise the box steps up without stopping, in cells.
    int max_step_height;
    // The bottom rows of the box in which powder collides. `height` is everywhere
    // (the player: sand is ground and wall alike); fewer is a body whose own
    // falling grains must pass through it above the feet (an enemy shedding
    // pixels); 0 is a body that wades through powder entirely (the troll).
    // Static matter and the world's border collide everywhere regardless.
    int powder_rows;
};

// A push the box asks to put into a loose body: Grid::queue_impulse's
// arguments, held until someone with a mutable Grid can queue it. The player
// reads the grid const (player.h says why), so it says what it pushed and Run
// queues it, the same way an enemy's blow is reported and Run applies it.
struct BoxPush {
    int x = 0;  // a cell of the body
    int y = 0;
    int64_t jx = 0;  // Material::weight units times fx cells per second
    int64_t jy = 0;
};

// What a loose body did to the box this step by moving into it, from shove().
struct BoxStrike {
    bool hit = false;
    int body = -1;         // its pool index, valid until the next update
    int x = 0;             // a cell of the body inside the box, nearest its middle:
    int y = 0;             //   where it struck, and where to push back
    int64_t mass = 0;      // the body's, Material::weight units
    // How much faster than the box the body was going, along the way it pushed
    // the box (the way it was moving, if it could not push it at all), in fx
    // cells per second. What a blow hurts by.
    fx::v speed = 0;
    // The change it made to the box's velocity, fx cells per second: the box's
    // momentum gained, which the body lost (the box pushes back with it).
    fx::v dvx = 0;
    fx::v dvy = 0;
    // Whether it pushed the box clear of itself. False is crushed: pinned against
    // something that would not let it go, and left to the box's own way out.
    bool moved = false;
};

// The loose body under a box's feet, if it stands on one.
struct BoxFooting {
    int body = -1;
    int id = 0;  // Grid::body_id of it, which outlasts the step
    int x = 0;   // the cell it rests on, the body's nearest the box's middle
    int y = 0;
    fx::v vx = 0;  // how that cell moved this step, fx cells per second
    fx::v vy = 0;
};

// How deep a box is in liquid: the share of it under (fx, 0 to ONE) and the
// heaviest liquid it is in, as Material::weight.
struct Immersion {
    fx::v share = 0;
    int weight = 0;
};

// --- the knobs, shared by every box ---

// A knock's speed bleeds off at this rate on the ground, in cells per second
// squared: a hard shove skids the box a few cells and stops it within a few
// steps, rather than sliding it across the level. About twice the player's own
// gravity, the friction of feet that are trying to stand.
inline constexpr fx::v KNOCK_GROUND_DECEL = fx::from_int(900);
// And this share of it a step in the air: little, so a box thrown by a blow
// flies an arc rather than stopping in mid-air, and lands still going.
inline constexpr fx::v KNOCK_AIR_DRAG = fx::from_ratio(1, 64);

// How far a box standing on a loose body follows it down in a step, in cells.
// A raft bobbing, a plank rocking, a slab sliding down a slope all drop their
// cells a whole cell at a time while their motion is smooth, and a box left a
// cell above one would fall onto it every few steps -- the fall pose flickering
// on and off under a player standing still. Two, so a body sinking faster than a
// cell a step is still followed, and a body falling away faster than that is let
// go: what stood on it falls after it.
inline constexpr int RIDE_SNAP = 2;

// The furthest a body can push a box out of itself in a step, in cells: as far
// as a body can move in a step (Grid::MAX_FALL_SPEED at its rim), and two for the
// rounding of its cells.
inline constexpr int MAX_SHOVE = Grid::MAX_FALL_SPEED + 2;

// Drag in a liquid, on a box all the way under; a box partly under feels the
// share of it that is. Form drag, which grows with speed, as v / (1 + k v) with v
// in cells a step -- the loose bodies' form, so a fall into a pool is slowed
// hard at the surface and never turned back -- and a viscous share per step,
// which is what brings a box sinking to a slow, steady rate.
inline constexpr fx::v WADE_FORM_DRAG = fx::from_ratio(1, 8);
inline constexpr fx::v WADE_DRAG = fx::from_ratio(1, 32);
// The share of its walking speed a box loses all the way under: legs pushing
// water, not air.
inline constexpr fx::v WADE_SLOW = fx::from_ratio(1, 2);

struct BoxBody {
    int x = 0;  // top-left cell of the box
    int y = 0;
    fx::v rem_x = 0;  // sub-cell motion not yet taken, fx cells
    fx::v rem_y = 0;
    fx::v vel_x = 0;  // cells/second
    fx::v vel_y = 0;
    bool on_ground = false;

    // See the top of the file. push_x is cells/second and decays; carry_* is the
    // footing's motion this step; riding is whether the box stood on a loose body
    // at the end of the last step. blocked is the side a wall stopped the box on
    // this step (-1 left, 1 right, 0 none), for a body that pushes what stops it.
    fx::v push_x = 0;
    fx::v carry_x = 0;
    fx::v carry_y = 0;
    bool riding = false;
    int blocked = 0;
    // The body ride() found under the feet this step, before the box moved: what
    // a jump off it pushes down on.
    BoxFooting stood{};

    // Whether the box, put at (px, py), overlaps anything that stops it.
    // get_element() reads out-of-bounds as Wall, which is exactly right here: the
    // same border that seals the sand in also stops a body leaving the world.
    bool overlaps(const Grid& grid, const BoxRule& rule, int px, int py) const {
        const int powder_top = py + rule.height - rule.powder_rows;
        for (int cy = py; cy < py + rule.height; ++cy) {
            for (int cx = px; cx < px + rule.width; ++cx) {
                const ElementType t = grid.get_element(cx, cy).type;
                if (!is_solid(t)) continue;
                if (material_of(t).move == MoveKind::Powder && cy < powder_top) continue;
                return true;
            }
        }
        return false;
    }

    // Blocked at foot height is not the same as blocked. Before calling it a
    // wall, try lifting the whole box by up to max_step_height and re-testing: if
    // it fits there, what it hit was a step. 0 for open ground, the rise for a
    // step, -1 for a wall. Testing the destination box rather than the blocking
    // cell is what makes this safe -- a position with no overlap anywhere cannot
    // be inside geometry, so there is nothing to tunnel through.
    //
    // Grounded only, so a body cannot climb the side of a shaft by nudging into
    // it mid-air.
    int climb_for(const Grid& grid, const BoxRule& rule, int sign) const {
        if (!overlaps(grid, rule, x + sign, y)) return 0;
        if (!on_ground) return -1;
        for (int up = 1; up <= rule.max_step_height; ++up)
            if (!overlaps(grid, rule, x + sign, y - up)) return up;
        return -1;
    }

    void move_x(const Grid& grid, const BoxRule& rule, int amount) {
        const int sign = amount > 0 ? 1 : -1;
        for (int i = 0; i < std::abs(amount); ++i) {
            const int climbed = climb_for(grid, rule, sign);
            if (climbed < 0) {
                // A real wall. Drop the leftover sub-cell motion too, otherwise it
                // accumulates while held against the wall and fires the instant
                // the wall is removed. A knock ends against it as well: what was
                // thrown into a wall has stopped.
                vel_x = 0;
                rem_x = 0;
                push_x = 0;
                blocked = sign;
                return;
            }
            x += sign;
            y -= climbed;
        }
    }

    void move_y(const Grid& grid, const BoxRule& rule, int amount) {
        const int sign = amount > 0 ? 1 : -1;
        for (int i = 0; i < std::abs(amount); ++i) {
            if (overlaps(grid, rule, x, y + sign)) {
                // Landed on something, or hit a ceiling. Either way the fall (or
                // the jump) is over.
                vel_y = 0;
                rem_y = 0;
                return;
            }
            y += sign;
        }
    }

    // One step of motion, after the body has set vel_x (and vel_y, for a jump or
    // a flap). Returns vel_y as it was just before the vertical move -- the speed
    // the body arrived at whatever it landed on, which move_y then zeroes -- for
    // a body that takes fall damage.
    fx::v integrate(const Grid& grid, const BoxRule& rule, fx::v gravity, fx::v max_fall) {
        // Standing on the floor otherwise lets gravity pile up unbounded, which
        // makes the velocity meaningless and gives a one-frame lurch when the floor
        // is removed. rem_y goes with it: the remainder is pending, untested
        // motion, and keeping it while cancelling the velocity that produced it is
        // a slow sink -- gravity re-adds a step's worth every step until it crosses
        // a cell, the floor test finally runs and snaps the body back, and the
        // result is a continuous bob.
        if (on_ground && vel_y > 0) {
            vel_y = 0;
            rem_y = 0;
        }

        // One step's worth of gravity, folded at compile time by the caller's
        // constant -- exactly the same number on every machine.
        vel_y += fx::per_step(gravity);
        if (vel_y > max_fall) vel_y = max_fall;

        // The horizontal motion is the legs' plus whatever else is moving the box:
        // a knock, and the body it stands on. Both are zero for a box nothing has
        // touched, which leaves this the sum it always was.
        blocked = 0;
        fx::v run_x = vel_x + push_x + carry_x;

        // The same rule on the other three sides, applied before the remainder is
        // accumulated rather than after it has carried the body into geometry.
        // Without these, a body held against a surface drifts up to a cell into it
        // before a whole-cell step is attempted -- phasing into walls. climb_for
        // rather than a bare overlap test, so walking up a step is still a move.
        if (run_x != 0 && climb_for(grid, rule, run_x > 0 ? 1 : -1) < 0) {
            blocked = run_x > 0 ? 1 : -1;
            vel_x = 0;
            rem_x = 0;
            push_x = 0;
            run_x = 0;
        }
        if (vel_y < 0 && overlaps(grid, rule, x, y - 1)) {
            vel_y = 0;
            rem_y = 0;
        }

        // Axes separately, horizontal first, so sliding along a surface works:
        // being blocked vertically must not also cancel the horizontal move.
        // fx::trunc truncates toward zero, which the remainder scheme needs in
        // both directions and is why it is not a shift.
        rem_x += fx::per_step(run_x);
        const int step_x = fx::trunc(rem_x);
        rem_x = fx::frac(rem_x);
        if (step_x != 0) move_x(grid, rule, step_x);

        const fx::v impact_speed = vel_y;
        rem_y += fx::per_step(vel_y);
        const int step_y = fx::trunc(rem_y);
        rem_y = fx::frac(rem_y);
        if (step_y != 0) move_y(grid, rule, step_y);

        // Asked once, at the end, against the world the body actually ended up
        // in. Deriving it from "did the downward move get blocked" instead would
        // report false on any step slow enough not to attempt a whole cell.
        on_ground = overlaps(grid, rule, x, y + 1);

        // A knock bleeds off after it has moved the box, so its first step goes
        // at its full speed. Nothing to do, and nothing done, for a box nothing
        // has knocked.
        if (push_x != 0) {
            if (on_ground) {
                const fx::v d = fx::per_step(KNOCK_GROUND_DECEL);
                push_x =
                    push_x > 0 ? std::max<fx::v>(0, push_x - d) : std::min<fx::v>(0, push_x + d);
            } else {
                push_x -= fx::mul(push_x, KNOCK_AIR_DRAG);
            }
        }
        return impact_speed;
    }

    // --- loose bodies ---

    // The loose body under the box's feet, with the box's bottom at row `py`
    // (the box at its own y unless asked about another). The row just under the
    // box, column by column; the body with the most cells there, and of its
    // cells the one nearest the box's middle, since that is where the box's
    // weight comes down. Cheap enough to ask every step: a box's width of
    // lookups, each a tag table read.
    BoxFooting footing(const Grid& grid, const BoxRule& rule, int py) const {
        BoxFooting f;
        if (grid.tipping_count() == 0) return f;
        const int row = py + rule.height;
        const int mid2 = 2 * x + rule.width;  // the middle, doubled, so it is an integer
        constexpr int MOST = 4;  // distinct bodies under one box; more is rubble
        int ids[MOST] = {-1, -1, -1, -1};
        int count[MOST] = {};
        int near_x[MOST] = {};
        for (int cx = x; cx < x + rule.width; ++cx) {
            const int b = grid.body_at(cx, row);
            if (b < 0) continue;
            int k = 0;
            while (k < MOST && ids[k] >= 0 && ids[k] != b) ++k;
            if (k == MOST) continue;
            if (ids[k] < 0) {
                ids[k] = b;
                near_x[k] = cx;
            } else if (std::abs(2 * cx + 1 - mid2) < std::abs(2 * near_x[k] + 1 - mid2)) {
                near_x[k] = cx;
            }
            ++count[k];
        }
        int best = -1;
        for (int k = 0; k < MOST && ids[k] >= 0; ++k)
            if (best < 0 || count[k] > count[best]) best = k;
        if (best < 0) return f;
        f.body = ids[best];
        f.id = grid.body_id(f.body);
        f.x = near_x[best];
        f.y = row;
        const Grid::BodyMotion m = grid.body_velocity_at(f.body, f.x, f.y);
        f.vx = m.vx;
        f.vy = m.vy;
        return f;
    }

    // A box that was standing on a loose body at the end of the last step, and
    // finds a gap under its feet now that the body has moved, follows it down --
    // by up to RIDE_SNAP cells, and only onto a loose body, so stepping off a raft
    // onto the bank two cells below is still a fall. Call before anything reads
    // on_ground this step. Not while rising: a box jumping off is leaving.
    void stick(const Grid& grid, const BoxRule& rule) {
        if (!riding || vel_y < 0 || grid.tipping_count() == 0) return;
        if (overlaps(grid, rule, x, y + 1)) return;  // still standing on something
        for (int d = 1; d <= RIDE_SNAP; ++d) {
            if (overlaps(grid, rule, x, y + d)) return;
            if (!overlaps(grid, rule, x, y + d + 1)) continue;
            if (footing(grid, rule, y + d).body < 0) return;
            y += d;
            on_ground = true;
            return;
        }
    }

    // The motion of what the box stands on, this step, into carry_*; and when
    // the box has just left a body it was riding, that body's motion kept as its
    // own, so stepping or jumping off a moving raft carries on with the raft's
    // speed rather than stopping dead in the air. Call after stick() and before
    // integrate(). A box standing on nothing that moves rides nothing, at no cost
    // but the footing read.
    void ride(const Grid& grid, const BoxRule& rule) {
        const bool was = riding;
        const fx::v was_x = carry_x;
        const fx::v was_y = carry_y;
        carry_x = 0;
        carry_y = 0;
        riding = false;
        stood = BoxFooting{};
        if (on_ground) {
            stood = footing(grid, rule, y);
            if (stood.body >= 0) {
                riding = true;
                carry_x = stood.vx;
                carry_y = stood.vy;
            }
        }
        if (was && !riding) {
            push_x += was_x;
            // Falling away faster than stick() follows: it falls after the body
            // at the body's speed, rather than starting its fall from rest a cell
            // above it. Not when it jumped, which set its own.
            if (vel_y >= 0 && was_y > vel_y) vel_y = was_y;
        }
    }

    // Whether the box at (px, py) overlaps body `body` (`inside`), and whether it
    // overlaps anything else that stops it (the return value): what shove()
    // asks of each place it might push the box to.
    bool blocked_but_for(const Grid& grid, const BoxRule& rule, int px, int py, int body,
                         bool& inside) const {
        inside = false;
        const int powder_top = py + rule.height - rule.powder_rows;
        for (int cy = py; cy < py + rule.height; ++cy) {
            for (int cx = px; cx < px + rule.width; ++cx) {
                const ElementType t = grid.get_element(cx, cy).type;
                if (!is_solid(t)) continue;
                if (material_of(t).move == MoveKind::Powder && cy < powder_top) continue;
                if (grid.body_at(cx, cy) == body) {
                    inside = true;
                    continue;
                }
                return true;
            }
        }
        return false;
    }

    // A loose body has moved into the box: grid writes cannot see the box, so a
    // raft rising under it, a slab sliding into it or a block landing on it
    // simply occupies cells the box is in. Push the box out along the way the
    // body was moving, a cell at a time, through nothing but the body itself --
    // never through a wall to reach open air -- and give it the body's speed that
    // way, so a body that strikes it throws it. Along the body's main direction
    // first, then its other one; for a body coming down onto it, squeezed out to
    // the nearer side. If none of that frees it, it is crushed, and the body's own
    // way out of terrain (the player's dig-out search, an enemy's lift) takes it.
    //
    // `own_vx` and the box's vel_y are what the box was doing; the strike's speed
    // is how much faster than that the body came. own_vx is the box's motion that
    // is not its legs' (push_x and carry_x): a body struck by something it walked
    // into is not struck by its own walk, which never carries it into a body's
    // cells anyway -- its moves are collision-checked; only the body's are not.
    // Call first, before anything that asks whether the box is stuck.
    BoxStrike shove(const Grid& grid, const BoxRule& rule, fx::v own_vx) {
        BoxStrike s;
        if (grid.tipping_count() == 0) return s;
        if (!grid.body_near(x, y, x + rule.width - 1, y + rule.height - 1)) return s;

        // The body with the most cells in the box, and its cell nearest the box's
        // middle: where it struck, and where the box pushes back.
        constexpr int MOST = 4;
        int ids[MOST] = {-1, -1, -1, -1};
        int count[MOST] = {};
        int near_x[MOST] = {}, near_y[MOST] = {}, near_d[MOST] = {};
        int sum_x2[MOST] = {};  // its cells' doubled centres, for which side it is on
        const int mid_x2 = 2 * x + rule.width, mid_y2 = 2 * y + rule.height;
        for (int cy = y; cy < y + rule.height; ++cy) {
            for (int cx = x; cx < x + rule.width; ++cx) {
                const int b = grid.body_at(cx, cy);
                if (b < 0) continue;
                int k = 0;
                while (k < MOST && ids[k] >= 0 && ids[k] != b) ++k;
                if (k == MOST) continue;
                const int d = std::abs(2 * cx + 1 - mid_x2) + std::abs(2 * cy + 1 - mid_y2);
                if (ids[k] < 0 || d < near_d[k]) {
                    near_x[k] = cx;
                    near_y[k] = cy;
                    near_d[k] = d;
                }
                ids[k] = b;
                ++count[k];
                sum_x2[k] += 2 * cx + 1;
            }
        }
        int best = -1;
        for (int k = 0; k < MOST && ids[k] >= 0; ++k)
            if (best < 0 || count[k] > count[best]) best = k;
        if (best < 0) return s;

        s.hit = true;
        s.body = ids[best];
        s.x = near_x[best];
        s.y = near_y[best];
        s.mass = grid.body_mass(s.body);
        const Grid::BodyMotion m = grid.body_velocity_at(s.body, s.x, s.y);

        // Which way to try, in order: the body's main direction of travel, its
        // other one, and -- for a body coming down -- out to either side,
        // nearer side first. Each a unit step and how far it may go.
        struct Way {
            int dx, dy, reach;
        };
        Way ways[4];
        int n = 0;
        const bool down_first = std::abs(m.vy) >= std::abs(m.vx);
        const int sx = m.vx > 0 ? 1 : (m.vx < 0 ? -1 : 0);
        const int sy = m.vy > 0 ? 1 : (m.vy < 0 ? -1 : 0);
        if (down_first && sy != 0) ways[n++] = {0, sy, MAX_SHOVE};
        if (sx != 0) ways[n++] = {sx, 0, MAX_SHOVE};
        if (!down_first && sy != 0) ways[n++] = {0, sy, MAX_SHOVE};
        if (sy > 0) {
            // Squeezed out from under: away from the side the body's cells inside
            // the box are mostly on, so the shorter way. As far as half the box
            // and the shove's reach, which is what a box pinned under the middle
            // of a slab needs to come out from under one edge of it.
            const int side = sum_x2[best] >= mid_x2 * count[best] ? -1 : 1;
            const int reach = rule.width / 2 + MAX_SHOVE;
            ways[n++] = {side, 0, reach};
            ways[n++] = {-side, 0, reach};
        }

        int moved_dx = 0, moved_dy = 0;
        for (int w = 0; w < n && !s.moved; ++w) {
            for (int k = 1; k <= ways[w].reach; ++k) {
                bool inside = false;
                const int px = x + ways[w].dx * k, py = y + ways[w].dy * k;
                if (blocked_but_for(grid, rule, px, py, s.body, inside)) break;
                if (inside) continue;
                moved_dx = ways[w].dx * k;
                moved_dy = ways[w].dy * k;
                s.moved = true;
                break;
            }
        }

        // Its own floor coming up under it -- the raft it stands on bobbing up a
        // cell, the plank it stands on rocking -- is standing, not being struck.
        // The box goes up with it and nothing else: what passes between a box
        // and the body it stands on is its weight, which the box presses with
        // every step it stands there (Player::press_bodies), and counting the
        // lift as a blow as well -- a throw up, a push back, a landing -- loads
        // the raft twice and sinks it.
        const bool own_floor = riding && stood.id != 0 && grid.body_id(s.body) == stood.id &&
                               moved_dy < 0 && moved_dx == 0;
        if (own_floor) {
            x += moved_dx;
            y += moved_dy;
            rem_y = 0;
            return s;
        }

        // How hard it hit: the body's speed against the box's along the body's
        // own main direction of travel -- not the way the box got out, which for
        // a slab landing on it and squeezing it out sideways is a way the slab
        // was not going at all. Only closing counts; a body the box was already
        // moving away from faster did not strike it.
        const fx::v closing = down_first ? sy * (m.vy - vel_y) : sx * (m.vx - own_vx);
        s.speed = std::max<fx::v>(0, closing);

        if (!s.moved) return s;
        x += moved_dx;
        y += moved_dy;
        // And given the body's speed the way it pushed the box, where that is
        // faster than the box's: a body that pushed it out carries on pushing, or
        // throws it. Horizontally into push_x, the box's own velocity that is not
        // its legs', which decays as a knock does once the body stops pushing.
        // The change is the box's momentum gained, which the body lost.
        if (moved_dx != 0) {
            const int ax = moved_dx > 0 ? 1 : -1;
            if (ax * (m.vx - own_vx) > 0) {
                s.dvx = m.vx - own_vx;
                push_x = m.vx;
            }
            rem_x = 0;
        } else {
            const int ay = moved_dy > 0 ? 1 : -1;
            if (ay * (m.vy - vel_y) > 0) {
                s.dvy = m.vy - vel_y;
                vel_y = m.vy;
            }
            rem_y = 0;
        }
        return s;
    }

    // --- liquids ---

    // How much of the box is under liquid, from three columns of it -- both
    // edges and the middle -- rather than every cell: a surface is level across
    // a box's width far more often than not, and three columns see a box half
    // in a waterfall as half in. Each cell counts for how full it is.
    Immersion immersion(const Grid& grid, const BoxRule& rule) const {
        Immersion im;
        int64_t amount = 0;
        const int cols[3] = {x, x + rule.width / 2, x + rule.width - 1};
        for (int cy = y; cy < y + rule.height; ++cy) {
            for (const int cx : cols) {
                const Element e = grid.get_element(cx, cy);
                if (!is_liquid(e.type)) continue;
                amount += std::min<int>(e.mass, Grid::LIQUID_FULL);
                im.weight = std::max<int>(im.weight, material_of(e.type).weight);
            }
        }
        if (amount > 0)
            im.share = static_cast<fx::v>(amount * fx::ONE /
                                          (int64_t{3} * rule.height * Grid::LIQUID_FULL));
        return im;
    }

    // Buoyancy and drag for one step, for a box of `density` (Material::weight
    // per cell of it) that falls at `gravity`, `im` deep. Archimedes as the loose
    // bodies have it: lifted by the weight of the liquid it displaces, so a box
    // all the way under is pulled down by gravity times one less the liquid's
    // weight over its own -- a box denser than water sinks slowly, a lighter one
    // rises -- and partly under, by the share that is. Then drag on what it is
    // doing (WADE_FORM_DRAG, WADE_DRAG) and its legs slowed
    // (WADE_SLOW). Call after the body has set vel_x and any jump or stroke, and
    // before integrate(), which adds the gravity this takes the lift off.
    void wade(const Immersion& im, fx::v gravity, int density) {
        if (im.share == 0 || density <= 0) return;
        // integrate()'s resting rule, first: a box standing on a pool's bottom has
        // the floor holding it, and the lift taken off a fall it is not making
        // would otherwise leave it with an upward speed for gravity to cancel,
        // and the difference piling up step after step under a body standing
        // still.
        if (on_ground && vel_y > 0) {
            vel_y = 0;
            rem_y = 0;
        }
        const int64_t lift =
            int64_t{fx::per_step(gravity)} * im.share / fx::ONE * im.weight / density;
        vel_y -= static_cast<fx::v>(lift);

        // v / (1 + k s |v|), v in cells a step: hard on a fast fall into a pool,
        // nothing much on a slow sink.
        const auto form = [&](fx::v v) {
            const int64_t u = std::abs(fx::per_step(v));
            const int64_t k = int64_t{fx::mul(WADE_FORM_DRAG, im.share)} * u / fx::ONE;
            return static_cast<fx::v>(int64_t{v} * fx::ONE / (fx::ONE + k));
        };
        const fx::v viscous = fx::mul(WADE_DRAG, im.share);
        vel_y = form(vel_y);
        vel_y -= fx::mul(vel_y, viscous);
        push_x = form(push_x);
        push_x -= fx::mul(push_x, viscous);
        vel_x -= fx::mul(vel_x, fx::mul(WADE_SLOW, im.share));
    }
};
