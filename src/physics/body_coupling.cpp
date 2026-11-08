#include "grid.h"
#include <algorithm>
#include <cstdint>

// The loose bodies, as seen from outside the grid. See "Coupling" at the bottom
// of grid.h for what this is and the units it speaks; this file is the
// mechanism.
//
// Its own file rather than more of bodies.cpp because it is a different
// question. bodies.cpp is how a body moves against the grid; this is how the
// things the grid does not contain -- the player, the enemies, the arrows --
// read that motion and add to it. Nothing here changes how a body is solved:
// a push becomes velocity on the body before the solve, exactly the way
// gravity does, and the solver then answers it with the same contacts,
// friction and restitution it answers everything else with.
//
// The rules bodies.cpp keeps hold here too: no float, nothing allocated on
// the step (the queue is reserved in the constructor), and every answer a
// function of the grid and the order the pushes arrived in, which is Run::step's
// fixed order.

namespace {
constexpr int64_t ONE = fx::ONE;

int64_t abs64(int64_t v) { return v < 0 ? -v : v; }

// A per-step amount in cells, as an fx per-second one, clamped into fx's range.
// A body moves at most a handful of cells a step (Grid::MAX_FALL_SPEED), far
// inside the range, so the clamp only guards a body flung by something nobody
// planned for from wrapping round to a speed the other way.
fx::v per_second(int64_t per_step) {
    const int64_t v = per_step * fx::STEPS_PER_SECOND;
    const int64_t lim = INT32_MAX;
    return static_cast<fx::v>(std::clamp(v, -lim, lim));
}
}  // namespace

int Grid::body_at(int x, int y) const {
    if (!is_within_bounds(x, y)) return -1;
    const int idx = get_index(x, y);
    if (!is_structural(cells[idx].type)) return -1;
    return tag_body[cells[idx].piece_tag];
}

Grid::BodyMotion Grid::body_velocity_at(int body, int x, int y) const {
    const Body& b = tip_bodies[static_cast<size_t>(body)];
    // The step's motion rather than the velocity the solver left: see the
    // declaration. prev_* are written in step_tipping just before the body moves,
    // so this is exactly what advance_body did -- including a move cut short by
    // whatever it struck, which is the case that matters to what it struck.
    const int64_t vx = b.cx - b.prev_cx;
    const int64_t vy = b.cy - b.prev_cy;
    const int64_t omega = static_cast<int64_t>(b.theta) - b.prev_theta;
    // The point's speed is the centre's plus the turn's, omega x r, with r from
    // the centre to the middle of the cell; clockwise is positive because y
    // points down, as everywhere in bodies.cpp.
    const int64_t rx = static_cast<int64_t>(2 * x + 1) * ONE / 2 - b.cx;
    const int64_t ry = static_cast<int64_t>(2 * y + 1) * ONE / 2 - b.cy;
    BodyMotion m;
    m.vx = per_second(vx - omega * ry / ONE);
    m.vy = per_second(vy + omega * rx / ONE);
    return m;
}

int64_t Grid::body_mass(int body) const { return tip_bodies[static_cast<size_t>(body)].mass; }

int Grid::body_id(int body) const { return tip_bodies[static_cast<size_t>(body)].tag; }

int Grid::body_with_id(int id) const {
    if (id <= 0 || id > 255) return -1;
    return tag_body[id];
}

bool Grid::body_near(int x0, int y0, int x1, int y1) const {
    for (int i = 0; i < tip_count; ++i) {
        const Body& b = tip_bodies[static_cast<size_t>(i)];
        // Every cell of a body is within `reach` of its centre of mass (weigh_body
        // rounds it up and adds the cell a shear can be off by), so the square of
        // that radius is a box that cannot miss one. One more for the rounding of
        // the centre to a whole cell here.
        const int cx = static_cast<int>(b.cx / ONE);
        const int cy = static_cast<int>(b.cy / ONE);
        const int r = b.reach + 1;
        if (cx + r < x0 || cx - r > x1 || cy + r < y0 || cy - r > y1) continue;
        return true;
    }
    return false;
}

bool Grid::queue_impulse(int x, int y, int64_t jx, int64_t jy) {
    if (!is_within_bounds(x, y) || (jx == 0 && jy == 0)) return false;
    const int idx = get_index(x, y);
    if (!is_structural(cells[idx].type)) return false;

    // A queue left over from a step that never drained it -- no body was moving
    // then, so step_tipping returned before its hook -- is stale, and goes
    // before anything joins it. See impulse_step.
    if (impulse_step != step_count) {
        impulses.clear();
        impulse_step = step_count;
    }
    if (static_cast<int>(impulses.size()) >= IMPULSE_CAPACITY) return false;

    if (tag_body[cells[idx].piece_tag] < 0 && !wake_for_push(idx, jx, jy)) return false;

    QueuedImpulse q;
    q.cell = idx;
    q.tag = cells[idx].piece_tag;  // after the wake, which re-tags what it wakes
    q.jx = jx;
    q.jy = jy;
    impulses.push_back(q);
    return true;
}

