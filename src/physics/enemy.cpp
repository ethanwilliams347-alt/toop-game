#include "enemy.h"
#include <algorithm>
#include <cstdlib>

namespace {

// a + (b - a) * k, k in fx [0, ONE].
fx::v lerp(fx::v a, fx::v b, fx::v k) { return a + fx::mul(b - a, k); }

// t of T as fx, clamped to [0, ONE].
fx::v fraction(int t, int T) {
    if (t <= 0 || T <= 0) return 0;
    if (t >= T) return fx::ONE;
    return fx::from_ratio(t, T);
}

// Smoothstep of t of T: slow out, slow in. What makes a club being raised read as
// weight rather than as a dial turning.
fx::v ease(int t, int T) {
    const fx::v x = fraction(t, T);
    return fx::mul(fx::mul(x, x), 3 * fx::ONE - 2 * x);
}

// Where the stroke ends, as an angle the stroke reaches by turning clockwise
// from the raise -- which, facing right, is up over the top and down the front.
// The same place as Rig::strike, a whole turn on if need be. Without it a troll
// whose club is raised back over its shoulder (+150) and lands a little forward
// of straight down (-14) swings the club back the way it came, down its own back.
fx::v stroke_end(const rig::Rig& r) {
    fx::v end = r.strike;
    while (end < r.raise) end += 4 * fx::HALF_PI;
    return end;
}

// --- snapped poses -----------------------------------------------------------
//
// The alternative to the continuous turn, behind Enemy::set_snapped_poses. A limb
// turned a degree or two re-rounds its edge one cell at a time, starting at one
// end, so a slow turn reads on screen as a ripple running down the limb rather
// than as the limb moving. Holding the slow clocks at a few key points instead
// makes the pose a handful of frames that are each held still and then replaced
// whole -- a pixel-art walk cycle -- with the same pixels and the same mask.
//
// Only the slow motions are held. The strike is six or eight steps and moves
// many cells a step, which never crawls; snapping it would only take frames out
// of the blow.

// Key frames per stride: legs together, apart, together, apart the other way,
// and one between each.
constexpr int WALK_KEYS = 8;
// Per breath: in, level, out, level.
constexpr int BREATH_KEYS = 4;
// For each eased transition: the club's raise and the recovery from a blow.
constexpr int EASE_KEYS = 4;
// For the flinch easing back.
constexpr int FLINCH_KEYS = 3;

// k in fx [0, ONE] rounded to the nearest of `keys` equal steps, so 0 and ONE
// are both kept exactly and a transition still starts and ends where it did.
fx::v hold_fraction(fx::v k, int keys) {
    const int n = (k * keys + fx::ONE / 2) / fx::ONE;
    return fx::from_ratio(n, keys);
}

}  // namespace

void Enemy::spawn(int x, int y, const Species& k) {
    *this = Enemy{};
    kind = &k;
    alive = true;
    body.x = prev_x = x;
    body.y = prev_y = y;
    const body_art::Art& art = *kind->art;
    for (int fy = 0; fy < art.h; ++fy)
        for (int fx = 0; fx < art.w; ++fx)
            pixels[fy * art.w + fx] =
                art.is_body(fx, fy) ? static_cast<uint8_t>(1 + rig::part_of(art, kind->rig, fx, fy))
                                    : 0;
    remaining = kind->pixel_count;
    // Every animation clock is at zero, so this is the rest pose: every pixel
    // exactly where the art has it.
    compute_pose();
}

bool Enemy::overlaps_solid(const Grid& grid, int px, int py) const {
    // Powder only counts at the feet (BoxRule::powder_rows). A body that comes
    // apart drops its own grains inside its own box -- a chest hit, or an arm cut
    // off at the shoulder -- and counting a grain falling past the ribs as terrain
    // made the body hop up out of every hit it took. At the feet a pile is still
    // ground: it is stood on, climbed as a step and walked into as a wall, as for
    // the player. A wading species sees no powder at all: it stands on what is
    // under the drift and pushes the drift aside -- see shove_powder.
    return body.overlaps(grid, rule(), px, py);
}

int Enemy::world_column(int frame_x) const {
    const int left = body.x - kind->offset_x();
    return face_left ? left + (kind->frame_w() - 1 - frame_x) : left + frame_x;
}

