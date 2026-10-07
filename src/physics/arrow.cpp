#include "arrow.h"
#include "int_math.h"
#include <algorithm>
#include <cstdlib>

bool Quiver::update_bow(bool held, int from_x, int from_y, int aim_x, int aim_y) {
    if (draw > 0) --draw;
    if (!held || draw > 0) return false;

    const int dx = aim_x - from_x;
    const int dy = aim_y - from_y;
    if (dx == 0 && dy == 0) return false;  // aiming at your own chest

    // The launch velocity is the aim direction scaled to LAUNCH_SPEED, in integers.
    // The length is isqrt -- the dig tool's, for the dig tool's reason: a float
    // length is a float deciding where a shot goes.
    const long long len = int_math::isqrt(static_cast<long long>(dx) * dx +
                                          static_cast<long long>(dy) * dy);
    const long long speed = LAUNCH_SPEED;

    // The oldest arrow is recycled when the pool is full, and "oldest" is the stuck
    // one with the least life left. A flying arrow is never taken: it is the shot
    // the player is watching.
    Arrow* slot = nullptr;
    for (Arrow& a : pool) {
        if (!a.live) { slot = &a; break; }
        if (a.stuck && (!slot || a.life < slot->life)) slot = &a;
    }
    if (!slot) return false;

    Arrow a;
    a.live = true;
    a.x = a.prev_x = from_x;
    a.y = a.prev_y = from_y;
    a.vel_x = static_cast<fx::v>(int_math::div_round(speed * dx, len > 0 ? len : 1));
    a.vel_y = static_cast<fx::v>(int_math::div_round(speed * dy, len > 0 ? len : 1));
    *slot = a;
    draw = DRAW_STEPS;
    return true;
}

void Quiver::step_one(Arrow& a, Grid& grid, Enemy* enemies, int enemy_count, int& hit_pixels) {
    a.vel_y += fx::per_step(GRAVITY);
    if (a.vel_y > MAX_FALL_SPEED) a.vel_y = MAX_FALL_SPEED;

    const MoveKind medium = material_of(grid.get_element(a.x, a.y).type).move;
    if (medium == MoveKind::Liquid) {
        a.vel_x = a.vel_x / 100 * FLUID_DRAG_PERCENT;
        a.vel_y = a.vel_y / 100 * FLUID_DRAG_PERCENT;
    }

    a.rem_x += fx::per_step(a.vel_x);
    a.rem_y += fx::per_step(a.vel_y);
    const int dx = fx::trunc(a.rem_x);
    const int dy = fx::trunc(a.rem_y);
    a.rem_x = fx::frac(a.rem_x);
    a.rem_y = fx::frac(a.rem_y);

    // Every cell on the way, one at a time, the dig tool's march and for its
    // reason: an arrow that skipped cells would pass through an arm thinner than
    // its speed. The path is the rounded line from where the tip was to where it
    // is going, so it is the same line whichever way the arrow is travelling.
    const int span = std::max(std::abs(dx), std::abs(dy));
    const int x0 = a.x, y0 = a.y;
    for (int i = 1; i <= span; ++i) {
        const int cx = x0 + static_cast<int>(int_math::div_round(static_cast<long long>(dx) * i, span));
        const int cy = y0 + static_cast<int>(int_math::div_round(static_cast<long long>(dy) * i, span));

        // Bodies before the world, so a body standing in front of a wall is hit
        // rather than shot through. The gaps in a body are gaps -- between an arm
        // and the ribs, between the legs -- and an arrow passes through them.
        for (int e = 0; e < enemy_count; ++e) {
            if (enemies[e].pixel_at(cx, cy) < 0) continue;
            const int taken = enemies[e].shatter(grid, cx, cy, BITE_RADIUS);
            hit_pixels += taken;
            impacts[static_cast<size_t>(impacts_n++)] = ArrowImpact{cx, cy, e, taken};
            // Spent. An arrow that has just turned the thing it was stuck in into
            // sand has nothing left to be stuck in.
            a.live = false;
            return;
        }

        if (is_solid(grid.get_element(cx, cy).type)) {
            a.x = cx;
            a.y = cy;
            a.rem_x = 0;
            a.rem_y = 0;
            a.stuck = true;
            a.life = STUCK_STEPS;
            impacts[static_cast<size_t>(impacts_n++)] = ArrowImpact{cx, cy, -1, 0};
            return;
        }
        a.x = cx;
        a.y = cy;
    }
}

int Quiver::update_arrows(Grid& grid, Enemy* enemies, int enemy_count) {
    int hit_pixels = 0;
    impacts_n = 0;
    for (Arrow& a : pool) {
        if (!a.live) continue;
        a.prev_x = a.x;
        a.prev_y = a.y;
        a.prev_rem_x = a.rem_x;
        a.prev_rem_y = a.rem_y;

        if (a.stuck) {
            // The wall it was in has gone -- dug, burnt, or fallen away -- so it
            // drops. Velocity is kept while stuck only so the renderer knows which
            // way it points; a freed arrow starts from rest.
            if (!is_solid(grid.get_element(a.x, a.y).type)) {
                a.stuck = false;
                a.vel_x = 0;
                a.vel_y = 0;
            } else {
                if (--a.life <= 0) a.live = false;
                continue;
            }
        }
        step_one(a, grid, enemies, enemy_count, hit_pixels);
    }
    return hit_pixels;
}
