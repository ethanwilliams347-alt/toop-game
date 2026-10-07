#include "enemy.h"
#include <cstdlib>

void Enemy::spawn(int x, int y) {
    *this = Enemy{};
    alive = true;
    pos_x = prev_x = x;
    pos_y = prev_y = y;
    for (int fy = 0; fy < FRAME_H; ++fy)
        for (int fx = 0; fx < FRAME_W; ++fx)
            pixels[fy * FRAME_W + fx] = enemy_art::is_body(fx, fy) ? 1 : 0;
    remaining = enemy_art::PIXEL_COUNT;
}

bool Enemy::overlaps_solid(const Grid& grid, int px, int py) const {
    // Powder only counts at the feet. A body that comes apart drops its own grains
    // inside its own box -- a chest hit, or an arm cut off at the shoulder -- and
    // counting a grain falling past the ribs as terrain made the body hop up out
    // of every hit it took. At the feet a pile is still ground: it is stood on,
    // climbed as a step and walked into as a wall, as for the player.
    const int footing_top = py + HEIGHT - FOOTING_ROWS;
    for (int cy = py; cy < py + HEIGHT; ++cy) {
        for (int cx = px; cx < px + WIDTH; ++cx) {
            const ElementType t = grid.get_element(cx, cy).type;
            if (!is_solid(t)) continue;
            if (cy < footing_top && material_of(t).move == MoveKind::Powder) continue;
            return true;
        }
    }
    return false;
}

int Enemy::world_x_of(int x) const {
    const int left = pos_x - OFFSET_X;
    return face_left ? left + (FRAME_W - 1 - x) : left + x;
}

int Enemy::world_y_of(int y) const {
    return pos_y - OFFSET_Y + y;
}

int Enemy::pixel_at(int wx, int wy) const {
    if (!alive) return -1;
    const int fy = wy - (pos_y - OFFSET_Y);
    if (fy < 0 || fy >= FRAME_H) return -1;
    int fx = wx - (pos_x - OFFSET_X);
    if (fx < 0 || fx >= FRAME_W) return -1;
    if (face_left) fx = FRAME_W - 1 - fx;
    const int index = fy * FRAME_W + fx;
    return pixels[index] ? index : -1;
}

bool Enemy::has_arms() const {
    for (int fy = 0; fy < FRAME_H; ++fy)
        for (int fx = 0; fx < FRAME_W; ++fx)
            if (pixels[fy * FRAME_W + fx] && enemy_art::is_arm(fx, fy)) return true;
    return false;
}

bool Enemy::has_feet() const {
    for (int fy = FRAME_H - enemy_art::FOOT_ROWS; fy < FRAME_H; ++fy)
        for (int fx = 0; fx < FRAME_W; ++fx)
            if (pixels[fy * FRAME_W + fx] && enemy_art::is_foot(fx, fy)) return true;
    return false;
}

void Enemy::crumble(Grid& grid, int index) {
    if (!pixels[index]) return;
    pixels[index] = 0;
    --remaining;

    const int fx = index % FRAME_W;
    const int fy = index / FRAME_W;
    const int wx = world_x_of(fx);
    const int wy = world_y_of(fy);

    // Sand, in the pixel's own colour. Not a new material: "falls and piles like
    // sand" is exactly the Sand row's behaviour, and a second powder row identical
    // to it in everything but name would be a row whose only job is to be kept in
    // step with the first. The colour is what makes it read as the body, and
    // Grid::paint is the existing write path for a cell whose colour the caller
    // names. The grain keeps it as it falls, because swap_elements moves whole
    // Elements.
    const uint32_t color = enemy_art::color_at(fx, fy);
    const ElementType there = grid.get_element(wx, wy).type;
    if (there == ElementType::Empty) {
        grid.paint(wx, wy, ElementType::Sand, color);
    } else if (!is_solid(there)) {
        // A fluid or a gas. displace lifts it out of the way the way the brush does,
        // so an arm cut off over a pond pushes the water up instead of deleting a
        // cell of it; the paint after it only recolours the grain displace wrote.
        grid.displace(wx, wy, ElementType::Sand);
        grid.paint(wx, wy, ElementType::Sand, color);
    }
    // Solid: a foot inside the step it is climbing. The pixel goes and leaves no
    // grain, for the reason at the declaration.
}