bool Grid::wake_for_push(int idx, int64_t jx, int64_t jy) {
    // Pressed from above, a piece has nowhere to go but into what it stands on:
    // the reason step_tipping's lean wakes sideways only. Something standing on
    // a crate at rest does not wake it every step by standing there.
    if (jy > 0 && jy >= abs64(jx)) return false;

    // A fresh balance pass, as step_tipping starts one for the pieces bodies
    // wake: what the last pass judged was judged standing still, and being pushed
    // is a new question. Without it, a piece judged earlier in this step's update
    // reads as one already judged, and collect_piece refuses it.
    balance_pass_start = static_cast<uint8_t>(balance_epoch + 1);
    if (balance_pass_start == 0) {
        std::fill(balance_visit.begin(), balance_visit.end(), uint8_t{0});
        balance_epoch = 0;
        balance_pass_start = 1;
    }
    if (!collect_piece(idx, false)) return false;

    // gather_contacts' test, word for word: only a piece standing on something.
    // One with nothing under it is hanging from whatever it is welded to, and
    // nothing pushed it loose.
    bool stands = false;
    int64_t mass = 0;
    for (const int c : balance_component) {
        mass += material_of(cells[c].type).weight;
        const int below = c + width;
        if (!stands && below < width * height && balance_visit[below] != balance_epoch &&
            is_solid(cells[below].type))
            stands = true;
    }
    if (!stands || mass <= 0) return false;

    // Hard enough: the speed this push would give it, in cells per step -- the
    // unit PUSH_WAKE is in -- along whichever axis it pushes harder on. The
    // larger component rather than the length, which would square an impulse
    // past what 64 bits hold; the two differ by at most a factor of root two,
    // inside how loosely a wake threshold is set anyway.
    const int64_t j = std::max(abs64(jx), abs64(jy));
    if (j / (mass * fx::STEPS_PER_SECOND) < PUSH_WAKE) return false;

    start_body(balance_component, 0, 0);
    return true;
}

int64_t Grid::carried_liquid(const Body& b) const {
    // apply_body_forces marks a body touching when it is in liquid (or against
    // something); one touching nothing is in the air and carries none.
    if (!b.touching) return 0;
    int water_row = INT32_MAX;
    int64_t water_fill = 0;
    int liquid_weight = 0;
    for (const int idx : b.at) {
        const int y = idx / width;
        const int x = idx - y * width;
        for (int side = -1; side <= 1; side += 2) {
            if (!is_within_bounds(x + side, y)) continue;
            const Element& e = cells[idx + side];
            if (!is_liquid(e.type)) continue;  // a liquid cell is never the body's own
            const int64_t fill = static_cast<int64_t>(e.mass) * ONE / LIQUID_FULL;
            if (y < water_row || (y == water_row && fill > water_fill)) {
                water_row = y;
                water_fill = fill;
            }
            liquid_weight = std::max<int>(liquid_weight, material_of(e.type).weight);
        }
    }
    if (water_row == INT32_MAX) return 0;
    // Whole cells against the waterline, without the continuous offset
    // apply_body_forces refines it by: a push is judged against a step's motion,
    // and a cell's rounding is far inside how exactly a push is ever aimed.
    const int64_t waterline = static_cast<int64_t>(water_row + 1) * ONE - water_fill;
    int64_t under = 0;
    for (const int idx : b.at) {
        const int64_t y = idx / width;
        under += std::clamp<int64_t>((y + 1) * ONE - waterline, 0, ONE);
    }
    return liquid_weight * under / ONE * LIQUID_ADDED_MASS / ONE;
}

void Grid::apply_queued_impulses() {
    // Only last step's pushes. Anything older was queued before a step on which
    // nothing moved, and the body it was meant for is not this one.
    if (impulse_step + 1 != step_count) {
        impulses.clear();
        return;
    }

    int heavy_for = -1;  // the body `heavy` was last worked out for
    int64_t heavy = 1;
    for (const QueuedImpulse& q : impulses) {
        // Still the body it was pushed as? A cell dug, burnt or crumbled since,
        // or a body that has stopped and handed its cells back, is not there to
        // take it.
        const Element& e = cells[static_cast<size_t>(q.cell)];
        if (!is_structural(e.type) || e.piece_tag != q.tag) continue;
        const int i = tag_body[q.tag];
        if (i < 0) continue;
        Body& b = tip_bodies[static_cast<size_t>(i)];

        // How heavy it is to a push: its own mass, and in liquid the liquid it
        // drags along with it as well (LIQUID_ADDED_MASS) -- the same inertia
        // apply_body_forces divides its buoyancy by. Over its bare mass, the
        // player's weight on a floating raft came to more than the lift that
        // holds the raft up, because the lift is divided by the heavier number,
        // and every raft the player stood on sank. Pushes come in runs on one
        // body, so it is worked out once a run.
        if (heavy_for != i) {
            heavy_for = i;
            heavy = std::max<int64_t>(1, b.mass + carried_liquid(b));
        }

        // Per unit of that and per step: the solver's units, the same ones its
        // `apply` adds a contact's impulse in.
        const int64_t per = heavy * fx::STEPS_PER_SECOND;
        const int64_t dvx = q.jx / per;
        const int64_t dvy = q.jy / per;
        const int y = q.cell / width;
        const int x = q.cell - y * width;
        const int64_t rx = static_cast<int64_t>(2 * x + 1) * ONE / 2 - b.cx;
        const int64_t ry = static_cast<int64_t>(2 * y + 1) * ONE / 2 - b.cy;
        b.vx += dvx;
        b.vy += dvy;
        // And the turn it gives about the centre, r x j over the moment of
        // inertia per unit mass -- solve_contacts' own expression for it.
        b.omega += (rx * dvy - ry * dvx) / ONE * ONE / b.k2;
    }
    impulses.clear();
}
