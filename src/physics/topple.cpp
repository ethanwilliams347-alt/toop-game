#include "grid.h"
#include <algorithm>
#include <utility>

// Toppling: a held-up piece whose centre of mass is not over what it stands on
// pivots about its last supporting edge until it hits something. See the
// "toppling" block in grid.h for the design; this file is the mechanism.
//
// Its own file because grid.cpp is the cellular automaton and this is the one
// part of the simulation that is not -- it has bodies with identity, an angle and
// a spin. It is still Grid: it reads and writes the same cells, under the same
// rules (no float, nothing allocated per step, everything a function of the seed
// and the writes), and nothing here knows a renderer exists.

namespace {
    // Charred is a state of Wood, not a different thing: a beam that has caught in
    // the middle is still one beam.
    ElementType material_family(ElementType t) {
        return t == ElementType::Charred ? ElementType::Wood : t;
    }

    // floor(n / d) for d > 0. The shear below has to round the same way on both
    // sides of the pivot, and C++ division truncates toward zero, which would
    // round a row a hair left of the pivot the opposite way from one a hair right.
    int64_t floor_div(int64_t n, int64_t d) {
        return n >= 0 ? n / d : -((-n + d - 1) / d);
    }

    // How many whole cells a row (or column) moves under a shear of factor `k`,
    // given its centre in doubled coordinates (c2 = 2 * row + 1, so a centre at
    // half a cell is the integer 1). round(k * c2 / 2), halves up.
    //
    // The integer shift is the whole trick: a shear that moves every cell of a row
    // by the same whole number of cells is a permutation of that row, so the three
    // of them together are a permutation of the lattice. That is what lets a piece
    // turn without losing, doubling or resampling a single cell.
    int shear(fx::v k, int c2) {
        return static_cast<int>(floor_div(static_cast<int64_t>(k) * c2 + fx::ONE,
                                          2 * static_cast<int64_t>(fx::ONE)));
    }

    // Smallest s with s * s >= n. Once per topple, so a plain search is fine.
    int64_t ceil_sqrt(int64_t n) {
        int64_t lo = 0, hi = 1;
        while (hi * hi < n) hi *= 2;
        while (lo < hi) {
            const int64_t mid = (lo + hi) / 2;
            if (mid * mid >= n) hi = mid; else lo = mid + 1;
        }
        return lo;
    }
}