void Enemy::settle_after_loss(Grid& grid) {
    if (!alive) return;

    // --- what is still attached ---
    //
    // A flood fill from every surviving heart pixel through surviving pixels,
    // 8-connected so the art's diagonal joins (a claw, a heel) hold. Anything not
    // reached has been cut off and comes down. Fixed arrays on the stack, which is
    // the step loop's no-allocation rule kept without a persistent scratch buffer:
    // the body has a compile-time size, so its fill does too.
    constexpr int N = FRAME_W * FRAME_H;
    std::array<uint8_t, N> seen{};
    std::array<int16_t, N> queue{};
    int head = 0, tail = 0;
    for (int i = 0; i < N; ++i) {
        if (pixels[i] && enemy_art::is_heart(i % FRAME_W, i / FRAME_W)) {
            seen[i] = 1;
            queue[tail++] = static_cast<int16_t>(i);
        }
    }
    while (head < tail) {
        const int i = queue[head++];
        const int x = i % FRAME_W, y = i / FRAME_W;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= FRAME_W || ny < 0 || ny >= FRAME_H) continue;
                const int n = ny * FRAME_W + nx;
                if (seen[n] || !pixels[n]) continue;
                seen[n] = 1;
                queue[tail++] = static_cast<int16_t>(n);
            }
        }
    }
    for (int i = 0; i < N; ++i)
        if (pixels[i] && !seen[i]) crumble(grid, i);

    // --- is it still alive ---
    bool head_left = false, heart_left = false;
    for (int i = 0; i < N; ++i) {
        if (!pixels[i]) continue;
        const int x = i % FRAME_W, y = i / FRAME_W;
        head_left = head_left || enemy_art::is_head(x, y);
        heart_left = heart_left || enemy_art::is_heart(x, y);
    }
    const bool mostly_gone = remaining * 100 < enemy_art::PIXEL_COUNT * COLLAPSE_PERCENT;
    if (!head_left || !heart_left || !has_feet() || mostly_gone) {
        for (int i = 0; i < N; ++i) crumble(grid, i);
        alive = false;
    }
}

int Enemy::shatter(Grid& grid, int wx, int wy, int radius) {
    if (!alive) return 0;
    const int before = remaining;
    const int r2 = radius * radius;
    for (int fy = 0; fy < FRAME_H; ++fy) {
        for (int fx = 0; fx < FRAME_W; ++fx) {
            const int i = fy * FRAME_W + fx;
            if (!pixels[i]) continue;
            const int dx = world_x_of(fx) - wx;
            const int dy = world_y_of(fy) - wy;
            if (dx * dx + dy * dy <= r2) crumble(grid, i);
        }
    }
    settle_after_loss(grid);
    return before - remaining;
}

int Enemy::climb_for(const Grid& grid, int sign) const {
    if (!overlaps_solid(grid, pos_x + sign, pos_y)) return 0;
    if (!on_ground) return -1;
    for (int up = 1; up <= MAX_STEP_HEIGHT; ++up)
        if (!overlaps_solid(grid, pos_x + sign, pos_y - up)) return up;
    return -1;
}

bool Enemy::drop_ahead_is_deep(const Grid& grid, int sign) const {
    // The column just past the leading edge, from the feet down. One column, not
    // the box's width: the question is whether the next step lands on anything,
    // and the next step is one cell.
    const int x = sign > 0 ? pos_x + WIDTH : pos_x - 1;
    for (int dy = 0; dy <= LEDGE_DROP; ++dy)
        if (is_solid(grid.get_element(x, pos_y + HEIGHT + dy).type)) return false;
    return true;
}

void Enemy::move_x(const Grid& grid, int amount) {
    const int sign = amount > 0 ? 1 : -1;
    for (int i = 0; i < std::abs(amount); ++i) {
        const int climbed = climb_for(grid, sign);
        if (climbed < 0) {
            vel_x = 0;
            rem_x = 0;
            return;
        }
        pos_x += sign;
        pos_y -= climbed;
    }
}

void Enemy::move_y(const Grid& grid, int amount) {
    const int sign = amount > 0 ? 1 : -1;
    for (int i = 0; i < std::abs(amount); ++i) {
        if (overlaps_solid(grid, pos_x, pos_y + sign)) {
            vel_y = 0;
            rem_y = 0;
            return;
        }
        pos_y += sign;
    }
}

