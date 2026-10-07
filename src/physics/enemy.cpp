#include "enemy.h"
#include <cstdlib>

void Enemy::spawn(int x, int y, const Species& k) {
    *this = Enemy{};
    kind = &k;
    alive = true;
    pos_x = prev_x = x;
    pos_y = prev_y = y;
    const body_art::Art& art = *kind->art;
    for (int fy = 0; fy < art.h; ++fy)
        for (int fx = 0; fx < art.w; ++fx)
            pixels[fy * art.w + fx] = art.is_body(fx, fy) ? 1 : 0;
    remaining = kind->pixel_count;
}

bool Enemy::overlaps_solid(const Grid& grid, int px, int py) const {
    // Powder only counts at the feet. A body that comes apart drops its own grains
    // inside its own box -- a chest hit, or an arm cut off at the shoulder -- and
    // counting a grain falling past the ribs as terrain made the body hop up out
    // of every hit it took. At the feet a pile is still ground: it is stood on,
    // climbed as a step and walked into as a wall, as for the player.
    const int footing_top = py + kind->height - kind->footing_rows();
    for (int cy = py; cy < py + kind->height; ++cy) {
        for (int cx = px; cx < px + kind->width; ++cx) {
            const ElementType t = grid.get_element(cx, cy).type;
            if (!is_solid(t)) continue;
            // A wading species sees no powder at all: it stands on what is under
            // the drift and pushes the drift aside -- see shove_powder.
            if (material_of(t).move == MoveKind::Powder && (kind->wades || cy < footing_top))
                continue;
            return true;
        }
    }
    return false;
}

int Enemy::world_x_of(int x) const {
    const int left = pos_x - kind->offset_x();
    return face_left ? left + (kind->frame_w() - 1 - x) : left + x;
}

int Enemy::world_y_of(int y) const {
    return pos_y - kind->offset_y() + y;
}

int Enemy::pixel_at(int wx, int wy) const {
    if (!alive) return -1;
    const int frame_w = kind->frame_w();
    const int fy = wy - (pos_y - kind->offset_y());
    if (fy < 0 || fy >= kind->frame_h()) return -1;
    int fx = wx - (pos_x - kind->offset_x());
    if (fx < 0 || fx >= frame_w) return -1;
    if (face_left) fx = frame_w - 1 - fx;
    const int index = fy * frame_w + fx;
    return pixels[index] ? index : -1;
}

bool Enemy::has_arms() const {
    const body_art::Art& art = *kind->art;
    for (int fy = art.arm_top; fy < art.h; ++fy)
        for (int fx = 0; fx < art.w; ++fx)
            if (pixels[fy * art.w + fx] && art.is_arm(fx, fy)) return true;
    return false;
}

bool Enemy::has_feet() const {
    const body_art::Art& art = *kind->art;
    for (int fy = art.h - art.foot_rows; fy < art.h; ++fy)
        for (int fx = 0; fx < art.w; ++fx)
            if (pixels[fy * art.w + fx] && art.is_foot(fx, fy)) return true;
    return false;
}

void Enemy::crumble(Grid& grid, int index) {
    if (!pixels[index]) return;
    pixels[index] = 0;
    --remaining;

    const int fx = index % kind->frame_w();
    const int fy = index / kind->frame_w();
    const int wx = world_x_of(fx);
    const int wy = world_y_of(fy);

    // Sand, in the pixel's own colour. Not a new material: "falls and piles like
    // sand" is exactly the Sand row's behaviour, and a second powder row identical
    // to it in everything but name would be a row whose only job is to be kept in
    // step with the first. The colour is what makes it read as the body, and
    // Grid::paint is the existing write path for a cell whose colour the caller
    // names. The grain keeps it as it falls, because swap_elements moves whole
    // Elements.
    const uint32_t color = kind->art->color_at(fx, fy);
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
    // the body has a compile-time size, so its fill does too. Sized for the
    // largest species and walked only as far as this one's frame.
    const body_art::Art& art = *kind->art;
    const int W = art.w, H = art.h;
    const int N = W * H;
    std::array<uint8_t, MAX_FRAME_PIXELS> seen{};
    std::array<int16_t, MAX_FRAME_PIXELS> queue{};
    int head = 0, tail = 0;
    for (int i = 0; i < N; ++i) {
        if (pixels[i] && art.is_heart(i % W, i / W)) {
            seen[i] = 1;
            queue[tail++] = static_cast<int16_t>(i);
        }
    }
    while (head < tail) {
        const int i = queue[head++];
        const int x = i % W, y = i / W;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                const int n = ny * W + nx;
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
        const int x = i % W, y = i / W;
        head_left = head_left || art.is_head(x, y);
        heart_left = heart_left || art.is_heart(x, y);
    }
    const bool mostly_gone = remaining * 100 < kind->pixel_count * kind->collapse_percent;
    if (!head_left || !heart_left || !has_feet() || mostly_gone) {
        for (int i = 0; i < N; ++i) crumble(grid, i);
        alive = false;
    }
}