bool Grid::topple_if_unbalanced(int x, int y, bool in_flight) {
    const int seed = get_index(x, y);
    if (balance_visit[seed] == balance_epoch) return false;  // judged already this pass

    const uint8_t tag = cells[seed].piece_tag;
    if (tip_tag_live[tag]) return false;

    // Which cells are "this piece".
    //
    // Just landed, it is the cells that were moving: see the declaration for why.
    //
    // At rest it is narrower than what the support fill walks. The fill treats
    // every touching structural cell with the same tag as one rigid piece, so a
    // wooden post standing on a stone floor is welded to the floor and, to the
    // fill, is the floor -- it has the whole world's footprint and can never be off
    // balance. For holding things up that weld is the right, cautious answer. For
    // tipping over it is the wrong one: nobody glued the post down. So balance is
    // judged per material: the run of this tag in this material, standing on
    // whatever is under it, the other material included. Two materials touching is
    // the one seam the grid has always had that reads as a joint to a player.
    const ElementType family = material_family(cells[seed].type);
    const auto member = [&](int idx) {
        const Element& e = cells[idx];
        if (!is_structural(e.type) || e.piece_tag != tag) return false;
        return in_flight ? e.ticks > 0 : material_family(e.type) == family;
    };

    balance_component.clear();
    balance_component.push_back(seed);
    balance_visit[seed] = balance_epoch;

    for (size_t head = 0; head < balance_component.size(); ++head) {
        // Too big to judge is too big to tip, for the reason MAX_SUPPORT_CELLS
        // gives: when the question costs more than it is worth, the answer that
        // leaves the level standing is the one to guess. This is also what keeps a
        // whole authored landscape -- one welded piece standing on the bottom of
        // the world -- from being weighed every time the player digs it.
        if (static_cast<int>(balance_component.size()) > MAX_SUPPORT_CELLS) return false;

        const int idx = balance_component[head];
        const int cy = idx / width;
        const int cx = idx - cy * width;
        for (int ny = cy - 1; ny <= cy + 1; ++ny) {
            for (int nx = cx - 1; nx <= cx + 1; ++nx) {
                if (!is_within_bounds(nx, ny)) continue;
                const int nidx = get_index(nx, ny);
                if (balance_visit[nidx] == balance_epoch) continue;
                if (!member(nidx)) continue;
                balance_visit[nidx] = balance_epoch;
                balance_component.push_back(nidx);
            }
        }
    }

    // The footprint: every cell of the piece with something solid directly under
    // it that is not more of the piece. Only its extent matters -- the support
    // polygon of a body on a grid is the span from its leftmost contact to its
    // rightmost -- plus, at each end, the lowest contact, which is the corner it
    // would pivot about.
    int64_t n = 0, sum_x2 = 0, sum_y2 = 0;
    int min_sx = width, min_sy = -1, max_sx = -1, max_sy = -1;
    for (const int idx : balance_component) {
        const int cy = idx / width;
        const int cx = idx - cy * width;
        n++;
        sum_x2 += 2 * cx + 1;
        sum_y2 += 2 * cy + 1;

        bool contact;
        if (cy + 1 >= height) {
            contact = true;  // the bottom of the world
        } else {
            const int below = get_index(cx, cy + 1);
            contact = !member(below) && is_solid(cells[below].type);
        }
        if (!contact) continue;

        if (cx < min_sx || (cx == min_sx && cy > min_sy)) { min_sx = cx; min_sy = cy; }
        if (cx > max_sx || (cx == max_sx && cy > max_sy)) { max_sx = cx; max_sy = cy; }
    }

    // Standing on nothing at all is falling's business, not this.
    if (max_sx < 0) return false;

    // Balanced if the mean cell centre lies over the span, edges included. Ties go
    // to standing: an authored piece built exactly to its edge stays built, which
    // is the missed-collapse-over-wrong-collapse asymmetry again. Compared in
    // doubled coordinates times n so nothing is divided.
    const int64_t lo = 2 * static_cast<int64_t>(min_sx) * n;
    const int64_t hi = 2 * static_cast<int64_t>(max_sx + 1) * n;
    if (sum_x2 >= lo && sum_x2 <= hi) return false;

    // Over the right-hand edge: pivot about the bottom-right corner of the
    // rightmost contact, which is the top-right corner of what it stands on.
    // Over the left, the mirror image.
    const bool right = sum_x2 > hi;
    const int px = right ? max_sx + 1 : min_sx;
    const int sy = right ? max_sy : min_sy;
    const int py = sy + 1;

    // A centre of mass already below the corner it would turn about is a piece
    // hanging from that corner, not standing on it, and turning about it would
    // swing it up. Leave it: this is where a piece that slid off an edge and
    // could not get clear ends up, and the alternative is a piece rotated through
    // the floor.
    if (sum_y2 > 2 * static_cast<int64_t>(py) * n) return false;

    const int ground_idx = py < height ? get_index(right ? max_sx : min_sx, py) : -1;
    start_tipping(px, py, ground_idx);
    return true;
}

void Grid::start_tipping(int px, int py, int ground_idx) {
    // A new pool entry only at a new high-water mark; after that, entries and the
    // vectors inside them are reused, so a world that keeps knocking things over
    // stops allocating once it has seen its busiest moment.
    if (tip_count == static_cast<int>(tip_bodies.size())) tip_bodies.emplace_back();
    TipBody& b = tip_bodies[tip_count++];

    b.px = px;
    b.py = py;
    b.ground_idx = ground_idx;
    b.tag = alloc_piece_tag();
    b.age = 0;
    b.moved = false;
    b.theta = 0;
    b.omega = 0;
    b.sx2 = b.sy2 = b.i4 = 0;
    b.shape.clear();
    b.at.clear();

    int64_t r2_max = 1;
    for (const int idx : balance_component) {
        const int cy = idx / width;
        const int cx = idx - cy * width;
        const int lx = cx - px, ly = cy - py;
        b.shape.push_back({static_cast<int16_t>(lx), static_cast<int16_t>(ly)});
        b.at.push_back(idx);

        const int64_t x2 = 2 * lx + 1, y2 = 2 * ly + 1;
        b.sx2 += x2;
        b.sy2 += y2;
        b.i4 += x2 * x2 + y2 * y2;
        r2_max = std::max(r2_max, x2 * x2 + y2 * y2);

        // A fresh tag makes it a piece of its own from here on -- separate from
        // the floor it was welded to by landing on it, and recognisable to the
        // support fill as something that is not its to move. ticks goes back to
        // zero because the fall it may have been in is over.
        cells[idx].piece_tag = b.tag;
        cells[idx].ticks = 0;
        mark_dirty(cx, cy);
    }

    // In doubled units the furthest centre is sqrt(r2_max); halve it, rounding up.
    b.reach = static_cast<int>((ceil_sqrt(r2_max) + 1) / 2);
    if (b.reach < 1) b.reach = 1;

    tip_tag_live[b.tag] = true;
}