bool Enemy::update(Grid& grid, int target_x, int target_y, bool target_alive) {
    if (!alive) return false;

    prev_x = pos_x;
    prev_y = pos_y;
    prev_rem_x = rem_x;
    prev_rem_y = rem_y;

    // --- heat ---
    //
    // Pixel by pixel against the cell each one covers, so what burns is the part in
    // the fire. The grain a burnt pixel leaves is scorched to half its colour --
    // ash, still recognisably the body's -- and takes the spot's heat with it,
    // because place() keeps the temperature of the cell it writes into.
    //
    // Checked every step until something burns, then every BURN_INTERVAL_STEPS
    // while it keeps burning: the player's rule, so walking out of the fire and
    // back in costs a tick at once rather than resuming a hidden countdown.
    if (burn_timer > 0) {
        --burn_timer;
    } else {
        // Bottom row first, so what burns is what is deepest in the fire -- a flame
        // is a gas and sits on the floor, so in practice the feet.
        int burned = 0;
        for (int i = FRAME_W * FRAME_H - 1; i >= 0 && burned < BURN_PIXELS_PER_TICK; --i) {
            if (!pixels[i]) continue;
            const int wx = world_x_of(i % FRAME_W);
            const int wy = world_y_of(i / FRAME_W);
            if (grid.get_element(wx, wy).temperature < BURN_TEMPERATURE) continue;
            crumble(grid, i);
            // Darkened after the fact, on the grain crumble just wrote. Only if it is
            // the grain crumble wrote -- a solid cell got no grain and must not be
            // recoloured.
            const Element e = grid.get_element(wx, wy);
            if (e.type == ElementType::Sand) {
                const uint32_t c = e.color;
                const uint32_t scorched = 0xFF000000u | ((c >> 1) & 0x007F7F7Fu);
                grid.paint(wx, wy, ElementType::Sand, scorched);
            }
            ++burned;
        }
        if (burned > 0) {
            burn_timer = BURN_INTERVAL_STEPS;
            settle_after_loss(grid);
            if (!alive) return false;
        }
    }

    // --- stuck inside something ---
    //
    // Most often its own dust: a hit to the chest drops grains inside the box,
    // and they land around the feet. Lifting by at most a climbable step is what a
    // body standing in a pile of sand would do. Anything worse than that -- buried
    // by the brush -- leaves it where it is until it is dug out, rather than
    // teleporting it the way the player's unstuck search does: an enemy appearing
    // ten cells away from where it was buried reads as a bug, not as an escape.
    if (overlaps_solid(grid, pos_x, pos_y)) {
        for (int up = 1; up <= MAX_STEP_HEIGHT; ++up) {
            if (!overlaps_solid(grid, pos_x, pos_y - up)) {
                pos_y -= up;
                break;
            }
        }
        vel_x = 0;
        vel_y = 0;
        rem_x = 0;
        rem_y = 0;
        on_ground = overlaps_solid(grid, pos_x, pos_y + 1);
    } else {
        // --- deciding where to go ---
        //
        // Box centre to box centre, per axis, in whole cells.
        const int dx = (target_x + WIDTH / 2) - center_x();
        const int dy = (target_y + HEIGHT / 2) - center_y();
        chasing = target_alive && std::abs(dx) <= NOTICE_X && std::abs(dy) <= NOTICE_Y;

        int want = 0;
        if (chasing) {
            // Stops once the boxes are roughly over each other, rather than
            // oscillating across the target's centre one cell at a time.
            if (dx > WIDTH / 2) want = 1;
            else if (dx < -WIDTH / 2) want = -1;
        } else {
            want = face_left ? -1 : 1;
        }
        if (want != 0) face_left = want < 0;
        vel_x = want * (chasing ? CHASE_SPEED : PATROL_SPEED);

        if (want != 0 && on_ground) {
            const int climb = climb_for(grid, want);
            if (climb < 0) {
                // A wall. A chase hops at it; a wander turns round.
                if (chasing) {
                    vel_y = -JUMP_SPEED;
                } else {
                    face_left = !face_left;
                    vel_x = 0;
                }
            } else if (!chasing && drop_ahead_is_deep(grid, want)) {
                face_left = !face_left;
                vel_x = 0;
            }
        }

        // From here on it is Player::update's motion, minus the wings: the same
        // order and the same reasons, which player.cpp gives at length.
        if (on_ground && vel_y > 0) {
            vel_y = 0;
            rem_y = 0;
        }
        vel_y += fx::per_step(GRAVITY);
        if (vel_y > MAX_FALL_SPEED) vel_y = MAX_FALL_SPEED;

        if (vel_x != 0 && climb_for(grid, vel_x > 0 ? 1 : -1) < 0) {
            vel_x = 0;
            rem_x = 0;
        }
        if (vel_y < 0 && overlaps_solid(grid, pos_x, pos_y - 1)) {
            vel_y = 0;
            rem_y = 0;
        }

        rem_x += fx::per_step(vel_x);
        const int step_x = fx::trunc(rem_x);
        rem_x = fx::frac(rem_x);
        if (step_x != 0) move_x(grid, step_x);

        rem_y += fx::per_step(vel_y);
        const int step_y = fx::trunc(rem_y);
        rem_y = fx::frac(rem_y);
        if (step_y != 0) move_y(grid, step_y);

        on_ground = overlaps_solid(grid, pos_x, pos_y + 1);
    }

    // --- the swipe ---
    //
    // Reach is the arms' overhang either side of the box, so a swipe lands where
    // the drawn claws are rather than where the collision box is.
    if (swipe_timer > 0) --swipe_timer;
    if (target_alive && swipe_timer == 0 && has_arms()) {
        const int reach = OFFSET_X;
        const bool touching = target_x < pos_x + WIDTH + reach &&
                              target_x + Player::WIDTH > pos_x - reach &&
                              target_y < pos_y + HEIGHT &&
                              target_y + Player::HEIGHT > pos_y;
        if (touching) {
            swipe_timer = SWIPE_INTERVAL_STEPS;
            return true;
        }
    }
    return false;
}
