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

void Enemy::spawn(int x, int y, const Species& k, Chains* with) {
    *this = Enemy{};
    // Left dead rather than spawned rigid: a tentacled species' parts are
    // posed only along its chains, so without them its legs and lure would
    // never move.
    if (k.tentacles && !with) return;
    if (k.tentacles) {
        chains = with;
        *chains = Chains{};
    }
    kind = &k;
    alive = true;
    body.x = prev_x = x;
    body.y = prev_y = y;
    const body_art::Art& art = *kind->art;
    for (int fy = 0; fy < art.h; ++fy)
        for (int fx = 0; fx < art.w; ++fx)
            set_pixel(fy * art.w + fx,
                      art.is_body(fx, fy)
                          ? static_cast<uint8_t>(1 + rig::part_of(art, kind->rig, fx, fy))
                          : 0);
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
    const rig::Part part = pixel(index) ? static_cast<rig::Part>(pixel(index) - 1)
                                        : rig::part_of(art, kind->rig, x, y);
    const int limb = kind->tentacles ? kind->tentacles->of(part) : -1;
    const rig::Point p =
        limb >= 0 ? arm(limb).fwd[tentacle::link_of(kind->tentacles->chain[limb], x, y)].at(x, y)
                  : pose.forward(part, x, y);
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
        if (const int limb = kind->tentacles ? kind->tentacles->of(part) : -1; limb >= 0) {
            const int index = tentacle_pixel(limb, px, py);
            if (index >= 0) return index;
            continue;
        }
        const rig::Point rest = pose.backward(part, px, py);
        const int x = rig::nearest(rest.x), y = rig::nearest(rest.y);
        const rig::Box home = rig::rest_box(art, r, part);
        if (x < home.x0 || x >= home.x1 || y < home.y0 || y >= home.y1) continue;
        const int index = y * art.w + x;
        if (pixel(index) == 1 + part) return index;
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
    // A tentacled body's arm is its lure, and it strikes with the bulb: a stump
    // has nothing to strike with, and a lure walked on is a leg.
    if (kind->tentacles) {
        const int k = lure_limb();
        return k >= 0 && arm(k).intact;
    }
    const body_art::Art& art = *kind->art;
    for (int fy = art.arm_top; fy < art.h; ++fy)
        for (int fx = 0; fx < art.w; ++fx)
            if (pixel(fy * art.w + fx) && art.is_arm(fx, fy)) return true;
    return false;
}

bool Enemy::has_feet() const {
    // On tentacles, anything whole is something to stand on: one leg hops,
    // and with none the lure is walked on (Motion's "after a sever"). The
    // bottom rows say nothing once a foot can be anywhere.
    if (kind->tentacles) {
        for (int k = 0; k < kind->tentacles->count; ++k)
            if (arm(k).intact) return true;
        return false;
    }
    const body_art::Art& art = *kind->art;
    for (int fy = art.h - art.foot_rows; fy < art.h; ++fy)
        for (int fx = 0; fx < art.w; ++fx)
            if (pixel(fy * art.w + fx) && art.is_foot(fx, fy)) return true;
    return false;
}

bool Enemy::crumble(Grid& grid, int index, int* gx, int* gy) {
    if (!pixel(index)) return false;
    // The last grain of a piece ends it.
    const int piece = piece_of(index);
    set_pixel(index, 0);
    --remaining;
    if (piece >= 0 && --arm(piece).piece_pixels == 0) arm(piece).cut = -1;

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

    // Solid and not loose: a foot inside the step it is climbing. The pixel goes
    // and leaves no grain, for the reason at the declaration.
    if (is_solid(there) && material_of(there).move != MoveKind::Powder) return false;

    // Loose powder already there -- most often a grain this same body dropped a
    // moment ago, because a turned limb can put two pixels within a cell of the
    // same spot (rig.h), and a whole body coming down at once is a solid block
    // of its own grains. The grain goes on top of the powder instead, as a grain
    // dropped onto a pile would, so a body still turns into exactly as many
    // grains as it had pixels. Straight up the column, so the result is the same
    // on every machine, and through powder only, never through a wall -- as far
    // as the powder goes. It used to stop at twice the frame's height, which
    // under a deep drift dropped the grain inside the powder and lost it.
    const auto climb = [&](int cx, int& cy) {
        ElementType t = grid.get_element(cx, cy).type;
        // Terminates: past the top of the world get_element answers Wall.
        while (material_of(t).move == MoveKind::Powder) t = grid.get_element(cx, --cy).type;
        return t;
    };
    there = climb(wx, wy);

    // The column is capped -- a ceiling over the drift, or the top of the world.
    // Try the columns either side, nearest first and left before right so it is
    // the same on every machine, out to the frame's width; a pile against a wall
    // spills over the side rather than vanishing. Only when every one of those is
    // capped too is the grain lost.
    if (is_solid(there)) {
        const int start_y = wy;
        for (int d = 1; d <= kind->frame_w() && is_solid(there); ++d) {
            for (const int side : {-d, d}) {
                int cy = start_y;
                const ElementType t = climb(wx + side, cy);
                if (is_solid(t)) continue;
                wx += side;
                wy = cy;
                there = t;
                break;
            }
        }
        if (is_solid(there)) return false;
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
    if (gx) *gx = wx;
    if (gy) *gy = wy;
    return true;
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
    // A piece already cut off is not the body's, and not to be crumbled here
    // either: marked seen, so the fill neither reaches it nor goes through it.
    bool pieces = false;
    for (int k = 0; kind->tentacles && k < kind->tentacles->count; ++k)
        pieces = pieces || arm(k).cut >= 0;
    for (int i = 0; pieces && i < N; ++i)
        if (in_piece(i)) seen[i] = 1;
    for (int i = 0; i < N; ++i) {
        if (pixel(i) && !seen[i] && art.is_heart(i % W, i / W)) {
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
                if (seen[n] || !pixel(n)) continue;
                seen[n] = 1;
                queue[tail++] = static_cast<int16_t>(n);
            }
        }
    }
    if (kind->tentacles) cut_pieces(seen);
    for (int i = 0; i < N; ++i)
        if (pixel(i) && !seen[i]) crumble(grid, i);
    if (kind->tentacles) mark_intact();

    // --- is it still alive --- on what is still the body's: a piece
    // wriggling on the ground is neither a head nor a share of the body.
    bool head_left = false, heart_left = false;
    int kept = 0;
    for (int i = 0; i < N; ++i) {
        if (!attached(i)) continue;
        const int x = i % W, y = i / W;
        head_left = head_left || art.is_head(x, y);
        heart_left = heart_left || art.is_heart(x, y);
        ++kept;
    }
    const bool mostly_gone = kept * 100 < kind->pixel_count * kind->collapse_percent;
    if (!head_left || !heart_left || !has_feet() || mostly_gone) {
        for (int i = 0; i < N; ++i) crumble(grid, i);
        alive = false;
    }
}