bool Grid::tip_pose(const TipBody& b, fx::v theta) {
    // Exact quarter turns first, then what is left -- within 45 degrees -- by
    // three shears. A single shear factor tan(r/2) stays at or under tan(22.5) for
    // that range, which keeps the staircase each shear leaves small; a 90-degree
    // turn done by shears alone would need a factor of one and visibly smear.
    const int32_t k = fx::quarters(theta);
    const fx::v r = theta - k * fx::HALF_PI;
    const fx::SinCos half = fx::sincos(r / 2);
    const fx::v a = -static_cast<fx::v>((static_cast<int64_t>(half.s) * fx::ONE) / half.c);
    const fx::v s = fx::sin(r);
    const int quarter_turns = ((k % 4) + 4) % 4;

    tip_next.clear();
    for (const TipCell& c : b.shape) {
        int x = c.x, y = c.y;
        // A quarter turn clockwise about the pivot corner. On cell centres it is
        // (x, y) -> (-y, x); on the cells' own corners that is (-y - 1, x).
        for (int t = 0; t < quarter_turns; ++t) {
            const int nx = -y - 1;
            y = x;
            x = nx;
        }
        // Paeth: rotate(r) = shearX(-tan(r/2)) . shearY(sin r) . shearX(-tan(r/2)).
        x += shear(a, 2 * y + 1);
        y += shear(s, 2 * x + 1);
        x += shear(a, 2 * y + 1);

        const int wx = b.px + x, wy = b.py + y;
        if (!is_within_bounds(wx, wy)) return false;
        tip_next.push_back(get_index(wx, wy));
    }
    return true;
}

bool Grid::tip_move(TipBody& b) {
    // Most steps of a slow start round to the pose the body is already in.
    if (std::equal(tip_next.begin(), tip_next.end(), b.at.begin())) return true;

    // Borrowing support_visit, which is free here: resolve_support() has finished
    // for this step and starts its next pass on a fresh epoch, so nothing stamped
    // below can be mistaken for one of its answers.
    if (++support_epoch == 0) {
        std::fill(support_visit.begin(), support_visit.end(), uint8_t{0});
        support_epoch = 1;
    }
    const uint8_t here = support_epoch;
    for (const int idx : b.at) support_visit[idx] = here;

    // Blocked by anything solid that is not the body itself. Fluids and gases are
    // not in the way: a piece tipping into a pool pushes the water aside, the
    // same way a falling one sinks through it.
    for (const int idx : tip_next) {
        if (support_visit[idx] != here && is_solid(cells[idx].type)) return false;
    }

    tip_carry.clear();
    for (const int idx : b.at) tip_carry.push_back(cells[idx]);

    tip_displaced.clear();
    for (const int idx : tip_next) {
        if (support_visit[idx] != here && cells[idx].type != ElementType::Empty)
            tip_displaced.push_back(cells[idx]);
    }

    if (++support_epoch == 0) {
        std::fill(support_visit.begin(), support_visit.end(), uint8_t{0});
        support_epoch = 1;
    }
    const uint8_t there = support_epoch;
    for (const int idx : tip_next) support_visit[idx] = there;

    const auto write = [&](int idx, Element e) {
        e.updated_tag = frame_tag;
        cells[idx] = e;
        pixels[idx] = e.color;
        const int cy = idx / width;
        mark_dirty(idx - cy * width, cy);
    };

    // The cells the body is leaving. Because the pose is a permutation there are
    // exactly as many of these as there are cells it is entering, so every bit of
    // fluid it pushes out of the way has somewhere to go -- conserved, though not
    // necessarily put back next to where it was taken from; a turning step is at
    // most a cell at the rim, so in practice it is close.
    size_t d = 0;
    for (const int idx : b.at) {
        if (support_visit[idx] == there) continue;
        write(idx, d < tip_displaced.size() ? tip_displaced[d++] : Element{});

        // Whatever was resting on this cell has just lost it.
        const int cy = idx / width;
        queue_support_check(idx - cy * width, cy - 1);
    }

    for (size_t i = 0; i < tip_carry.size(); ++i) {
        Element e = tip_carry[i];
        e.ticks = 0;
        write(tip_next[i], e);
    }

    // Same length, so this copies into the capacity already there.
    b.at.assign(tip_next.begin(), tip_next.end());
    b.moved = true;
    return true;
}