int Enemy::shatter(Grid& grid, int wx, int wy, int radius) {
    if (!alive) return 0;
    const int before = remaining;
    const int r2 = radius * radius;
    const int frame_w = kind->frame_w();
    for (int fy = 0; fy < kind->frame_h(); ++fy) {
        for (int fx = 0; fx < frame_w; ++fx) {
            const int i = fy * frame_w + fx;
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
    for (int up = 1; up <= kind->max_step_height; ++up)
        if (!overlaps_solid(grid, pos_x + sign, pos_y - up)) return up;
    return -1;
}

bool Enemy::drop_ahead_is_deep(const Grid& grid, int sign) const {
    // The column just past the leading edge, from the feet down. One column, not
    // the box's width: the question is whether the next step lands on anything,
    // and the next step is one cell.
    const int x = sign > 0 ? pos_x + kind->width : pos_x - 1;
    for (int dy = 0; dy <= kind->ledge_drop; ++dy)
        if (is_solid(grid.get_element(x, pos_y + kind->height + dy).type)) return false;
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
        const int frame_w = kind->frame_w();
        for (int i = frame_w * kind->frame_h() - 1; i >= 0 && burned < kind->burn_pixels_per_tick; --i) {
            if (!pixels[i]) continue;
            const int wx = world_x_of(i % frame_w);
            const int wy = world_y_of(i / frame_w);
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
    //
    // A species that wades never collides with powder (overlaps_solid), so it is
    // only ever stuck in something static; the powder in its box is shoved out
    // of it here, every step, before anything else looks at the box.
    if (kind->wades) shove_powder(grid);
    if (overlaps_solid(grid, pos_x, pos_y)) {
        for (int up = 1; up <= kind->max_step_height; ++up) {
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
        const int dx = (target_x + Player::WIDTH / 2) - center_x();
        const int dy = (target_y + Player::HEIGHT / 2) - center_y();
        chasing = target_alive && std::abs(dx) <= kind->notice_x && std::abs(dy) <= kind->notice_y;

        // A slam plants the feet: nothing walks while it winds up or while it
        // stands spent after one. That stillness is the telegraph's other half --
        // the eyes flare, and the thing that was coming at you stops.
        const bool planted = windup > 0 || (kind->attack == Attack::Slam && attack_timer > 0);

        int want = 0;
        if (planted) {
            want = 0;
        } else if (chasing) {
            // Stops once the boxes are roughly over each other, rather than
            // oscillating across the target's centre one cell at a time.
            if (dx > kind->width / 2) want = 1;
            else if (dx < -kind->width / 2) want = -1;
        } else {
            want = face_left ? -1 : 1;
        }
        if (want != 0) face_left = want < 0;
        vel_x = want * (chasing ? kind->chase_speed : kind->patrol_speed);

        if (want != 0 && on_ground) {
            const int climb = climb_for(grid, want);
            if (climb < 0) {
                // A wall. A chase hops at it; a wander turns round.
                if (chasing) {
                    vel_y = -kind->jump_speed;
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

    return attack(grid, target_x, target_y, target_alive);
}

void Enemy::shove_powder(Grid& grid) {
    // Every grain in the box goes out of the side of the box it is nearer, along
    // its own row, to the first empty cell past the edge -- the legs pushing
    // through a drift. From there it is ordinary sand again and slumps into a
    // pile against the body's flank. Moved, never deleted: the grain keeps its
    // colour, so an arm shot off and waded through is still the arm's colour on
    // the ground.
    //
    // Row by row, not a search outward in every direction, so the result is a
    // function of the grid alone and the same on every machine. A row with no
    // room within a box's width either side keeps its grain, and the lift below
    // decides whether the body is still stuck.
    const int w = grid.get_width();
    const int mid = pos_x + kind->width / 2;
    for (int cy = pos_y; cy < pos_y + kind->height; ++cy) {
        for (int cx = pos_x; cx < pos_x + kind->width; ++cx) {
            const Element e = grid.get_element(cx, cy);
            if (material_of(e.type).move != MoveKind::Powder) continue;
            const int dir = cx < mid ? -1 : 1;
            int tx = dir < 0 ? pos_x - 1 : pos_x + kind->width;
            for (int d = 0; d < kind->width; ++d, tx += dir) {
                if (tx < 0 || tx >= w) break;
                const ElementType there = grid.get_element(tx, cy).type;
                if (there == ElementType::Empty) {
                    grid.set_element(cx, cy, ElementType::Empty);
                    grid.paint(tx, cy, e.type, e.color);
                    break;
                }
                // Past more of the pile, but never through a wall.
                if (material_of(there).move == MoveKind::Static) break;
            }
        }
    }
}

bool Enemy::target_in_reach(int target_x, int target_y) const {
    // The box and `reach` past it. For a swipe, both sides -- the claws hang
    // either side of the box. For a slam, the front only: the club comes down
    // where it is facing, and the space behind a troll is the safe place to be.
    const int reach = kind->reach;
    int left = pos_x - reach;
    int right = pos_x + kind->width + reach;  // one past
    if (kind->attack == Attack::Slam) {
        if (face_left) right = pos_x + kind->width;
        else left = pos_x;
    }
    return target_x < right && target_x + Player::WIDTH > left &&
           target_y < pos_y + kind->height && target_y + Player::HEIGHT > pos_y;
}

bool Enemy::attack(Grid& grid, int target_x, int target_y, bool target_alive) {
    if (attack_timer > 0) --attack_timer;

    if (kind->attack == Attack::Swipe) {
        // Lands the step the boxes touch, and only while it still has an arm to
        // swipe with.
        if (!target_alive || attack_timer > 0 || !has_arms()) return false;
        if (!target_in_reach(target_x, target_y)) return false;
        attack_timer = kind->attack_interval;
        return true;
    }

    // --- the slam ---
    if (windup > 0) {
        // Disarmed mid-swing -- both arms shot off while the club was raised --
        // and the blow never comes. The arms are what it slams with.
        if (!has_arms()) {
            windup = 0;
            return false;
        }
        if (--windup > 0) return false;
        slam(grid);
        attack_timer = kind->attack_interval;
        // Tested where the target is now, not where it was when the wind-up
        // began: that difference is the whole point of winding up.
        return target_alive && target_in_reach(target_x, target_y);
    }

    // Deciding to slam: standing, armed, rested, and the target is under the
    // club. It turns to face the target first, so a player who has slipped
    // behind it is slammed at rather than ignored -- which is still escapable,
    // because the turn costs the whole wind-up.
    if (!target_alive || attack_timer > 0 || !on_ground || !has_arms()) return false;
    const int dx = (target_x + Player::WIDTH / 2) - center_x();
    const bool was_left = face_left;
    if (dx != 0) face_left = dx < 0;
    if (target_in_reach(target_x, target_y)) {
        windup = kind->windup_steps;
    } else {
        face_left = was_left;
    }
    return false;
}

void Enemy::slam(Grid& grid) {
    // Where the club lands: as far out along its reach as keeps the whole crater
    // inside it, on the row the feet stand on top of. Far enough out, too, that
    // the debris thrown back toward the troll lands clear of its own feet -- see
    // below -- rather than in its footing rows, where it would lift the body.
    const int r = kind->crush_radius;
    const int out = kind->reach - r - 1;
    const int ix = face_left ? pos_x - 1 - out : pos_x + kind->width + out;
    const int iy = pos_y + kind->height;
    const int w = grid.get_width(), h = grid.get_height();

    // The ground in a disc around the impact breaks. Static structural cells
    // only -- wall, wood, charred wood -- because those are what a blow breaks;
    // powder is already loose and a fluid has nothing to break.
    //
    // Everything inside the disc is thrown: lifted out, and its grain put down in
    // the air beside the crater, flung past the rim on its own side (the middle
    // column alternating sides by row), higher the nearer the middle it was, so
    // it falls as a ring of debris either side of an open bowl. A ring one cell
    // wider is only loosened -- turned to sand where it lies -- so the bowl's
    // floor and lip are rubble that can slide, and a thin floor gives way.
    //
    // Two versions taught the shape. Thrown straight up, every grain fell back
    // into the hole it came from and the ground looked untouched. Centred on the
    // surface with only the disc's upper half thrown, the upper half was air, and
    // the crater was one row deep.
    //
    // Nothing is created or deleted: every grain that lands is a cell that
    // broke, which keeps the slam honest matter in a sand game. A grain whose
    // landing cell is taken is loosened in place instead. Grid::paint is the
    // write path, as it is for a body's grains, so the colour of the wall it broke
    // is the colour of the sand it became.
    const int rim = (r + 1) * (r + 1);
    for (int dy = -r - 1; dy <= r + 1; ++dy) {
        for (int dx = -r - 1; dx <= r + 1; ++dx) {
            const int d2 = dx * dx + dy * dy;
            if (d2 > rim) continue;
            const int cx = ix + dx, cy = iy + dy;
            if (cx < 0 || cx >= w || cy < 0 || cy >= h) continue;
            const Element e = grid.get_element(cx, cy);
            if (material_of(e.type).move != MoveKind::Static || !is_structural(e.type)) continue;

            if (d2 <= r * r) {
                const int adx = std::abs(dx);
                const int side = dx > 0 ? 1 : dx < 0 ? -1 : ((dy & 1) != 0 ? 1 : -1);
                const int tx = ix + side * (r + 2 + adx);
                const int ty = iy - 3 - (r - adx) - (dy + r);
                if (tx >= 0 && tx < w && ty >= 0 && ty < h &&
                    grid.get_element(tx, ty).type == ElementType::Empty) {
                    grid.set_element(cx, cy, ElementType::Empty);
                    grid.paint(tx, ty, ElementType::Sand, e.color);
                    continue;
                }
            }
            grid.paint(cx, cy, ElementType::Sand, e.color);
        }
    }
}
