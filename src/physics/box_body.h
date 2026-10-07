#pragma once
#include <cstdlib>
#include "fixed.h"
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

struct BoxBody {
    int x = 0;  // top-left cell of the box
    int y = 0;
    fx::v rem_x = 0;  // sub-cell motion not yet taken, fx cells
    fx::v rem_y = 0;
    fx::v vel_x = 0;  // cells/second
    fx::v vel_y = 0;
    bool on_ground = false;

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
                // the wall is removed.
                vel_x = 0;
                rem_x = 0;
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

        // The same rule on the other three sides, applied before the remainder is
        // accumulated rather than after it has carried the body into geometry.
        // Without these, a body held against a surface drifts up to a cell into it
        // before a whole-cell step is attempted -- phasing into walls. climb_for
        // rather than a bare overlap test, so walking up a step is still a move.
        if (vel_x != 0 && climb_for(grid, rule, vel_x > 0 ? 1 : -1) < 0) {
            vel_x = 0;
            rem_x = 0;
        }
        if (vel_y < 0 && overlaps(grid, rule, x, y - 1)) {
            vel_y = 0;
            rem_y = 0;
        }

        // Axes separately, horizontal first, so sliding along a surface works:
        // being blocked vertically must not also cancel the horizontal move.
        // fx::trunc truncates toward zero, which the remainder scheme needs in
        // both directions and is why it is not a shift.
        rem_x += fx::per_step(vel_x);
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
        return impact_speed;
    }
};