int Enemy::piece_of(int index) const {
    if (!kind->tentacles) return -1;
    const uint8_t p = pixel(index);
    if (p == 0) return -1;
    const int k = kind->tentacles->of(static_cast<rig::Part>(p - 1));
    if (k < 0 || arm(k).cut < 0) return -1;
    const int W = kind->frame_w();
    return tentacle::link_of(kind->tentacles->chain[k], index % W, index / W) >= arm(k).cut ? k
                                                                                            : -1;
}

void Enemy::cut_pieces(std::array<uint8_t, MAX_FRAME_PIXELS>& seen) {
    const tentacle::Set& set = *kind->tentacles;
    const int W = kind->frame_w(), N = W * kind->frame_h();
    for (int k = 0; k < set.count; ++k) {
        const tentacle::Chain& c = set.chain[k];
        Limb& limb = arm(k);
        // One piece a tentacle at a time: cut again while one wriggles, the
        // stump's loss crumbles as it always did.
        if (limb.cut >= 0) continue;
        const uint8_t mine = static_cast<uint8_t>(1 + c.part);
        // The piece starts past the last link anything of the body still
        // holds on to. A link cut through the middle is the body's, and the
        // loose half of it crumbles: one link cannot be in two places.
        int held = -1;
        for (int i = 0; i < N; ++i)
            if (pixel(i) == mine && seen[i])
                held = std::max(held, tentacle::link_of(c, i % W, i / W));
        const int cut = held + 1;
        if (cut >= c.links()) continue;
        int count = 0;
        for (int i = 0; i < N; ++i) {
            if (pixel(i) != mine || seen[i] || tentacle::link_of(c, i % W, i / W) < cut) continue;
            seen[i] = 1;
            ++count;
        }
        if (count == 0) continue;
        // Its joints start where they are drawn now: each link carries its
        // own rest root joint onto the posed one, and the last its tip.
        limb.cut = limb.seam = cut;
        limb.piece_pixels = count;
        limb.piece_left = set.motion.piece_steps;
        for (int j = cut; j <= c.links(); ++j) {
            const int link = std::min(j, c.links() - 1);
            limb.piece[j] = to_world(limb.fwd[link].at(c.rest[j].x, c.rest[j].y));
            limb.piece_was[j] = limb.piece[j];
        }
    }
}

void Enemy::mark_intact() {
    const tentacle::Set& set = *kind->tentacles;
    const int W = kind->frame_w(), N = W * kind->frame_h();
    bool any_leg = false;
    int lure = -1;
    for (int k = 0; k < set.count; ++k) {
        const tentacle::Chain& c = set.chain[k];
        const uint8_t mine = static_cast<uint8_t>(1 + c.part);
        bool tip = false;
        for (int i = 0; i < N && !tip; ++i)
            tip = pixel(i) == mine && !in_piece(i) &&
                  tentacle::link_of(c, i % W, i / W) == c.links() - 1;
        Limb& limb = arm(k);
        limb.intact = tip;
        if (c.role == tentacle::Role::Leg && tip) any_leg = true;
        if (c.role == tentacle::Role::Lure && tip) lure = k;
    }
    // No leg left to stand on, and a whole lure: it walks on that instead.
    // For good -- a leg does not grow back.
    if (!any_leg && lure >= 0) arm(lure).role = tentacle::Role::Leg;
}

int Enemy::legs_standing() const {
    int n = 0;
    for (int k = 0; kind->tentacles && k < kind->tentacles->count; ++k)
        n += arm(k).role == tentacle::Role::Leg && arm(k).intact;
    return n;
}

bool Enemy::crawling() const {
    for (int k = 0; kind->tentacles && k < kind->tentacles->count; ++k)
        if (kind->tentacles->chain[k].role == tentacle::Role::Lure &&
            arm(k).role == tentacle::Role::Leg)
            return true;
    return false;
}