void Enemy::world_of(int x, int y, int& wx, int& wy) const {
    const body_art::Art& art = *kind->art;
    const int index = y * art.w + x;
    const rig::Part part = pixels[index] ? static_cast<rig::Part>(pixels[index] - 1)
                                         : rig::part_of(art, kind->rig, x, y);
    const rig::Point p = pose.forward(part, x, y);
    wx = world_column(rig::nearest(p.x));
    wy = body.y - kind->offset_y() + rig::nearest(p.y);
}

int Enemy::posed_pixel(int px, int py) const {
    if (px < bounds.x0 || px >= bounds.x1 || py < bounds.y0 || py >= bounds.y1) return -1;
    const body_art::Art& art = *kind->art;
    const rig::Rig& r = kind->rig;
    // Front to back: the first part with a surviving pixel in this cell is the
    // one in front, and the one that is hit.
    for (int i = rig::PART_COUNT - 1; i >= 0; --i) {
        const rig::Part part = static_cast<rig::Part>(i);
        const rig::Box& b = part_bounds[i];
        if (px < b.x0 || px >= b.x1 || py < b.y0 || py >= b.y1) continue;
        const rig::Point rest = pose.backward(part, px, py);
        const int x = rig::nearest(rest.x), y = rig::nearest(rest.y);
        const rig::Box home = rig::rest_box(art, r, part);
        if (x < home.x0 || x >= home.x1 || y < home.y0 || y >= home.y1) continue;
        const int index = y * art.w + x;
        if (pixels[index] == 1 + part) return index;
    }
    return -1;
}

int Enemy::pixel_at(int wx, int wy) const {
    if (!alive) return -1;
    const int py = wy - (body.y - kind->offset_y());
    int px = wx - (body.x - kind->offset_x());
    if (face_left) px = kind->frame_w() - 1 - px;
    return posed_pixel(px, py);
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
    int wx = 0, wy = 0;
    world_of(fx, fy, wx, wy);

    // Sand, in the pixel's own colour. Not a new material: "falls and piles like
    // sand" is exactly the Sand row's behaviour, and a second powder row identical
    // to it in everything but name would be a row whose only job is to be kept in
    // step with the first. The colour is what makes it read as the body, and
    // Grid::paint is the existing write path for a cell whose colour the caller
    // names. The grain keeps it as it falls, because swap_elements moves whole
    // Elements.
    const uint32_t color = kind->art->color_at(fx, fy);
    ElementType there = grid.get_element(wx, wy).type;

    // Loose powder already there -- most often a grain this same body dropped a
    // moment ago, because a turned limb can put two pixels within a cell of the
    // same spot (rig.h), and a whole body coming down at once is a solid block
    // of its own grains. The grain goes on top of the powder instead, as a grain
    // dropped onto a pile would, so a body still turns into exactly as many
    // grains as it had pixels. Straight up the column, so the result is the same
    // on every machine; through powder only, never through a wall; and as far as
    // twice the frame's height, because the column a whole body leaves is the
    // body's own height of grains with whatever it was standing in under them.
    for (int climbed = 0;
         material_of(there).move == MoveKind::Powder && climbed < 2 * kind->frame_h(); ++climbed) {
        --wy;
        there = grid.get_element(wx, wy).type;
    }

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
            int px = 0, py = 0;
            world_of(fx, fy, px, py);
            const int dx = px - wx;
            const int dy = py - wy;
            if (dx * dx + dy * dy <= r2) crumble(grid, i);
        }
    }
    settle_after_loss(grid);
    // Rocks back from the hit, from the next step: the pose is not recomputed
    // here, so the rest of this step's arrows find the body where they aimed.
    if (alive && remaining < before) flinch = kind->rig.flinch_steps;
    return before - remaining;
}

bool Enemy::drop_ahead_is_deep(const Grid& grid, int sign) const {
    // The column just past the leading edge, from the feet down. One column, not
    // the box's width: the question is whether the next step lands on anything,
    // and the next step is one cell.
    const int x = sign > 0 ? body.x + kind->width : body.x - 1;
    for (int dy = 0; dy <= kind->ledge_drop; ++dy)
        if (is_solid(grid.get_element(x, body.y + kind->height + dy).type)) return false;
    return true;
}