void Grid::step_tipping() {
    if (tip_count == 0) return;

    // The writes below must not queue the body against itself; the cells it
    // uncovers are queued by hand in tip_move instead.
    resolving_support = true;

    for (int i = 0; i < tip_count;) {
        TipBody& b = tip_bodies[i];

        // Still the body it was? Fire, the brush and the player's dig all write
        // cells through place(), which gives a fresh tag, so a burnt or dug cell
        // shows up here as a cell that is no longer wearing this one. Any change
        // ends the turn: the shape it was rotating no longer exists.
        bool intact = true;
        for (const int idx : b.at) {
            if (!is_structural(cells[idx].type) || cells[idx].piece_tag != b.tag) {
                intact = false;
                break;
            }
        }
        const bool lost_footing = b.ground_idx >= 0 && !is_solid(cells[b.ground_idx].type);
        if (!intact || lost_footing) {
            end_tipping(i, true);
            continue;
        }
        if (++b.age > TIP_MAX_STEPS) {
            end_tipping(i, false);
            continue;
        }

        // Gravity's torque about the pivot over the moment of inertia, both summed
        // per cell with every cell the same mass. The lever arm is the horizontal
        // offset of the centre of mass from the pivot at the current angle.
        //
        //   alpha = g * sum(rx) / sum(r^2),  rx = x cos - y sin
        //
        // In doubled coordinates sum(rx) = (c*sx2 - s*sy2) / 2 and sum(r^2) = i4/4,
        // so alpha = 2g(c*sx2 - s*sy2) / i4.
        const fx::SinCos sc = fx::sincos(b.theta);
        const int64_t lever = static_cast<int64_t>(sc.c) * b.sx2 - static_cast<int64_t>(sc.s) * b.sy2;
        b.omega += static_cast<fx::v>((2 * static_cast<int64_t>(TIP_GRAVITY) * lever) /
                                      (b.i4 * fx::ONE));

        // The rim may travel no faster than a falling piece does, and like a falling
        // piece it travels in steps of at most one cell, each checked, so a fast
        // topple cannot swing through a wall thinner than its speed.
        const fx::v max_omega = MAX_FALL_SPEED * fx::ONE / b.reach;
        if (b.omega > max_omega) b.omega = max_omega;
        if (b.omega < -max_omega) b.omega = -max_omega;

        const int64_t rim = static_cast<int64_t>(b.omega < 0 ? -b.omega : b.omega) * b.reach;
        int substeps = static_cast<int>((rim + fx::ONE - 1) / fx::ONE);
        if (substeps < 1) substeps = 1;
        if (substeps > MAX_FALL_SPEED) substeps = MAX_FALL_SPEED;

        const fx::v start = b.theta;
        bool blocked = false;
        for (int s = 1; s <= substeps; ++s) {
            const fx::v theta = start + static_cast<fx::v>(static_cast<int64_t>(b.omega) * s / substeps);
            if (!tip_pose(b, theta) || !tip_move(b)) {
                blocked = true;
                break;
            }
            b.theta = theta;
        }

        if (blocked) {
            // It hit something. If it got anywhere first, the ordinary support
            // logic looks at it again where it lies: on its side it is balanced;
            // caught on a corner it may tip again about that. If it did not move at
            // all, it is wedged, and asking again would only start the same topple
            // into the same obstacle every step, so it stays where it is.
            end_tipping(i, b.moved);
            continue;
        }

        // Centre of mass below the pivot: it has gone over an edge rather than
        // onto its side, and is hanging off the corner. A real one would leave the
        // edge here, so it does: one cell outward, clear of the corner, which
        // leaves it standing on nothing and hands it to falling.
        const fx::SinCos now = fx::sincos(b.theta);
        if (static_cast<int64_t>(now.s) * b.sx2 + static_cast<int64_t>(now.c) * b.sy2 > 0) {
            const int dx = b.omega > 0 ? 1 : -1;
            tip_next.clear();
            bool inside = true;
            for (const int idx : b.at) {
                const int cy = idx / width;
                const int cx = idx - cy * width + dx;
                if (!is_within_bounds(cx, cy)) { inside = false; break; }
                tip_next.push_back(get_index(cx, cy));
            }
            if (inside) tip_move(b);
            end_tipping(i, true);
            continue;
        }

        ++i;
    }

    resolving_support = false;
}

void Grid::end_tipping(int i, bool requeue) {
    TipBody& b = tip_bodies[i];
    tip_tag_live[b.tag] = false;

    for (const int idx : b.at) {
        // A body that started mid-fall may have had cells sitting in the deferred
        // queue, which ages them; whatever happens next starts from rest.
        if (is_structural(cells[idx].type) && cells[idx].piece_tag == b.tag) cells[idx].ticks = 0;
        if (requeue) {
            const int cy = idx / width;
            queue_support_check(idx - cy * width, cy);
            mark_dirty(idx - cy * width, cy);
        }
    }

    // Swap-remove. The order bodies are stepped in changes when one ends, but it
    // changes the same way every run, which is all determinism asks.
    --tip_count;
    if (i != tip_count) std::swap(tip_bodies[i], tip_bodies[tip_count]);
}