int Enemy::shatter(Grid& grid, int wx, int wy, int radius) {
    if (!alive) return 0;
    const int before = remaining;
    const int r2 = radius * radius;
    const int frame_w = kind->frame_w();
    // Stung in the bulb; felt at all -- a shot into a piece lying cut off
    // hurts nothing that is still the body's.
    bool stung = false, felt = false;
    for (int fy = 0; fy < kind->frame_h(); ++fy) {
        for (int fx = 0; fx < frame_w; ++fx) {
            const int i = fy * frame_w + fx;
            if (!pixel(i)) continue;
            int px = 0, py = 0;
            world_of(fx, fy, px, py);
            const int dx = px - wx;
            const int dy = py - wy;
            if (dx * dx + dy * dy > r2) continue;
            stung = stung || is_bulb(fx, fy);
            felt = felt || !in_piece(i);
            crumble(grid, i);
        }
    }
    settle_after_loss(grid);
    // Rocks back from the hit, from the next step: the pose is not recomputed
    // here, so the rest of this step's arrows find the body where they aimed.
    if (alive && felt) flinch = kind->rig.flinch_steps;
    // The bulb is where a lure-striker is weakest, and the hardest thing on it
    // to hit: it hangs out in front, swaying, and is only still at the top of
    // a wind-up. Hit there, it thrashes, a strike being wound up never comes,
    // and it is two recoveries before it can strike again -- the reward for
    // the risky shot, standing in front of the thing that hits you.
    if (alive && stung && lure_strikes()) {
        windup = 0;
        strike_follow = 0;
        attack_timer = std::max(attack_timer, 2 * kind->attack_interval);
        arm(lure_limb()).flourish = 2 * kind->tentacles->motion.lure_flourish;
    }
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
    did_teleport = false;
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
            if (!pixel(i)) continue;
            int wx = 0, wy = 0;
            world_of(i % frame_w, i / frame_w, wx, wy);
            if (grid.get_element(wx, wy).temperature < BURN_TEMPERATURE) continue;
            // Darkened after the fact, on the grain crumble just wrote, wherever it
            // wrote it. That is not the pixel's own cell when that cell held loose
            // powder -- often the hot sand the body is standing in -- because the
            // grain goes on top of the powder; recolouring (wx, wy) instead would
            // scorch whatever was already there, a little darker every tick.
            int gx = 0, gy = 0;
            if (crumble(grid, i, &gx, &gy)) {
                const uint32_t c = grid.get_element(gx, gy).color;
                const uint32_t scorched = 0xFF000000u | ((c >> 1) & 0x007F7F7Fu);
                grid.paint(gx, gy, ElementType::Sand, scorched);
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
    // body standing in a pile of sand would do. Never further, and never
    // sideways: an enemy appearing ten cells away from where it was buried reads
    // as a bug, not as an escape.
    //
    // Deeper in powder than a step, it pushes the powder out of its box the way a
    // wading species does every step (shove_powder) and then tries the lift
    // again. Without that, sand poured to its waist froze it for the rest of the
    // run: no motion at all, because the stuck branch replaces the step. Buried in
    // something static, it stays put until the player digs it out.
    //
    // A species that wades never collides with powder (overlaps_solid), so it is
    // only ever stuck in something static; the powder in its box is shoved out
    // of it here, every step, before anything else looks at the box.
    if (kind->wades) shove_powder(grid);
    if (overlaps_solid(grid, body.x, body.y)) {
        const auto lift = [&]() {
            for (int up = 1; up <= kind->max_step_height; ++up) {
                if (!overlaps_solid(grid, body.x, body.y - up)) {
                    body.y -= up;
                    did_teleport = true;
                    return true;
                }
            }
            return false;
        };
        if (!lift() && !kind->wades) {
            shove_powder(grid);
            if (overlaps_solid(grid, body.x, body.y)) lift();
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

        // Walking on what it has left (Motion's "after a sever"), decided from
        // what is whole now, so a leg shot off changes the next step: dragged
        // along slowly on the lure, or on one leg in hops -- standing to
        // gather itself, then covering the ground in the air.
        if (kind->tentacles && body.vel_x != 0) {
            const tentacle::Motion& m = kind->tentacles->motion;
            if (crawling()) {
                body.vel_x = fx::mul(body.vel_x, m.crawl_share);
            } else if (legs_standing() == 1 && body.on_ground) {
                if (hop_wait > 0) {
                    --hop_wait;
                    body.vel_x = 0;
                } else {
                    body.vel_y = -m.hop_speed;
                    hop_wait = m.hop_rest;
                }
            }
        }

        // The box's motion: the player's, from box_body.h, with this species'
        // rule.
        body.integrate(grid, rule(), GRAVITY, MAX_FALL_SPEED);
    }
    advance_gait();

    bool landed = attack(grid, target_x, target_y, target_alive);
    if (kind->tentacles) carry_body();
    pose_parts();
    if (kind->tentacles) {
        move_tentacles(grid, target_x, target_y);
        move_pieces(grid);
    }
    finish_pose();
    // A lure strike lands when the bulb, as drawn this step, has touched the
    // target on its way here -- tested after the pose, so what hits is what
    // the player saw coming, and once a strike.
    if (lure_strikes() && target_alive && !strike_hit && strike_live() &&
        bulb_hits(target_x, target_y)) {
        strike_hit = true;
        landed = true;
    }
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
    pose_parts();
    if (kind->tentacles) rest_tentacles();
    finish_pose();
}

void Enemy::pose_parts() {
    const rig::Rig& r = kind->rig;
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
    //
    // A body on tentacle feet bobs and leans with where its feet are instead
    // (carry_body): nothing about it runs on the stride.
    const int sink =
        kind->tentacles
            ? rig::nearest(carried_sink.pos)
            : rig::nearest(fx::mul(fx::from_int(r.bob), fx::mul(stride_sin, stride_sin)));
    fx::v lean = (chasing ? r.chase_lean : 0) + (kind->tentacles ? carried_lean.pos : 0);

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
}

rig::Box Enemy::pad_limit() const {
    const int pad = kind->rig.pad;
    return {-pad, -pad, kind->frame_w() + pad, kind->frame_h() + pad};
}

void Enemy::finish_pose() {
    const rig::Rig& r = kind->rig;
    const body_art::Art& art = *kind->art;
    pose.flatten(r);

    // --- where each part can be --- its rest rectangle's corners put through
    // the pose, plus a cell of slack for rounding. Turning a rectangle's corners
    // bounds everything inside it, so this is a bound, not a guess. A tentacle
    // is the union of its links' rectangles, which place_links built the same
    // way from the chain.
    const rig::Box limit = pad_limit();
    bounds = {limit.x1, limit.y1, limit.x0, limit.y0};
    for (int i = 0; i < rig::PART_COUNT; ++i) {
        const rig::Part part = static_cast<rig::Part>(i);
        const rig::Box home = rig::rest_box(art, r, part);
        rig::Box b{limit.x1, limit.y1, limit.x0, limit.y0};
        if (const int limb = kind->tentacles ? kind->tentacles->of(part) : -1; limb >= 0) {
            for (int k = 0; k < kind->tentacles->chain[limb].links(); ++k) {
                const rig::Box& link = arm(limb).box[k];
                if (link.x0 >= link.x1 || link.y0 >= link.y1) continue;
                b.x0 = std::min(b.x0, link.x0);
                b.y0 = std::min(b.y0, link.y0);
                b.x1 = std::max(b.x1, link.x1);
                b.y1 = std::max(b.y1, link.y1);
            }
        } else if (home.x0 < home.x1 && home.y0 < home.y1) {
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

// --- tentacles ---------------------------------------------------------------

rig::Point Enemy::to_world(rig::Point p) const {
    const fx::v left = fx::from_int(body.x - kind->offset_x()) + body.rem_x;
    const fx::v top = fx::from_int(body.y - kind->offset_y()) + body.rem_y;
    const fx::v x = face_left ? fx::from_int(kind->frame_w() - 1) - p.x : p.x;
    return {left + x, top + p.y};
}

rig::Point Enemy::to_frame(rig::Point w) const {
    const fx::v left = fx::from_int(body.x - kind->offset_x()) + body.rem_x;
    const fx::v top = fx::from_int(body.y - kind->offset_y()) + body.rem_y;
    const fx::v x = w.x - left;
    return {face_left ? fx::from_int(kind->frame_w() - 1) - x : x, w.y - top};
}

void Enemy::carry_body() {
    const tentacle::Set& set = *kind->tentacles;
    const tentacle::Motion& m = set.motion;

    // Each leg's foot against where it stands at rest, in the frame: positive
    // is lower. A foot in the air counts where it is going (its home), so the
    // body leans into a step before it lands rather than after. Last step's
    // feet, which is what the body is standing on as this step's pose is made.
    int legs = 0;
    fx::v drop_sum = 0;
    rig::Point rear{}, front{};
    fx::v rear_drop = 0, front_drop = 0;
    bool have_rear = false, have_front = false;
    for (int k = 0; k < set.count; ++k) {
        const tentacle::Chain& c = set.chain[k];
        const Limb& limb = arm(k);
        // A lure walked on stands nowhere near its rest tip; crawling, the
        // body lies slumped instead (below).
        if (c.role != tentacle::Role::Leg || !limb.intact) continue;
        const rig::Joint rest = c.rest[c.links()];
        const rig::Point at = to_frame(limb.swinging ? limb.home : limb.tip);
        const fx::v drop = at.y - fx::from_int(rest.y);
        drop_sum += drop;
        ++legs;
        if (c.part == rig::RearLeg) {
            rear = at;
            rear_drop = drop;
            have_rear = true;
        } else {
            front = at;
            front_drop = drop;
            have_front = true;
        }
    }
    // In the air the feet hang wherever they trail, which says nothing about
    // ground: the body comes back level and to its height.
    const bool airborne = !body.on_ground && body.vel_y != 0;
    fx::v sink = 0, lean = 0;
    if (legs > 0 && !airborne) {
        const fx::v most = fx::from_int(m.body_sink);
        sink = std::clamp(drop_sum / legs, -most, most);
        if (have_rear && have_front) {
            // The slope from rear foot to front foot, as an angle -- the
            // small-angle tangent is close enough under body_tilt -- over at
            // least ten cells, so two feet put down close together do not
            // tip the body over.
            const fx::v span = std::max(front.x - rear.x, fx::from_int(10));
            const fx::v slope =
                static_cast<fx::v>(int64_t{front_drop - rear_drop} * fx::ONE / span);
            lean = std::clamp(slope, -m.body_tilt, m.body_tilt);
        }
    }
    // With no leg under it the body comes down its box onto the ground, and
    // what is left of it is dragged along there.
    if (crawling()) sink = fx::from_int(m.legless_sink);
    if (footfalls != footfalls_felt) {
        carried_sink.vel += m.footfall_dip;
        footfalls_felt = footfalls;
    }
    tentacle::follow(carried_sink, sink, m.body_follow);
    tentacle::follow(carried_lean, lean, m.body_follow);
}

void Enemy::rest_tentacles() {
    const tentacle::Set& set = *kind->tentacles;
    limbs_face_left = face_left;
    wave_clock = 0;
    for (int k = 0; k < set.count; ++k) {
        const tentacle::Chain& c = set.chain[k];
        Limb& limb = arm(k);
        limb.joint = tentacle::rest_points(c);
        limb.tip = to_world(limb.joint[c.links()]);
        limb.lift_off = limb.tip;
        limb.vel = {};
        limb.aim = limb.tip;
        limb.home = limb.tip;
        limb.swing_t = 0;
        limb.swinging = false;
        limb.grounded = false;
        limb.drawn_was = limb.drawn_now = limb.tip;
        limb.crack = -1;
        limb.flourish = 0;
        limb.role = c.role;
        limb.intact = true;
        limb.cut = limb.seam = -1;
        // Unmoved, so every link's turn is exact identity (tentacle::link_turn)
        // and the rest pose maps every pixel to itself.
        place_links(k, limb.joint, 0, c.links());
    }
}

void Enemy::move_tentacles(const Grid& grid, int target_x, int target_y) {
    const tentacle::Set& set = *kind->tentacles;
    const tentacle::Motion& m = set.motion;
    constexpr fx::v TURN = 4 * fx::HALF_PI;
    wave_clock = (wave_clock + 1) % m.wave_steps;

    // Turned round: the frame has mirrored, so the chains mirror with it and
    // stay where they were in the world. Left as they were, every tentacle
    // would flip across the body in one step and the solve would then drag it
    // back the long way round.
    if (face_left != limbs_face_left) {
        const fx::v edge = fx::from_int(kind->frame_w() - 1);
        for (int k = 0; k < set.count; ++k)
            for (rig::Point& p : arm(k).joint) p.x = edge - p.x;
        limbs_face_left = face_left;
    }

    const fx::v phase = static_cast<fx::v>(int64_t{TURN} * wave_clock / m.wave_steps);
    const fx::SinCos bend = fx::sincos(m.max_bend);
    const rig::Box limit = pad_limit();

    // The roots ride the body, wherever the lean and the bob have put them.
    // All of them first, because which foot steps is decided between the legs
    // (plan_steps) before either is solved.
    tentacle::Points roots{};
    for (int k = 0; k < set.count; ++k) {
        const rig::Joint r0 = set.chain[k].rest[0];
        roots[k] = rig::apply(pose.part[rig::Body], kind->rig.waist,
                              {fx::from_int(r0.x), fx::from_int(r0.y)});
    }
    plan_steps(grid, roots);

    for (int k = 0; k < set.count; ++k) {
        const tentacle::Chain& c = set.chain[k];
        Limb& limb = arm(k);
        const int n = c.links();
        const tentacle::Lengths len = tentacle::rest_lengths(c);
        const rig::Point root = roots[k];
        rig::Point goal = limb.role == tentacle::Role::Leg ? leg_goal(grid, k, root)
                                                           : lure_goal(k, target_x, target_y);
        // Kept far enough inside the pad that a link round it, at its radius,
        // is still inside -- what pose_is_sound in enemy_test holds every pose
        // to.
        const int margin = c.radius + 3;
        goal.x =
            std::clamp(goal.x, fx::from_int(limit.x0 + margin), fx::from_int(limit.x1 - margin));
        goal.y =
            std::clamp(goal.y, fx::from_int(limit.y0 + margin), fx::from_int(limit.y1 - margin));

        tentacle::smooth(limb.joint, n, m.smoothing);
        if (limb.role == tentacle::Role::Leg) {
            // The rear tentacle bows back and the front one forward.
            const fx::v out = c.part == rig::RearLeg ? -m.bow : m.bow;
            tentacle::nudge(limb.joint, n, out, 0);
        }
        tentacle::reach(limb.joint, len, n, root, goal, m.iterations);
        tentacle::limit_bend(limb.joint, n, bend.c, bend.s);

        // What is drawn: the solved chain with the wave through it, held to the
        // bend limit, put back on its ends, and held to the limit again. Limited
        // only last, a joint bent far past it swung the rest of the chain round
        // and moved a planted foot by cells from one step to the next; limited
        // only first, the round of reach after it bent joints past it again.
        // This way the last limit only corrects the little the reach undid. The
        // tentacles are a third of a wave apart, so they never ripple in step.
        //
        // A lure also carries the strike's two flourishes on the drawn copy:
        // the bend rolling down it through the whip, and the thrash after.
        tentacle::Points shown = limb.joint;
        tentacle::wave(shown, n, m.wave_amplitude + limb.flourish, phase + k * (TURN / 3),
                       m.wave_spread);
        if (limb.crack >= 0) tentacle::pulse(shown, n, m.lure_crack, limb.crack);
        tentacle::limit_bend(shown, n, bend.c, bend.s);
        tentacle::reach(shown, len, n, root, goal, 1);
        tentacle::limit_bend(shown, n, bend.c, bend.s);
        // Only as far as the cut: the links past it are a piece now, placed
        // by its own chain (move_pieces). The solve still runs the whole
        // chain, which nothing draws past the cut, so the stump moves as the
        // tentacle it was a part of.
        place_links(k, shown, 0, limb.cut >= 0 ? limb.cut : n);
        if (limb.cut < 0) limb.seam = -1;
        limb.drawn_was = limb.drawn_now;
        limb.drawn_now = to_world(shown[n]);
    }
}

namespace {

rig::Point lerp(rig::Point a, rig::Point b, fx::v k) {
    return {lerp(a.x, b.x, k), lerp(a.y, b.y, k)};
}

}  // namespace

rig::Point Enemy::foot_home(const Grid& grid, int k, rig::Point root, bool& grounded) const {
    const tentacle::Chain& c = kind->tentacles->chain[k];
    const tentacle::Motion& m = kind->tentacles->motion;
    const rig::Joint rest_tip = c.rest[c.links()];
    rig::Point home = to_world({fx::from_int(rest_tip.x), fx::from_int(rest_tip.y)});

    // Led ahead by the distance the body covered this step -- what it really
    // moved, so a body held up by a wall or slowed by a climb leads less --
    // times step_lead of a swing: the foot goes down where the body is going.
    const fx::v moved = fx::from_int(body.x - prev_x) + body.rem_x - prev_rem_x;
    home.x += fx::mul(moved * m.swing_steps, m.step_lead);

    // Ground within reach: from the hip down as far as the tentacle goes, in
    // home's column and then outward a column at a time, nearest first. A
    // column solid at the hip is a wall, not a foothold, and a spot out of the
    // leg's reach is no use either. A solid foothold wins over powder anywhere
    // in the search: a foot on loose sand is a foot that will have to step
    // again when the sand slides.
    const rig::Point hip = to_world(root);
    const fx::v full = tentacle::total_length(c);
    const int top = rig::nearest(hip.y);
    const int deepest = top + fx::trunc(full);
    const int col = rig::nearest(home.x);
    rig::Point loose{};
    bool have_loose = false;
    for (int i = 0; i <= 2 * m.foot_probe; ++i) {
        const int d = (i + 1) / 2 * ((i & 1) != 0 ? 1 : -1);
        const int x = col + d;
        int y = top + 1;
        while (y <= deepest && !is_solid(grid.get_element(x, y).type)) ++y;
        if (y > deepest) continue;
        const rig::Point spot{d == 0 ? home.x : fx::from_int(x), fx::from_int(y - 1)};
        if (tentacle::length(spot.x - hip.x, spot.y - hip.y) > full - fx::ONE) continue;
        if (material_of(grid.get_element(x, y).type).move == MoveKind::Static) {
            grounded = true;
            return spot;
        }
        if (!have_loose) {
            loose = spot;
            have_loose = true;
        }
    }
    if (have_loose) {
        grounded = true;
        return loose;
    }
    // Nothing to stand on in reach -- over a drop -- so home is where the foot
    // would hang, no lower than the leg reaches.
    grounded = false;
    home.y = std::min(home.y, fx::from_int(deepest));
    return home;
}

void Enemy::plan_steps(const Grid& grid, const tentacle::Points& roots) {
    const tentacle::Set& set = *kind->tentacles;
    const tentacle::Motion& m = set.motion;
    int swinging = -1, best = -1;
    fx::v best_far = 0;
    const bool standing = body.x == prev_x && body.rem_x == prev_rem_x;
    // Standing, a foot steps to tidy itself under the body -- but not while a
    // strike is wound up: the stillness is half the telegraph.
    const bool tidy = windup == 0;
    for (int k = 0; k < set.count; ++k) {
        Limb& limb = arm(k);
        // A leg without its foot does not step; it hangs (leg_goal).
        if (limb.role != tentacle::Role::Leg || !limb.intact) continue;
        limb.home = foot_home(grid, k, roots[k], limb.grounded);
        if (limb.swinging) {
            swinging = k;
            continue;
        }
        if (standing && !tidy) continue;
        const fx::v far = tentacle::length(limb.tip.x - limb.home.x, limb.tip.y - limb.home.y);
        const fx::v trigger = fx::from_int(standing ? m.settle_trigger : m.step_trigger);
        if (limb.grounded && far > trigger && far > best_far) {
            best = k;
            best_far = far;
        }
    }
    // In the air nobody steps: leg_goal lets the feet trail instead.
    if (swinging >= 0 || best < 0 || (!body.on_ground && body.vel_y != 0)) return;
    Limb& limb = arm(best);
    limb.swinging = true;
    limb.swing_t = 0;
    limb.lift_off = limb.tip;
}

rig::Point Enemy::leg_goal(const Grid& grid, int k, rig::Point root) {
    const tentacle::Chain& c = kind->tentacles->chain[k];
    const tentacle::Motion& m = kind->tentacles->motion;
    Limb& limb = arm(k);
    const rig::Joint rest_tip = c.rest[c.links()];
    const rig::Point hang = to_world({fx::from_int(rest_tip.x), fx::from_int(rest_tip.y)});

    // In the air there is nothing to stand on: the foot lets go and trails
    // after where it would hang, a quarter of the way there each step. So
    // does a leg whose foot is gone, on the ground or off it.
    if ((!body.on_ground && body.vel_y != 0) || !limb.intact) {
        limb.swinging = false;
        limb.tip.x += (hang.x - limb.tip.x) / 4;
        limb.tip.y += (hang.y - limb.tip.y) / 4;
        return to_frame(limb.tip);
    }

    // Swinging: eased from where it left the ground to home -- re-aimed every
    // step, so the foot follows a body that speeds up or slows under it --
    // along an arc that peaks mid-swing, higher by half the climb when the
    // step is up onto something.
    if (limb.swinging) {
        ++limb.swing_t;
        if (limb.swing_t >= m.swing_steps) {
            limb.swinging = false;
            limb.tip = limb.home;
            ++footfalls;
            return to_frame(limb.tip);
        }
        const fx::v t = fx::from_ratio(limb.swing_t, m.swing_steps);
        limb.tip = lerp(limb.lift_off, limb.home, ease(limb.swing_t, m.swing_steps));
        const fx::v climb = std::max<fx::v>(0, limb.lift_off.y - limb.home.y);
        const fx::v lift = fx::from_int(m.step_lift) + climb / 2;
        limb.tip.y -= fx::mul(lift, fx::sin(fx::mul(2 * fx::HALF_PI, t)));
        return to_frame(limb.tip);
    }

    // Planted, the foot stays where it is unless it cannot: dragged further
    // from the hip than the tentacle reaches -- a lift out of the body's own
    // dust, a landing far from where it took off -- it is put straight down
    // where it belongs; with nothing under it -- the ground dug or shot away --
    // it drops a cell a step until it lands, and the next plan steps it
    // somewhere better once it is far enough from home.
    const rig::Point hip = to_world(root);
    const fx::v full = tentacle::total_length(c);
    if (tentacle::length(limb.tip.x - hip.x, limb.tip.y - hip.y) > full + fx::from_int(2)) {
        limb.tip = limb.home;
    } else {
        const int deepest = rig::nearest(hip.y) + fx::trunc(full);
        const int fx_col = rig::nearest(limb.tip.x), fy = rig::nearest(limb.tip.y);
        if (fy < deepest && !is_solid(grid.get_element(fx_col, fy + 1).type)) limb.tip.y += fx::ONE;
    }
    return to_frame(limb.tip);
}

rig::Point Enemy::lure_goal(int k, int target_x, int target_y) {
    const tentacle::Chain& c = kind->tentacles->chain[k];
    const tentacle::Motion& m = kind->tentacles->motion;
    const rig::Rig& r = kind->rig;
    Limb& limb = arm(k);
    constexpr fx::v TURN = 4 * fx::HALF_PI;

    // --- idle --- in front of the mouth, drifting in a slow figure of eight
    // with the breath.
    const rig::Joint rest_tip = c.rest[c.links()];
    const fx::v breath_phase = static_cast<fx::v>(int64_t{TURN} * breath / r.breathe_steps);
    rig::Point want{fx::from_int(rest_tip.x) +
                        fx::mul(fx::from_int(m.lure_sway), fx::sin(breath_phase)),
                    fx::from_int(rest_tip.y) +
                        fx::mul(fx::from_int(m.lure_sway) / 2, fx::sin(2 * breath_phase))};

    // --- the chase --- held further out, and dipped or raised a little toward
    // the player's middle: dangled at you.
    if (chasing) {
        want.x += fx::from_int(m.lure_reach);
        const rig::Point at = to_frame({fx::from_int(target_x + Player::WIDTH / 2),
                                        fx::from_int(target_y + Player::HEIGHT / 2)});
        want.y += std::clamp((at.y - want.y) / 3, fx::from_int(-10), fx::from_int(10));
    }

    // --- the attack --- reared back over the head through the wind-up; then,
    // over its last strike_steps, whipped over the top and down onto the spot
    // the blow lands on (Enemy::impact_point), along a curve through lure_arc;
    // then left lying there for half the recovery before it is drawn back.
    const rig::Point raised{fx::from_int(m.lure_raise.x), fx::from_int(m.lure_raise.y)};
    int ix = 0, iy = 0;
    impact_point(ix, iy);
    const rig::Point impact = to_frame({fx::from_int(ix), fx::from_int(iy - 1)});
    // Over the top no further out than a little past where it is aimed: a
    // strike aimed close in that arced out to the far point first swung out
    // past its target and came back to it, which reads as a miss that hits.
    const rig::Point arc{std::min(fx::from_int(m.lure_arc.x), impact.x + fx::from_int(6)),
                         fx::from_int(m.lure_arc.y)};
    bool whip = false;
    limb.crack = -1;
    limb.flourish -= limb.flourish / 8;
    if (kind->attack == Attack::Slam && windup > 0) {
        const int t = kind->windup_steps - windup;
        const int raising = kind->windup_steps - r.strike_steps;
        const int rear = std::max(1, raising - m.lure_hold);
        if (t < raising) {
            want = lerp(want, raised, ease(t, rear));
            // Held at the top, trembling a cell: about to go.
            if (t >= rear) want.x += fx::sin(fx::from_int(t) * 2);
        } else {
            // A quadratic curve, raise to impact pulled toward the arc point,
            // with a bend rolling down the stalk ahead of the bulb.
            const fx::v s = fraction(t - raising + 1, r.strike_steps);
            want = lerp(lerp(raised, arc, s), lerp(arc, impact, s), s);
            whip = true;
            limb.crack = s;
            if (s == fx::ONE) limb.flourish = m.lure_flourish;
        }
    } else if (attack_timer > 0) {
        const int interval = kind->attack_interval;
        const int since = interval - attack_timer;
        const int hold = interval / 2;
        want = lerp(impact, want, ease(since - hold, interval - hold));
    }

    // --- the flinch --- jerked back toward the body.
    if (flinch > 0) {
        want.x -= fx::from_int(6) * flinch / r.flinch_steps;
        want.y -= fx::from_int(3) * flinch / r.flinch_steps;
    }

    // The bulb follows on its spring, in the world, so it swings behind a body
    // that turns or lunges. The strike is the exception: there the bulb is put
    // where the blow is, step by step, because a blow that lagged its own
    // wind-up would land after the damage it did.
    const rig::Point w = to_world(want);
    if (whip) {
        limb.vel = {w.x - limb.tip.x, w.y - limb.tip.y};
        limb.tip = w;
    } else {
        tentacle::Follow fx_{limb.tip.x, limb.vel.x, limb.aim.x};
        tentacle::Follow fy{limb.tip.y, limb.vel.y, limb.aim.y};
        tentacle::follow(fx_, w.x, m.lure_follow);
        tentacle::follow(fy, w.y, m.lure_follow);
        limb.tip = {fx_.pos, fy.pos};
        limb.vel = {fx_.vel, fy.vel};
    }
    limb.aim = w;
    return to_frame(limb.tip);
}

void Enemy::place_links(int k, const tentacle::Points& p, int from, int to) {
    const tentacle::Chain& c = kind->tentacles->chain[k];
    Limb& limb = arm(k);
    const rig::Box limit = pad_limit();
    // Every pixel of a link lies within the chain's radius of it, and a link
    // also draws its neighbours' pixels into the notch at the outside of a
    // bend (tentacle_pixel), which are within a cell more. One more for
    // rounding.
    const int slack = c.radius + 2;
    std::array<rig::Xform, tentacle::MAX_LINKS> turns{};
    for (int i = from; i < to; ++i) {
        const rig::Xform t = turns[i] = tentacle::link_turn(c, i, p[i], p[i + 1]);
        const rig::Joint j = c.rest[i];
        limb.fwd[i] = rig::Affine::through(
            [&](int x, int y) { return rig::apply(t, j, {fx::from_int(x), fx::from_int(y)}); });
        limb.bwd[i] = rig::Affine::through(
            [&](int x, int y) { return rig::unapply(t, j, {fx::from_int(x), fx::from_int(y)}); });
        const int ax = rig::nearest(p[i].x), ay = rig::nearest(p[i].y);
        const int bx = rig::nearest(p[i + 1].x), by = rig::nearest(p[i + 1].y);
        limb.box[i] = {std::max(std::min(ax, bx) - slack, limit.x0),
                       std::max(std::min(ay, by) - slack, limit.y0),
                       std::min(std::max(ax, bx) + slack + 1, limit.x1),
                       std::min(std::max(ay, by) + slack + 1, limit.y1)};
    }
    // Each inner joint's halfway turn: the two links' directions summed and
    // normalised, about the rest joint, landing on the posed one.
    for (int j = from + 1; j < to; ++j) {
        const rig::Xform& a = turns[j - 1];
        const rig::Xform& b = turns[j];
        const fx::v sc = a.c + b.c, ss = a.s + b.s;
        const fx::v d = tentacle::length(sc, ss);
        rig::Xform half = a;
        if (d != 0) {
            half.c = static_cast<fx::v>(int64_t{sc} * fx::ONE / d);
            half.s = static_cast<fx::v>(int64_t{ss} * fx::ONE / d);
        }
        const rig::Joint at = c.rest[j];
        half.tx = p[j].x - fx::from_int(at.x);
        half.ty = p[j].y - fx::from_int(at.y);
        limb.joint_bwd[j - 1] = rig::Affine::through([&](int x, int y) {
            return rig::unapply(half, at, {fx::from_int(x), fx::from_int(y)});
        });
    }
}

int Enemy::tentacle_pixel(int k, int px, int py) const {
    const tentacle::Chain& c = kind->tentacles->chain[k];
    const Limb& limb = arm(k);
    const body_art::Art& art = *kind->art;
    const uint8_t mine = static_cast<uint8_t>(1 + c.part);
    const auto in = [&](const rig::Box& b) {
        return px >= b.x0 && px < b.x1 && py >= b.y0 && py < b.y1;
    };
    const auto pixel_there = [&](const rig::Affine& back, int& x, int& y) {
        const rig::Point rest = back.at(px, py);
        x = rig::nearest(rest.x);
        y = rig::nearest(rest.y);
        if (x < 0 || x >= art.w || y < 0 || y >= art.h) return -1;
        const int index = y * art.w + x;
        return pixel(index) == mine ? index : -1;
    };
    // A cell asks each link in turn which of its rest pixels lands there, and
    // the link the pixel belongs to answers for certain.
    for (int i = 0; i < c.links(); ++i) {
        if (!in(limb.box[i])) continue;
        int x = 0, y = 0;
        const int index = pixel_there(limb.bwd[i], x, y);
        if (index >= 0 && tentacle::link_of(c, x, y) == i) return index;
    }
    // No link claims it. At the outside of a bend two links turn apart and
    // leave a notch neither covers with its own pixels; it is filled through
    // the joint's halfway turn, which is half a bend from either link's, so the
    // filled cell is within about a cell of where its pixel is hit -- through
    // either link's own turn it would be a whole bend off. Only pixels within
    // the radius of the joint, which is where the notch is: further along, the
    // cell is simply outside the tentacle.
    const int near = c.radius * c.radius;
    for (int j = 1; j < c.links(); ++j) {
        // The cut is not a joint any more: the two links either side of it
        // are on two chains, and nothing fills between them.
        if (j == limb.cut || j == limb.seam) continue;
        if (!in(limb.box[j - 1]) && !in(limb.box[j])) continue;
        int x = 0, y = 0;
        const int index = pixel_there(limb.joint_bwd[j - 1], x, y);
        if (index < 0) continue;
        const int own = tentacle::link_of(c, x, y);
        const rig::Joint at = c.rest[j];
        if ((own == j - 1 || own == j) && (x - at.x) * (x - at.x) + (y - at.y) * (y - at.y) <= near)
            return index;
    }
    return -1;
}

// --- pieces ------------------------------------------------------------------
//
// A tentacle cut off falls as a chain of its own: Verlet, each joint carrying
// on by what it moved last step and falling, stopped by the ground, then the
// links put back to their lengths. Only the falling is remembered; the wriggle
// goes on a copy, as the wave does on a live tentacle, so the piece writhes
// where it lies rather than crawling off. Its pixels are the body's mask still
// -- drawn and hit through the piece's links -- which is why it needs no pool
// of its own, and why it must stay inside the pad: one that leaves it, the
// body walking away, crumbles where it is.
void Enemy::move_pieces(Grid& grid) {
    const tentacle::Set& set = *kind->tentacles;
    const tentacle::Motion& m = set.motion;
    constexpr fx::v TURN = 4 * fx::HALF_PI;
    const fx::v fall = fx::per_step(fx::per_step(GRAVITY));
    const fx::v fastest = fx::per_step(MAX_FALL_SPEED);
    const fx::SinCos bend = fx::sincos(m.max_bend);
    const rig::Box limit = pad_limit();
    for (int k = 0; k < set.count; ++k) {
        const tentacle::Chain& c = set.chain[k];
        Limb& limb = arm(k);
        if (limb.cut < 0) continue;
        // A joint rests a radius above what it lies on, so the piece lies on
        // the ground rather than half in it: a pixel crumbled inside a wall
        // leaves no grain.
        const auto solid = [&](rig::Point p) {
            return is_solid(grid.get_element(rig::nearest(p.x), rig::nearest(p.y) + c.radius).type);
        };
        if (--limb.piece_left <= 0) {
            crumble_piece(grid, k);
            continue;
        }
        const int from = limb.cut, n = c.links();
        const tentacle::Lengths len = tentacle::rest_lengths(c);

        for (int j = from; j <= n; ++j) {
            rig::Point& p = limb.piece[j];
            rig::Point& was = limb.piece_was[j];
            // A little lost to the air each step, so a piece knocked about
            // comes to rest.
            const fx::v vx = (p.x - was.x) * 15 / 16;
            const fx::v vy = std::min(p.y - was.y + fall, fastest);
            was = p;
            rig::Point next{p.x + vx, p.y + vy};
            // Down onto something: it stops falling and slides on a little.
            if (solid(next)) next = {p.x + vx / 2, p.y};
            if (!solid(next)) p = next;
        }
        // Two rounds of each link put back to its length, both ends moved
        // half the error -- from either end, so neither leads.
        for (int round = 0; round < 2; ++round) {
            for (int j = from; j < n; ++j) {
                rig::Point& a = limb.piece[j];
                rig::Point& b = limb.piece[j + 1];
                const fx::v dx = b.x - a.x, dy = b.y - a.y;
                const fx::v d = tentacle::length(dx, dy);
                if (d == 0) continue;
                const fx::v half = (d - len[j]) / 2;
                const fx::v sx = static_cast<fx::v>(int64_t{dx} * half / d);
                const fx::v sy = static_cast<fx::v>(int64_t{dy} * half / d);
                a = {a.x + sx, a.y + sy};
                b = {b.x - sx, b.y - sy};
            }
        }
        // The lengths can drag a joint lying on the ground down into it -- the
        // rest of the piece hanging off an edge pulls -- and nothing above
        // would bring it back out, so it would sink a little every step. Put
        // back up on top, as far as a link's radius twice over; deeper than
        // that it is wedged, and stays. Put up without the speed of it, or
        // Verlet would read the push as a throw upward next step.
        for (int j = from; j <= n; ++j) {
            rig::Point& p = limb.piece[j];
            int up = 0;
            for (; up < 2 * c.radius && solid(p); ++up) p.y -= fx::ONE;
            if (up > 0) limb.piece_was[j].y = p.y;
        }

        // What is drawn: the piece in the frame, wriggling less and less, put
        // back to its lengths from its cut end and held to the bend limit, so
        // it is a pose as sound as any tentacle's.
        const int links = n - from;
        tentacle::Points q{};
        tentacle::Lengths qlen{};
        for (int j = 0; j <= links; ++j) q[j] = to_frame(limb.piece[from + j]);
        for (int j = 0; j < links; ++j) qlen[j] = len[from + j];
        const fx::v amplitude =
            static_cast<fx::v>(int64_t{m.piece_wriggle} * limb.piece_left / m.piece_steps);
        const fx::v phase = static_cast<fx::v>(
            int64_t{TURN} * (limb.piece_left % m.piece_wriggle_steps) / m.piece_wriggle_steps);
        tentacle::wave(q, links, amplitude, phase, m.wave_spread);
        for (int j = 0; j < links; ++j) q[j + 1] = tentacle::place(q[j], q[j + 1], qlen[j]);
        tentacle::limit_bend(q, links, bend.c, bend.s);

        const int margin = c.radius + 3;
        bool inside = true;
        for (int j = 0; j <= links; ++j)
            inside = inside && q[j].x >= fx::from_int(limit.x0 + margin) &&
                     q[j].x <= fx::from_int(limit.x1 - margin) &&
                     q[j].y >= fx::from_int(limit.y0 + margin) &&
                     q[j].y <= fx::from_int(limit.y1 - margin);
        if (!inside) {
            // Crumbled where it was last drawn, which is still inside.
            crumble_piece(grid, k);
            continue;
        }
        tentacle::Points shown{};
        for (int j = 0; j <= links; ++j) shown[from + j] = q[j];
        place_links(k, shown, from, n);
    }
}

void Enemy::crumble_piece(Grid& grid, int k) {
    const int N = kind->frame_w() * kind->frame_h();
    for (int i = 0; i < N && arm(k).cut >= 0; ++i)
        if (piece_of(i) == k) crumble(grid, i);
    arm(k).cut = -1;
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
    if (strike_follow > 0) --strike_follow;

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
        // A body with no crush radius hits with something that breaks nothing --
        // the fish's hand -- and the ground under the blow is left as it was.
        if (kind->crush_radius > 0) slam(grid);
        attack_timer = kind->attack_interval;
        // A lure lands by touching (update, after the pose), not by reach,
        // and is still moving to hurt for lure_live steps from here.
        if (lure_strikes()) {
            strike_follow = kind->tentacles->motion.lure_live;
            return false;
        }
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
        // A lure is aimed now, at where the target stands, within its reach
        // and clear of its own front: what the player has the wind-up to step
        // out from under. The club's spot is fixed and needs no aim.
        if (lure_strikes()) {
            const int front = face_left ? body.x - 1 : body.x + kind->width;
            const int at = target_x + Player::WIDTH / 2;
            strike_out = std::clamp(face_left ? front - at : at - front, 2, kind->reach - 1);
            strike_hit = false;
        }
    } else {
        face_left = was_left;
    }
    return false;
}

int Enemy::lure_limb() const {
    if (!kind->tentacles) return -1;
    // A lure walked on (crawling) is a leg now, and strikes nothing.
    for (int k = 0; k < kind->tentacles->count; ++k)
        if (kind->tentacles->chain[k].role == tentacle::Role::Lure &&
            arm(k).role == tentacle::Role::Lure)
            return k;
    return -1;
}

bool Enemy::lure_strikes() const { return kind->attack == Attack::Slam && lure_limb() >= 0; }

// The whip, and the follow-through after it lands (strike_follow). Its own
// count rather than read off the recovery timer: a sting's longer recovery
// passes through the same values on its way down, and an idle bulb swaying
// into the player then is not a blow.
bool Enemy::strike_live() const {
    if (windup > 0) return windup <= kind->rig.strike_steps;
    return strike_follow > 0;
}

bool Enemy::bulb_hits(int target_x, int target_y) const {
    const Limb& lure = arm(lure_limb());
    const fx::v r = fx::from_int(kind->tentacles->motion.bulb_radius);
    return tentacle::sweep_touches(lure.drawn_was, lure.drawn_now, r, target_x, target_y,
                                   target_x + Player::WIDTH, target_y + Player::HEIGHT);
}

bool Enemy::is_bulb(int x, int y) const {
    const int k = lure_limb();
    if (k < 0) return false;
    const tentacle::Chain& c = kind->tentacles->chain[k];
    // A bulb lying cut off on the ground stings nothing.
    const int index = y * kind->frame_w() + x;
    return pixel(index) == 1 + c.part && !in_piece(index) &&
           tentacle::link_of(c, x, y) == c.links() - 1;
}

void Enemy::impact_point(int& x, int& y) const {
    // Where the club lands: as far out along its reach as keeps the whole crater
    // inside it, on the row the feet stand on top of. Far enough out, too, that
    // the debris thrown back toward the troll lands clear of its own feet -- see
    // slam() -- rather than in its footing rows, where it would lift the body.
    const int out = lure_strikes() ? strike_out : kind->reach - kind->crush_radius - 1;
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