bool Enemy::update(Grid& grid, int target_x, int target_y, bool target_alive) {
    if (!alive) return false;

    prev_x = body.x;
    prev_y = body.y;
    prev_rem_x = body.rem_x;
    prev_rem_y = body.rem_y;
    if (flinch > 0) --flinch;
    breath = (breath + 1) % kind->rig.breathe_steps;

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
        for (int i = frame_w * kind->frame_h() - 1; i >= 0 && burned < kind->burn_pixels_per_tick;
             --i) {
            if (!pixels[i]) continue;
            int wx = 0, wy = 0;
            world_of(i % frame_w, i / frame_w, wx, wy);
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
    if (overlaps_solid(grid, body.x, body.y)) {
        for (int up = 1; up <= kind->max_step_height; ++up) {
            if (!overlaps_solid(grid, body.x, body.y - up)) {
                body.y -= up;
                break;
            }
        }
        body.vel_x = 0;
        body.vel_y = 0;
        body.rem_x = 0;
        body.rem_y = 0;
        body.on_ground = overlaps_solid(grid, body.x, body.y + 1);
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
            if (dx > kind->width / 2)
                want = 1;
            else if (dx < -kind->width / 2)
                want = -1;
        } else {
            want = face_left ? -1 : 1;
        }
        if (want != 0) face_left = want < 0;
        body.vel_x = want * (chasing ? kind->chase_speed : kind->patrol_speed);

        if (want != 0 && body.on_ground) {
            const int climb = body.climb_for(grid, rule(), want);
            if (climb < 0) {
                // A wall. A chase hops at it; a wander turns round.
                if (chasing) {
                    body.vel_y = -kind->jump_speed;
                } else {
                    face_left = !face_left;
                    body.vel_x = 0;
                }
            } else if (!chasing && drop_ahead_is_deep(grid, want)) {
                face_left = !face_left;
                body.vel_x = 0;
            }
        }

        // The box's motion: the player's, from box_body.h, with this species'
        // rule.
        body.integrate(grid, rule(), GRAVITY, MAX_FALL_SPEED);
    }
    advance_gait();

    const bool landed = attack(grid, target_x, target_y, target_alive);
    compute_pose();
    return landed;
}

void Enemy::advance_gait() {
    // By the distance actually covered along the ground this step, so the feet
    // keep pace with the ground under them: blocked by a wall, it stands; slowed
    // by a climb, it steps slower. Off the ground the gait holds where it was and
    // compute_pose gives the legs their airborne spread instead.
    const fx::v cycle = fx::from_int(kind->rig.stride);
    const fx::v moved = std::abs(fx::from_int(body.x - prev_x) + body.rem_x - prev_rem_x);
    if (moved > 0 && body.on_ground) {
        gait = (gait + moved) % cycle;
        return;
    }
    // Standing: the legs come back together by finishing the step they were in
    // -- the gait runs on to the nearer of the two points in the cycle where the
    // legs pass each other -- rather than snapping back to straight.
    const fx::v half = cycle / 2;
    const fx::v settle = cycle / 16;
    const fx::v target = gait < half / 2 ? 0 : gait < half + half / 2 ? half : cycle;
    if (gait < target)
        gait = std::min(gait + settle, target);
    else
        gait = std::max(gait - settle, target);
    if (gait >= cycle) gait = 0;
}

void Enemy::compute_pose() {
    const rig::Rig& r = kind->rig;
    const body_art::Art& art = *kind->art;
    constexpr fx::v TURN = 4 * fx::HALF_PI;

    // Snapped, the clocks the pose reads are held at their key points; the clocks
    // themselves run on untouched, so the switch changes where pixels are and
    // nothing about where the body is or what it does.
    fx::v walk = gait;
    int breathing = breath;
    if (snap) {
        const fx::v slice = fx::from_int(r.stride) / WALK_KEYS;
        walk -= walk % slice;
        const int beat = std::max(1, r.breathe_steps / BREATH_KEYS);
        breathing -= breathing % beat;
    }

    // --- the walk --- one sine drives both legs, opposite ways, and the arms
    // against their own side's leg.
    const fx::v stride_sin =
        fx::sincos(static_cast<fx::v>(int64_t{walk} * TURN / fx::from_int(r.stride))).s;
    fx::v front_leg = -fx::mul(r.leg_swing, stride_sin);
    fx::v rear_leg = fx::mul(r.leg_swing, stride_sin);
    fx::v front_arm = fx::mul(r.arm_swing, stride_sin);
    fx::v rear_arm = -fx::mul(r.arm_swing, stride_sin);
    // Lowest with the legs furthest apart, as a walking body is. Whole cells,
    // because a body half a cell down rounds half its columns one way and half
    // the other and tears along the rounding.
    const int sink = rig::nearest(fx::mul(fx::from_int(r.bob), fx::mul(stride_sin, stride_sin)));
    fx::v lean = chasing ? r.chase_lean : 0;

    // --- the breath --- always running, so a body standing still is not a
    // picture of one.
    const fx::v breath_sin =
        fx::sincos(static_cast<fx::v>(int64_t{breathing} * TURN / r.breathe_steps)).s;
    const fx::v sway = fx::mul(r.breathe, breath_sin);
    lean += sway / 2;
    front_arm += sway;
    rear_arm += sway;

    if (chasing && r.chase_arms != 0) {
        // Reaching: both arms out in front, the rear one a little lower so the two
        // read as two, still pumping a little with the stride.
        front_arm = r.chase_arms + fx::mul(r.arm_swing, stride_sin) / 3 + sway;
        rear_arm = r.chase_arms + rig::deg(12) - fx::mul(r.arm_swing, stride_sin) / 3 + sway;
    }

    // Moving, not merely unsupported: a body just spawned in the air has not
    // started to fall yet and is shown at rest.
    if (!body.on_ground && body.vel_y != 0) {
        // In the air: the legs spread, front one reaching for the landing, and
        // the arms come up a little.
        front_leg = -r.leg_swing;
        rear_leg = r.leg_swing * 2 / 3;
        front_arm -= r.leg_swing;
        rear_arm -= r.leg_swing;
    }

    // --- the attack --- overrides the front arm and the lean; the rear arm
    // counterbalances a slam.
    const int interval = kind->attack_interval;
    if (kind->attack == Attack::Slam && windup > 0) {
        const int t = kind->windup_steps - windup;
        const int raising = kind->windup_steps - r.strike_steps;
        if (t < raising) {
            fx::v k = ease(t, raising);
            if (snap) k = hold_fraction(k, EASE_KEYS);
            front_arm = lerp(front_arm, r.raise, k);
            rear_arm = lerp(rear_arm, -r.raise / 6, k);
            lean = lerp(lean, r.windup_lean, k);
        } else {
            // Linear, not eased: the blow is all speed.
            const fx::v k = fraction(t - raising + 1, r.strike_steps);
            front_arm = lerp(r.raise, stroke_end(r), k);
            rear_arm = -r.raise / 6;
            lean = lerp(r.windup_lean, r.strike_lean, k);
        }
    } else if (attack_timer > 0) {
        const int since = interval - attack_timer;
        if (kind->attack == Attack::Swipe && since < r.strike_steps) {
            const fx::v k = fraction(since + 1, r.strike_steps);
            front_arm = lerp(r.raise, stroke_end(r), k);
            lean = lerp(lean, r.strike_lean, k);
        } else {
            // Recovering. A slam's club stays down in its crater for half the
            // recovery -- the window to punish it in, shown -- then both come
            // back to where the walk would have them.
            const int hold = kind->attack == Attack::Slam ? interval / 2 : r.strike_steps;
            fx::v k = ease(since - hold, interval - hold);
            if (snap) k = hold_fraction(k, EASE_KEYS);
            if (kind->attack == Attack::Slam) rear_arm = lerp(-r.raise / 6, rear_arm, k);
            front_arm = lerp(r.strike, front_arm, k);
            lean = lerp(r.strike_lean, lean, k);
        }
    }

    // --- the flinch --- rocked back, arms left behind going forward.
    if (flinch > 0) {
        const fx::v rock =
            snap ? fx::mul(r.flinch, hold_fraction(fraction(flinch, r.flinch_steps), FLINCH_KEYS))
                 : r.flinch * flinch / r.flinch_steps;
        lean += rock;
        front_arm += rock;
        rear_arm += rock;
    }

    pose.part[rig::Body] = rig::turn(lean, 0, fx::from_int(sink));
    pose.part[rig::Head] = rig::turn(sway - lean / 2);
    pose.part[rig::RearArm] = rig::turn(rear_arm);
    pose.part[rig::FrontArm] = rig::turn(front_arm);
    pose.part[rig::RearLeg] = rig::turn(rear_leg);
    pose.part[rig::FrontLeg] = rig::turn(front_leg);
    pose.flatten(r);

    // --- where each part can be --- its rest rectangle's corners put through
    // the pose, plus a cell of slack for rounding. Turning a rectangle's corners
    // bounds everything inside it, so this is a bound, not a guess.
    const rig::Box limit{-r.pad, -r.pad, art.w + r.pad, art.h + r.pad};
    bounds = {limit.x1, limit.y1, limit.x0, limit.y0};
    for (int i = 0; i < rig::PART_COUNT; ++i) {
        const rig::Part part = static_cast<rig::Part>(i);
        const rig::Box home = rig::rest_box(art, r, part);
        rig::Box b{limit.x1, limit.y1, limit.x0, limit.y0};
        if (home.x0 < home.x1 && home.y0 < home.y1) {
            const int xs[2] = {home.x0, home.x1 - 1};
            const int ys[2] = {home.y0, home.y1 - 1};
            for (int cx : xs) {
                for (int cy : ys) {
                    const rig::Point p = pose.forward(part, cx, cy);
                    const int x = rig::nearest(p.x), y = rig::nearest(p.y);
                    b.x0 = std::min(b.x0, x - 1);
                    b.y0 = std::min(b.y0, y - 1);
                    b.x1 = std::max(b.x1, x + 2);
                    b.y1 = std::max(b.y1, y + 2);
                }
            }
            b.x0 = std::max(b.x0, limit.x0);
            b.y0 = std::max(b.y0, limit.y0);
            b.x1 = std::min(b.x1, limit.x1);
            b.y1 = std::min(b.y1, limit.y1);
        }
        part_bounds[i] = b;
        if (b.x0 < b.x1 && b.y0 < b.y1) {
            bounds.x0 = std::min(bounds.x0, b.x0);
            bounds.y0 = std::min(bounds.y0, b.y0);
            bounds.x1 = std::max(bounds.x1, b.x1);
            bounds.y1 = std::max(bounds.y1, b.y1);
        }
    }
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
    const int mid = body.x + kind->width / 2;
    for (int cy = body.y; cy < body.y + kind->height; ++cy) {
        for (int cx = body.x; cx < body.x + kind->width; ++cx) {
            const Element e = grid.get_element(cx, cy);
            if (material_of(e.type).move != MoveKind::Powder) continue;
            const int dir = cx < mid ? -1 : 1;
            int tx = dir < 0 ? body.x - 1 : body.x + kind->width;
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
    int left = body.x - reach;
    int right = body.x + kind->width + reach;  // one past
    if (kind->attack == Attack::Slam) {
        if (face_left)
            right = body.x + kind->width;
        else
            left = body.x;
    }
    return target_x < right && target_x + Player::WIDTH > left &&
           target_y < body.y + kind->height && target_y + Player::HEIGHT > body.y;
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
    if (!target_alive || attack_timer > 0 || !body.on_ground || !has_arms()) return false;
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

void Enemy::impact_point(int& x, int& y) const {
    // Where the club lands: as far out along its reach as keeps the whole crater
    // inside it, on the row the feet stand on top of. Far enough out, too, that
    // the debris thrown back toward the troll lands clear of its own feet -- see
    // slam() -- rather than in its footing rows, where it would lift the body.
    const int out = kind->reach - kind->crush_radius - 1;
    x = face_left ? body.x - 1 - out : body.x + kind->width + out;
    y = body.y + kind->height;
}

void Enemy::slam(Grid& grid) {
    const int r = kind->crush_radius;
    int ix = 0, iy = 0;
    impact_point(ix, iy);
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
