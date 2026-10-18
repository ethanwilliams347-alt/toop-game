#include "grid.h"
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdint>
#include <utility>

// Loose bodies: pieces of structure that are moving as rigid bodies -- toppling,
// tumbling, sliding, bouncing, floating, being knocked over. See "loose bodies"
// in grid.h for the design; this file is the mechanism.
//
// Its own file because grid.cpp is the cellular automaton and this is the one
// part of the simulation that is not -- it has bodies with identity, momentum and
// a spin. It is still Grid: it reads and writes the same cells, under the same
// rules (no float, nothing allocated per step, everything a function of the seed
// and the writes), and nothing here knows a renderer exists.
//
// Units, throughout: 16.16 fixed point held in int64_t. Positions are world cells
// (cell x spans [x, x + 1)), velocities are cells per step, angles radians,
// clockwise on screen because y points down. Products are divided back down with
// `/ ONE`, which truncates toward zero, rather than shifted, which floors: a
// world mirrored left to right must come out mirrored, and flooring rounds the
// two halves of it differently.

namespace {
constexpr int64_t ONE = fx::ONE;

int64_t mulq(int64_t a, int64_t b) { return a * b / ONE; }
int64_t divq(int64_t a, int64_t b) { return a * ONE / b; }
int64_t abs64(int64_t v) { return v < 0 ? -v : v; }
int64_t clamp64(int64_t v, int64_t lo, int64_t hi) { return v < lo ? lo : (v > hi ? hi : v); }

// floor(n / d) for d > 0. The shear below has to round the same way on both
// sides of the pivot, and C++ division truncates toward zero, which would
// round a row a hair left of the pivot the opposite way from one a hair right.
int64_t floor_div(int64_t n, int64_t d) { return n >= 0 ? n / d : -((-n + d - 1) / d); }

// How many whole cells a row (or column) moves under a shear of factor `k`,
// given its centre in doubled coordinates (c2 = 2 * row + 1, so a centre at
// half a cell is the integer 1). round(k * c2 / 2), halves up.
//
// The integer shift is the whole trick: a shear that moves every cell of a row
// by the same whole number of cells is a permutation of that row, so the three
// of them together are a permutation of the lattice. That is what lets a piece
// turn without losing, doubling or resampling a single cell.
int shear(fx::v k, int c2) {
    return static_cast<int>(
        floor_div(static_cast<int64_t>(k) * c2 + fx::ONE, 2 * static_cast<int64_t>(fx::ONE)));
}

// floor(sqrt(n)), n >= 0. Newton's method from above, in integers.
int64_t isqrt(int64_t n) {
    if (n <= 0) return 0;
    int64_t x = n;
    int64_t y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }
    return x;
}

// A pair of materials' grip and bounce, as 16.16 fractions. Friction at the
// mean; restitution at the larger (see Material::bounce).
int64_t pair_grip(int a, int b) { return static_cast<int64_t>(a + b) * ONE / 200; }
int64_t pair_bounce(int a, int b) { return static_cast<int64_t>(a > b ? a : b) * ONE / 100; }

// The edge of the world reads as stone.
constexpr const Material& WORLD_EDGE = MATERIALS[static_cast<size_t>(ElementType::Wall)];

// The four edge neighbours, then the four corners. Order matters only in that
// it is fixed.
constexpr int NEIGHBOUR_DX[8] = {0, -1, 1, 0, -1, 1, -1, 1};
constexpr int NEIGHBOUR_DY[8] = {1, 0, 0, -1, 1, 1, -1, -1};
}  // namespace

// ---------------------------------------------------------------------------
// Judging a piece at rest
// ---------------------------------------------------------------------------

bool Grid::collect_piece(int seed, bool in_flight) {
    const auto stamped_this_pass = [&](int idx) {
        return balance_visit[idx] != 0 && balance_visit[idx] >= balance_pass_start;
    };
    if (stamped_this_pass(seed)) return false;  // judged already this pass

    const uint8_t tag = cells[seed].piece_tag;
    if (tag_body[tag] >= 0) return false;  // already moving

    // Which cells are "this piece".
    //
    // Just landed, it is the cells that were moving: see topple_if_unbalanced.
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

    // A new stamp for this check. On the rare wrap the whole array is cleared,
    // which forgets earlier checks this pass; that costs them being repeated,
    // never a wrong answer.
    if (++balance_epoch == 0) {
        std::fill(balance_visit.begin(), balance_visit.end(), uint8_t{0});
        balance_epoch = 1;
        balance_pass_start = 1;
    }

    balance_component.clear();
    balance_component.push_back(seed);
    balance_stack.clear();
    balance_stack.push_back(seed);
    balance_visit[seed] = balance_epoch;

    // Depth first, downhill first -- neighbours are pushed in reading order, so the
    // row below comes off the stack first -- for the same reason the support fill
    // runs that way: the ground is down there, and so is the early way out below.
    while (!balance_stack.empty()) {
        const int idx = balance_stack.back();
        balance_stack.pop_back();
        const int cy = idx / width;
        const int cx = idx - cy * width;

        // At rest and standing on the bottom of the world: assume it is standing
        // up. That is nearly always terrain -- one welded piece the width of the
        // level -- and walking it to the size cap on every disturbance is the
        // single biggest cost this check could add: fire eating along a wide slab
        // queues a seed every few cells, each too far from the last to share its
        // walk. Heading downhill gets here in about the depth of the piece rather
        // than a full budget. What it gives up -- an odd shape at rest on the
        // world floor with its weight past its foot -- only ever exists if someone
        // authored it, and leaving it standing is the harmless direction. A piece
        // that has just landed is still weighed, wherever it landed.
        if (!in_flight && cy + 1 >= height) return false;

        for (int ny = cy - 1; ny <= cy + 1; ++ny) {
            for (int nx = cx - 1; nx <= cx + 1; ++nx) {
                if (!is_within_bounds(nx, ny)) continue;
                const int nidx = get_index(nx, ny);
                if (balance_visit[nidx] == balance_epoch) continue;
                if (!member(nidx)) continue;
                // An earlier check's piece: one it gave up on. See balance_visit.
                if (stamped_this_pass(nidx)) return false;
                balance_visit[nidx] = balance_epoch;
                balance_component.push_back(nidx);
                balance_stack.push_back(nidx);

                // Too big to judge is too big to tip, for the reason
                // MAX_SUPPORT_CELLS gives: when the question costs more than it is
                // worth, the answer that leaves the level standing is the one to
                // guess.
                if (static_cast<int>(balance_component.size()) > MAX_SUPPORT_CELLS) return false;
            }
        }
    }
    return true;
}

void Grid::add_load(int64_t& w, int64_t& wx2) {
    // Everything resting on the piece, transitively, each at its real position:
    // the pieces standing on it, the pieces standing on those, and the loose
    // material heaped on any of them. A stack of blocks each set a little further
    // out than the one under it has every block balanced on the next, and the
    // whole stack over the edge of the bottom one; only weighing what is above as
    // the pile it is finds that.
    //
    // Weighed by share. A piece resting partly on this one and partly on
    // something else bears down here with the share of its footing that is here:
    // its contacts below, each worth the share of whatever it stands on. Which
    // fraction of a block bridging two supports each one carries is not something
    // statics settles from the shape alone (the problem is indeterminate), and
    // "in proportion to how much of it stands there" is the answer a player would
    // give. Shares are in 1/LOAD_WHOLE, and the piece itself is worth a whole.
    //
    // Loose material -- sand -- is a column: the unbroken run of it straight
    // above, up to LOAD_SCAN, past which it is not a heap but the rest of the
    // level. Moving bodies are not load; they press through their own contacts.
    //
    // w and wx2 come in as the piece's own sums and go out scaled by LOAD_WHOLE
    // with the load added; the caller only ever compares their ratio.
    if (++load_epoch == 0) {
        std::fill(load_visit.begin(), load_visit.end(), uint8_t{0});
        load_epoch = 1;
    }
    const uint8_t mark = load_epoch;
    constexpr uint16_t WALKING = 0xFFFF;  // load_share_at for the piece being walked

    const auto share_of = [&](int q) -> int64_t {
        if (balance_visit[q] == balance_epoch) return LOAD_WHOLE;  // the piece itself
        if (load_visit[q] == mark && load_share_at[q] != WALKING) return load_share_at[q];
        return 0;
    };
    const auto in_set = [&](int q) {
        return balance_visit[q] == balance_epoch || load_visit[q] == mark;
    };

    int64_t lw = 0, lwx2 = 0;
    int budget = 2 * MAX_SUPPORT_CELLS;  // cells found, across every piece weighed

    load_cells.clear();
    load_pieces.clear();
    for (const int idx : balance_component) load_cells.push_back(idx);

    for (size_t h = 0; h < load_cells.size(); ++h) {
        const int idx = load_cells[h];
        if (idx < width) continue;  // top row of the world: nothing above
        const int up = idx - width;
        if (in_set(up)) continue;
        const Element& e = cells[up];
        if (!is_solid(e.type) || is_tipping(up)) continue;
        const int64_t here = share_of(idx);
        if (here == 0) continue;

        if (!is_structural(e.type)) {
            int64_t column = 0;
            int depth = 0;
            for (int q = up; q >= 0; q -= width) {
                const Element& c = cells[q];
                if (!is_solid(c.type) || is_structural(c.type) || in_set(q)) break;
                column += material_of(c.type).weight;
                if (++depth >= LOAD_SCAN) {
                    column = 0;  // a ceiling, not a load: it holds itself up
                    break;
                }
            }
            const int cx = idx % width;
            lw += column * here;
            lwx2 += column * here * (2 * cx + 1);
            continue;
        }

        // A piece standing here: its run of one tag and one material, the unit
        // balance judges by.
        const uint8_t tag = e.piece_tag;
        const ElementType family = material_family(e.type);
        const size_t first = load_cells.size();
        load_stack.clear();
        load_stack.push_back(up);
        load_visit[up] = mark;
        load_share_at[up] = WALKING;
        load_cells.push_back(up);
        bool whole = true;
        while (!load_stack.empty()) {
            const int q = load_stack.back();
            load_stack.pop_back();
            const int qy = q / width, qx = q - qy * width;
            for (int ny = qy - 1; ny <= qy + 1; ++ny) {
                for (int nx = qx - 1; nx <= qx + 1; ++nx) {
                    if (!is_within_bounds(nx, ny)) continue;
                    const int nq = get_index(nx, ny);
                    if (in_set(nq)) continue;
                    const Element& c = cells[nq];
                    if (!is_structural(c.type) || c.piece_tag != tag ||
                        material_family(c.type) != family)
                        continue;
                    if (is_tipping(nq)) continue;
                    if (--budget <= 0) {
                        whole = false;
                        break;
                    }
                    load_visit[nq] = mark;
                    load_share_at[nq] = WALKING;
                    load_cells.push_back(nq);
                    load_stack.push_back(nq);
                }
                if (!whole) break;
            }
            if (!whole) break;
        }

        // Its share: the mean, over every cell of its footing, of the share of
        // what that cell stands on -- nothing in the set counts for nothing. Too
        // big to walk is a ceiling, as a column too tall to follow is.
        int64_t inside = 0, total = 0;
        if (whole) {
            for (size_t k = first; k < load_cells.size(); ++k) {
                const int below = load_cells[k] + width;
                if (below >= width * height) {
                    ++total;  // the bottom of the world carries it
                    continue;
                }
                if (load_visit[below] == mark && load_share_at[below] == WALKING)
                    continue;  // itself
                if (!is_solid(cells[below].type)) continue;
                ++total;
                inside += share_of(below);
            }
        }
        const uint16_t share = total > 0 ? static_cast<uint16_t>(inside / total) : uint16_t{0};
        if (share > 0) {
            load_pieces.push_back(static_cast<int>(first));
            load_pieces.push_back(static_cast<int>(load_cells.size()));
        }
        for (size_t k = first; k < load_cells.size(); ++k) {
            const int q = load_cells[k];
            load_share_at[q] = share;
            if (share == 0) continue;
            const int64_t cw = material_of(cells[q].type).weight;
            lw += cw * share;
            lwx2 += cw * share * (2 * (q % width) + 1);
        }
        if (budget <= 0) break;
    }

    w = w * LOAD_WHOLE + lw;
    wx2 = wx2 * LOAD_WHOLE + lwx2;
}

bool Grid::topple_if_unbalanced(int x, int y, bool in_flight) {
    const int seed = get_index(x, y);
    if (!collect_piece(seed, in_flight)) return false;

    // The footprint: every cell of the piece with something solid directly under
    // it that is not more of the piece. Only its extent matters -- the support
    // polygon of a body on a grid is the span from its leftmost contact to its
    // rightmost.
    //
    // Weighed by material, not counted: a beam that has half burnt to charcoal
    // has its weight in the half that has not, and that is where its centre is.
    int64_t w = 0, wx2 = 0;
    int min_sx = width, max_sx = -1;
    int max_ticks = 0;
    int liquid_weight = 0;  // the heaviest liquid touching it, if any
    for (const int idx : balance_component) {
        const int cy = idx / width;
        const int cx = idx - cy * width;
        const Element& e = cells[idx];
        const int64_t cw = material_of(e.type).weight;
        w += cw;
        wx2 += cw * (2 * cx + 1);
        if (e.ticks > max_ticks) max_ticks = e.ticks;

        for (int d = 0; d < 4; ++d) {
            const int nx = cx + NEIGHBOUR_DX[d], ny = cy + NEIGHBOUR_DY[d];
            if (!is_within_bounds(nx, ny)) continue;
            const ElementType t = cells[get_index(nx, ny)].type;
            if (is_liquid(t) && material_of(t).weight > liquid_weight)
                liquid_weight = material_of(t).weight;
        }

        bool contact;
        if (cy + 1 >= height) {
            contact = true;  // the bottom of the world
        } else {
            const int below = idx + width;
            contact = balance_visit[below] != balance_epoch && is_solid(cells[below].type);
        }
        if (!contact) continue;
        if (cx < min_sx) min_sx = cx;
        if (cx > max_sx) max_sx = cx;
    }

    // A piece that has just landed lands with the speed it fell at. That is what
    // kicks it: the impulse that stops a falling post on one corner of its foot
    // is off its centre, and turns it.
    const int64_t landing =
        in_flight ? static_cast<int64_t>(fall_speed(static_cast<uint8_t>(max_ticks))) * ONE : 0;

    // Lighter than the liquid it sits in: it floats. Only a loose piece -- one
    // that has already fallen, broken or moved, and wears a tag for it. An
    // authored piece is anchored, the same assumption that keeps an untouched
    // level standing: a wooden pier is meant to stand in its lake.
    const int64_t n = static_cast<int64_t>(balance_component.size());
    if (cells[seed].piece_tag != 0 && liquid_weight > 0 && w < liquid_weight * n) {
        start_body(balance_component, 0, landing);
        return true;
    }

    // Standing on nothing at all. At rest, that is a piece hanging from whatever it
    // is welded to -- a lamp under a ceiling -- and it was built that way. Just
    // landed, it is a piece that has struck something with a corner or a side:
    // the support fill joins diagonal neighbours of one tag, so a block falling
    // past the staircase of a slope is "held up" the moment its corner grazes a
    // step, and used to hang there by that corner. Nobody glued it; it goes on
    // as a body, which slides it off.
    if (max_sx < 0) {
        if (!in_flight) return false;
        start_body(balance_component, 0, landing);
        return true;
    }

    add_load(w, wx2);

    // Balanced if the centre of mass, load included, lies over the span, edges
    // included. Ties go to standing: an authored piece built exactly to its edge
    // stays built, which is the missed-collapse-over-wrong-collapse asymmetry
    // again. Compared in doubled coordinates times the weight so nothing is
    // divided.
    const int64_t lo = 2 * static_cast<int64_t>(min_sx) * w;
    const int64_t hi = 2 * static_cast<int64_t>(max_sx + 1) * w;
    // Near the edge, a loose piece is the solver's to judge. See BALANCE_MARGIN;
    // an authored one still stands on the line, which is the asymmetry again.
    const int64_t margin = 2 * w * BALANCE_MARGIN / ONE;
    const bool marginal = wx2 < lo + margin || wx2 > hi - margin;
    if (wx2 >= lo && wx2 <= hi && !(marginal && cells[seed].piece_tag != 0)) {
        // Standing. Then what it stands on carries it, and everything on it, and
        // is asked the same question next: a block set on top of a leaning stack
        // is weighed on every block down to the ground, a level a step. Only
        // downward, so it ends at the ground -- terrain, standing on the bottom of
        // the world, answers at once.
        for (const int idx : balance_component) {
            const int below = idx + width;
            if (below >= width * height || balance_visit[below] == balance_epoch) continue;
            if (!is_structural(cells[below].type) || is_tipping(below)) continue;
            support_rejudge.push_back(below);
        }
        return false;
    }

    // A centre of mass already below its contacts is a piece hanging off a corner
    // rather than standing on it, and it goes all the same; there is no special
    // case for it any more. A piece that is genuinely hooked -- over a ledge,
    // pressed against the face below -- finds every way it could move blocked by
    // a contact, stays where it is, stops, and is handed back unmoved.
    start_body(balance_component, 0, landing);

    // What it carries goes with it. Every piece add_load found standing on it is
    // a body from here too, so the solver has the whole stack: the bottom block
    // of a leaning tower goes over because of the blocks above it, and only the
    // contacts between them can carry their weight round with it. Each piece is
    // a range of grid indices in load_cells, so starting one, which re-tags its
    // cells, changes nothing about which cells the next one is. One bigger than
    // a body may be stays where it is, as anything too big to judge does.
    for (size_t p = 0; p + 1 < load_pieces.size(); p += 2) {
        if (load_pieces[p + 1] - load_pieces[p] > MAX_SUPPORT_CELLS) continue;
        balance_component.clear();
        for (int k = load_pieces[p]; k < load_pieces[p + 1]; ++k) {
            const int q = load_cells[static_cast<size_t>(k)];
            if (is_structural(cells[q].type) && !is_tipping(q)) balance_component.push_back(q);
        }
        if (!balance_component.empty()) start_body(balance_component, 0, 0);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Making and weighing bodies
// ---------------------------------------------------------------------------

int Grid::start_body(const std::vector<int>& cells_of, int64_t vx, int64_t vy) {
    // A new pool entry only at a new high-water mark; after that, entries and the
    // vectors inside them are reused, so a world that keeps knocking things over
    // stops allocating once it has seen its busiest moment.
    //
    // A new entry reserves its two vectors to the most a body can hold -- one past
    // MAX_SUPPORT_CELLS, the size every caller's piece is capped at -- so a body
    // that reuses it later, however large, never grows them mid-step.
    if (tip_count == static_cast<int>(tip_bodies.size())) {
        Body& fresh = tip_bodies.emplace_back();
        fresh.shape.reserve(MAX_SUPPORT_CELLS + 1);
        fresh.at.reserve(MAX_SUPPORT_CELLS + 1);
    }
    const int i = tip_count++;
    Body& b = tip_bodies[i];

    // Before the tag is taken: alloc_piece_tag refuses any tag a live body wears,
    // and this one is not live until it has one.
    b.tag = alloc_piece_tag();
    b.age = 0;
    b.still = 0;
    b.moved = false;
    b.touching = true;
    b.theta = 0;
    b.omega = 0;
    b.shape.clear();
    b.at.clear();

    // The origin the shape is stored about is the lattice point nearest its centre
    // of mass, so that turning it moves the cells as little as the turn does.
    int64_t m = 0, sx2 = 0, sy2 = 0;
    for (const int idx : cells_of) {
        const int cy = idx / width;
        const int cx = idx - cy * width;
        const int64_t cw = material_of(cells[idx].type).weight;
        m += cw;
        sx2 += cw * (2 * cx + 1);
        sy2 += cw * (2 * cy + 1);
    }
    if (m <= 0) m = 1;
    const int ox = static_cast<int>(floor_div(sx2 + m, 2 * m));
    const int oy = static_cast<int>(floor_div(sy2 + m, 2 * m));

    for (const int idx : cells_of) {
        const int cy = idx / width;
        const int cx = idx - cy * width;
        b.shape.push_back({static_cast<int16_t>(cx - ox), static_cast<int16_t>(cy - oy)});
        b.at.push_back(idx);

        // A fresh tag makes it a piece of its own from here on -- separate from
        // the floor it was welded to by landing on it, and recognisable to the
        // support fill as something that is not its to move. ticks goes back to
        // zero because the fall it may have been in is over.
        cells[idx].piece_tag = b.tag;
        cells[idx].ticks = 0;
        mark_dirty(cx, cy);
    }

    weigh_body(b, false);
    b.cx = static_cast<int64_t>(ox) * ONE + b.lcx;
    b.cy = static_cast<int64_t>(oy) * ONE + b.lcy;
    // Nothing comes loose perfectly still: a turn too small to see, drawn from
    // where and when it came loose, so that the same piece let go in two places
    // does not go over in two identical ways.
    const int nudge =
        sim_random::spread(8, world_seed, step_count, static_cast<uint64_t>(cells_of[0]),
                           sim_random::Stream::BodyNudge);
    b.vx = vx;
    b.vy = vy;
    b.omega = static_cast<int64_t>(nudge) * NUDGE_SPIN / 8;
    b.start_cx = b.rest_cx = b.prev_cx = b.cx;
    b.start_cy = b.rest_cy = b.prev_cy = b.cy;
    b.prev_theta = 0;
    b.start_theta = b.rest_theta = 0;
    b.pivot_x = 0;
    b.pivot_y = 0;
    b.pressed = 0;
    b.pivot_held = false;
    b.support = 0;

    tag_body[b.tag] = static_cast<int16_t>(i);
    return i;
}

void Grid::weigh_body(Body& b, bool keep_motion) {
    // Sums over the shape in doubled coordinates, X2 = 2 * x + 1, so a cell's
    // centre is an integer. A uniform square cell about its own centre adds 1/6 of
    // its weight to the moment of inertia (side 1: (1 + 1) / 12), on top of the
    // point-mass term.
    int64_t m = 0, sx2 = 0, sy2 = 0, q4 = 0, grip = 0, bounce = 0;
    for (size_t k = 0; k < b.shape.size(); ++k) {
        const Material& mat = material_of(cells[b.at[k]].type);
        const int64_t cw = mat.weight;
        const int64_t x2 = 2 * b.shape[k].x + 1, y2 = 2 * b.shape[k].y + 1;
        m += cw;
        sx2 += cw * x2;
        sy2 += cw * y2;
        q4 += cw * (x2 * x2 + y2 * y2);
        grip += cw * mat.grip;
        bounce += cw * mat.bounce;
    }
    if (m <= 0) m = 1;

    const int64_t lcx = sx2 * ONE / (2 * m);
    const int64_t lcy = sy2 * ONE / (2 * m);

    // About the origin, then moved to the centre by the parallel-axis theorem.
    const int64_t i_origin = q4 * ONE / 4 + m * ONE / 6;
    const int64_t i_centre = i_origin - m * (mulq(lcx, lcx) + mulq(lcy, lcy));
    int64_t k2 = i_centre / m;
    if (k2 < ONE / 6) k2 = ONE / 6;  // never less than one cell's own

    int64_t r2_max = 0;
    for (const BodyCell& c : b.shape) {
        const int64_t dx = (2 * c.x + 1) * ONE / 2 - lcx;
        const int64_t dy = (2 * c.y + 1) * ONE / 2 - lcy;
        r2_max = std::max(r2_max, dx * dx + dy * dy);
    }
    // In whole cells, rounded up, plus one for the cell a shear can be off by.
    b.reach = static_cast<int>((isqrt(r2_max) + ONE - 1) / ONE) + 1;

    if (keep_motion) {
        // The origin stays where it is and the body keeps moving as it was; only
        // which point of it is the centre has changed. So the new centre is the
        // old one plus the turned offset between them, moving at the speed that
        // point of the old motion had.
        const fx::SinCos sc = fx::sincos(b.theta);
        const int64_t dlx = lcx - b.lcx, dly = lcy - b.lcy;
        const int64_t rdx = (dlx * sc.c - dly * sc.s) / ONE;
        const int64_t rdy = (dlx * sc.s + dly * sc.c) / ONE;
        b.cx += rdx;
        b.cy += rdy;
        b.vx -= mulq(b.omega, rdy);
        b.vy += mulq(b.omega, rdx);
    }

    b.lcx = lcx;
    b.lcy = lcy;
    b.mass = m;
    b.k2 = k2;
    b.grip = grip / m;
    b.bounce = bounce / m;
}

// ---------------------------------------------------------------------------
// Poses
// ---------------------------------------------------------------------------

bool Grid::tip_pose(const Body& b, int64_t cx, int64_t cy, fx::v theta) {
    // The shape turns about its pivot, a lattice point of its own, which lands on
    // the world lattice point nearest where the centre of mass puts it. Cells at
    // the pivot are exactly where the continuous motion says; the shears' rounding
    // grows with distance from it. See Body::pivot_x for why it moves.
    const fx::SinCos full = fx::sincos(theta);
    const int64_t ax = static_cast<int64_t>(b.pivot_x) * ONE - b.lcx;
    const int64_t ay = static_cast<int64_t>(b.pivot_y) * ONE - b.lcy;
    const int64_t px = cx + (ax * full.c - ay * full.s) / ONE;
    const int64_t py = cy + (ax * full.s + ay * full.c) / ONE;
    const int ox = static_cast<int>(floor_div(px + ONE / 2, ONE));
    const int oy = static_cast<int>(floor_div(py + ONE / 2, ONE));

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
    for (const BodyCell& c : b.shape) {
        int x = c.x - b.pivot_x, y = c.y - b.pivot_y;
        // A quarter turn clockwise about the pivot. On cell centres it is
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

        const int wx = ox + x, wy = oy + y;
        if (!is_within_bounds(wx, wy)) return false;
        tip_next.push_back(get_index(wx, wy));
    }
    return true;
}

bool Grid::tip_move(Body& b) {
    // Most sub-steps of a slow body round to the pose it is already in.
    if (std::equal(tip_next.begin(), tip_next.end(), b.at.begin())) return true;

    // Its own marks, not the support fill's. They would be free to borrow here --
    // resolve_support() has finished for the step -- but that is an ordering fact
    // nothing enforces, and a borrowed array is how a later pass inserted between
    // the two quietly reads the other's answers.
    if (++tip_epoch == 0) {
        std::fill(tip_visit.begin(), tip_visit.end(), uint8_t{0});
        tip_epoch = 1;
    }
    const uint8_t here = tip_epoch;
    for (const int idx : b.at) tip_visit[idx] = here;

    // Blocked by anything solid that is not the body itself. Fluids and gases are
    // not in the way: a piece moving into a pool pushes the water aside, the same
    // way a falling one sinks through it.
    for (const int idx : tip_next) {
        if (tip_visit[idx] != here && is_solid(cells[idx].type)) return false;
    }

    tip_carry.clear();
    for (const int idx : b.at) tip_carry.push_back(cells[idx]);

    tip_displaced.clear();
    for (const int idx : tip_next) {
        if (tip_visit[idx] != here && cells[idx].type != ElementType::Empty)
            tip_displaced.push_back(cells[idx]);
    }

    if (++tip_epoch == 0) {
        std::fill(tip_visit.begin(), tip_visit.end(), uint8_t{0});
        tip_epoch = 1;
    }
    const uint8_t there = tip_epoch;
    for (const int idx : tip_next) tip_visit[idx] = there;

    const auto write = [&](int idx, Element e) {
        e.updated_tag = frame_tag;
        cells[idx] = e;
        pixels[idx] = pixel_of(e);
        const int cy = idx / width;
        mark_dirty(idx - cy * width, cy);
        note_warm_if(idx - cy * width, cy, e.temperature);
    };

    // The cells the body is leaving. Because the pose is a permutation there are
    // exactly as many of these as there are cells it is entering, so every bit of
    // fluid it pushes out of the way has somewhere to go -- conserved, though not
    // necessarily put back next to where it was taken from; a sub-step is at most
    // a cell at the rim, so in practice it is close.
    size_t d = 0;
    for (const int idx : b.at) {
        if (tip_visit[idx] == there) continue;
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

bool Grid::square_pose(const Body& b, int64_t& cx, int64_t& cy, fx::v& theta) const {
    const int32_t k = fx::quarters(b.theta);
    const fx::v square = k * fx::HALF_PI;
    if (abs64(b.theta - square) >= REST_SQUARE_ANGLE) return false;

    // At an exact quarter turn there is no shear, and the cells are the shape
    // turned about its pivot, put down on a lattice point. The centre that puts
    // the pivot exactly on that point is where the turned picture of the shape
    // and the cells agree to the pixel.
    const fx::SinCos sc = fx::sincos(square);
    const int64_t ax = static_cast<int64_t>(b.pivot_x) * ONE - b.lcx;
    const int64_t ay = static_cast<int64_t>(b.pivot_y) * ONE - b.lcy;
    const int64_t rx = (ax * sc.c - ay * sc.s) / ONE;
    const int64_t ry = (ax * sc.s + ay * sc.c) / ONE;
    const int px = static_cast<int>(floor_div(b.cx + rx + ONE / 2, ONE));
    const int below = static_cast<int>(floor_div(b.cy + ry + ONE - 1, ONE));

    // Which lattice point: the one under the pivot first, then the one over it,
    // whichever is free. Rounded to the nearer, a body lying on the ground would
    // be lifted as often as lowered, and lifted it is standing on nothing; the
    // support logic would drop it again, a hop after it had stopped.
    const int quarter_turns = ((k % 4) + 4) % 4;
    const auto fits = [&](int ox, int oy) {
        for (const BodyCell& c : b.shape) {
            int x = c.x - b.pivot_x, y = c.y - b.pivot_y;
            for (int t = 0; t < quarter_turns; ++t) {  // as in tip_pose
                const int nx = -y - 1;
                y = x;
                x = nx;
            }
            const int wx = ox + x, wy = oy + y;
            if (!is_within_bounds(wx, wy)) return false;
            const Element& e = cells[get_index(wx, wy)];
            if (is_solid(e.type) && !(is_structural(e.type) && e.piece_tag == b.tag)) return false;
        }
        return true;
    };
    int py = below;
    if (!fits(px, py) && !fits(px, --py)) return false;

    cx = static_cast<int64_t>(px) * ONE - rx;
    cy = static_cast<int64_t>(py) * ONE - ry;
    theta = square;
    return true;
}

void Grid::square_body(Body& b) {
    int64_t cx = 0, cy = 0;
    fx::v theta = 0;
    if (!square_pose(b, cx, cy, theta)) return;
    // Not if it would overlap something: then it rests as it is, at its angle.
    if (!tip_pose(b, cx, cy, theta) || !tip_move(b)) return;
    b.cx = cx;
    b.cy = cy;
    b.theta = theta;
}

// ---------------------------------------------------------------------------
// Contacts
// ---------------------------------------------------------------------------

void Grid::gather_contacts(int i) {
    face_count = 0;

    if (++tip_epoch == 0) {
        std::fill(tip_visit.begin(), tip_visit.end(), uint8_t{0});
        tip_epoch = 1;
    }
    const uint8_t here = tip_epoch;
    for (const int idx : tip_bodies[i].at) tip_visit[idx] = here;

    const auto foreign_solid = [&](int x, int y) {
        if (!is_within_bounds(x, y)) return true;
        const int q = get_index(x, y);
        return tip_visit[q] != here && is_solid(cells[q].type);
    };

    // The surface normal of the solid at (fx, fy), as the sum of the directions
    // from it to every open cell around it. A flat floor gives straight up; the
    // corner of a staircase gives the diagonal, which is what lets a body slide
    // down a slope drawn on a grid. `(dx, dy)` is the way from the body to the
    // obstacle, and the fallback when the neighbourhood says nothing useful -- an
    // isolated grain, open all round, sums to nothing.
    const auto surface_normal = [&](int fx_, int fy_, int dx, int dy, int& gx, int& gy) {
        gx = 0;
        gy = 0;
        if (is_within_bounds(fx_, fy_)) {
            for (int k = 0; k < 8; ++k) {
                const int qx = fx_ + NEIGHBOUR_DX[k], qy = fy_ + NEIGHBOUR_DY[k];
                if (!is_within_bounds(qx, qy)) continue;
                if (!foreign_solid(qx, qy)) {
                    gx += NEIGHBOUR_DX[k];
                    gy += NEIGHBOUR_DY[k];
                }
            }
        }
        if (gx * -dx + gy * -dy <= 0) {
            gx = -dx;
            gy = -dy;
        }
    };

    Body* b = &tip_bodies[i];
    for (size_t k = 0; k < b->at.size(); ++k) {
        const int idx = b->at[k];
        const int y = idx / width;
        const int x = idx - y * width;

        for (int d = 0; d < 8; ++d) {
            const int dx = NEIGHBOUR_DX[d], dy = NEIGHBOUR_DY[d];
            const int ox = x + dx, oy = y + dy;
            if (!foreign_solid(ox, oy)) continue;
            // A corner only touches where neither edge beside it does: otherwise
            // the edges already carry it, and counting the corner too would press
            // twice on one place.
            if (d >= 4 && (foreign_solid(x + dx, y) || foreign_solid(x, y + dy))) continue;

            const bool inside = is_within_bounds(ox, oy);
            const int oidx = inside ? get_index(ox, oy) : -1;
            const Material& om = inside ? material_of(cells[oidx].type) : WORLD_EDGE;

            // Which way the contact pushes. A face beats a corner: where the body
            // meets the obstacle with a flat face of its own -- the face carries on
            // past this cell to one side or the other -- the push is square to that
            // face, whatever shape the obstacle has there. A post standing on the
            // corner of a ledge is held straight up, not shoved sideways by the
            // corner's slant. Only where a corner of the body meets the obstacle
            // does the obstacle's own surface decide, and that is the case smoothing
            // exists for: a body sliding down a staircase touches it with corners.
            int gx = -dx, gy = -dy;
            bool face = false;
            if (d < 4) {
                for (int side = -1; side <= 1 && !face; side += 2) {
                    const int sx = x + side * dy, sy = y + side * dx;  // along the face
                    if (!is_within_bounds(sx, sy) || tip_visit[get_index(sx, sy)] != here) continue;
                    const int bx = sx + dx, by = sy + dy;
                    face = !is_within_bounds(bx, by) || tip_visit[get_index(bx, by)] != here;
                }
            }
            if (!face) surface_normal(ox, oy, dx, dy, gx, gy);
            const int64_t len = isqrt(static_cast<int64_t>(gx * gx + gy * gy) * ONE * ONE);
            const int64_t nx = static_cast<int64_t>(gx) * ONE * ONE / len;
            const int64_t ny = static_cast<int64_t>(gy) * ONE * ONE / len;

            // The middle of the shared edge, or the shared corner. Used for the
            // closing speed; the contacts themselves go at the edge's two ends.
            const int64_t px = static_cast<int64_t>(2 * x + 1 + dx) * ONE / 2;
            const int64_t py = static_cast<int64_t>(2 * y + 1 + dy) * ONE / 2;

            int other = -1;
            if (inside && is_structural(cells[oidx].type)) {
                other = tag_body[cells[oidx].piece_tag];

                // A piece at rest, being struck: wake it, so the blow moves it
                // rather than only stopping this body. Closing speed is read before
                // this step's gravity, so a body lying on something is not closing
                // on it at all.
                if (other < 0) {
                    const int64_t rx = px - b->cx, ry = py - b->cy;
                    const int64_t vpx = b->vx - mulq(b->omega, ry);
                    const int64_t vpy = b->vy + mulq(b->omega, rx);
                    const int64_t closing = mulq(vpx, nx) + mulq(vpy, ny);
                    if (closing < -WAKE_SPEED && collect_piece(oidx, false)) {
                        // Only a piece standing on something: one with nothing
                        // under it is hanging from whatever it is welded to, and
                        // nothing struck it loose.
                        bool stands = false;
                        for (const int c : balance_component) {
                            const int below = c + width;
                            if (below < width * height && balance_visit[below] != balance_epoch &&
                                is_solid(cells[below].type)) {
                                stands = true;
                                break;
                            }
                        }
                        if (stands) {
                            other = start_body(balance_component, 0, 0);
                            b = &tip_bodies[i];  // the pool may have grown
                        }
                    }
                }
            }

            // A pair of bodies is solved once, as the lower-numbered one's contact:
            // that one has already gathered these same edges from its side.
            if (other >= 0 && other < i) continue;

            Contact c;
            c.rax = px;  // the point itself until solve_contacts makes it relative
            c.ray = py;
            c.nx = nx;
            c.ny = ny;
            c.self = i;
            c.other = other;
            c.obstacle = other < 0 && inside && is_structural(cells[oidx].type) ? oidx : -1;
            // Rougher or smoother here than the material says, by a draw that
            // belongs to the spot, so a slab sliding down a slope catches and
            // slips rather than gliding at one rate.
            const uint64_t spot = static_cast<uint64_t>(oy + 1) * static_cast<uint64_t>(width + 2) +
                                  static_cast<uint64_t>(ox + 1);
            const int rough =
                100 + authored_spread(GRIP_ROUGHNESS, spot, sim_random::Stream::SurfaceGrip);
            c.grip = pair_grip(static_cast<int>(b->grip * 100 / ONE), om.grip) * rough / 100;
            c.target = pair_bounce(static_cast<int>(b->bounce * 100 / ONE), om.bounce);

            // Manifold reduction. The normal is snapped to one of eight directions,
            // and along each one, per thing touched, only the two contacts furthest
            // apart are kept: the ends of the face. A slab lying on the floor has a
            // contact under every column; two at its ends hold it up exactly as
            // well, and solve in a fraction of the passes.
            // Within 22.5 degrees of an axis (tan 22.5 is about 5/12) is that
            // axis; anything else is a diagonal.
            const int ax = gx < 0 ? -gx : gx, ay = gy < 0 ? -gy : gy;
            const int qx = 12 * ax < 5 * ay ? 0 : (gx > 0) - (gx < 0);
            const int qy = 12 * ay < 5 * ax ? 0 : (gy > 0) - (gy < 0);
            const int key = (qx + 1) * 3 + (qy + 1) + 9 * (other + 1);

            int f = 0;
            while (f < face_count && faces[f].key != key) ++f;
            if (f == face_count) {
                if (face_count == MAX_CONTACT_FACES) continue;
                faces[f].key = key;
                faces[f].lo_t = INT64_MAX;
                faces[f].hi_t = INT64_MIN;
                ++face_count;
            }

            // An edge touches along its length, so it offers both its ends, and
            // the face keeps whichever are outermost: a post standing on a single
            // cell of ledge is held at that cell's two corners, and the one it
            // turns about when it tips is the real corner of the ledge, not the
            // middle of the cell above it -- half a cell further in, which is half
            // the lever arm of a post only just past its balance point.
            const int ends = d < 4 ? 2 : 1;
            for (int end = 0; end < ends; ++end) {
                int64_t ex = px, ey = py;
                if (ends == 2) {
                    const int64_t half = end == 0 ? -ONE / 2 : ONE / 2;
                    ex += half * dy;  // along the edge: perpendicular to (dx, dy)
                    ey += half * dx;
                }
                c.rax = ex;
                c.ray = ey;
                const int64_t t = -qy * ex + qx * ey;
                if (t < faces[f].lo_t) {
                    faces[f].lo_t = t;
                    faces[f].lo = c;
                }
                if (t > faces[f].hi_t) {
                    faces[f].hi_t = t;
                    faces[f].hi = c;
                }
            }
        }
    }

    // Named for warm starting: which bodies, by tag -- a pool index moves when a
    // body ends -- which of the eight directions, and which end of the face.
    for (int f = 0; f < face_count; ++f) {
        const int other = faces[f].key / 9 - 1;
        const uint32_t base =
            (static_cast<uint32_t>(b->tag) << 16) |
            (static_cast<uint32_t>(other >= 0 ? tip_bodies[other].tag : 0xFF) << 8) |
            (static_cast<uint32_t>(faces[f].key % 9) << 1);
        faces[f].lo.id = base;
        faces[f].hi.id = base | 1u;
        contacts.push_back(faces[f].lo);
        if (faces[f].hi_t != faces[f].lo_t) contacts.push_back(faces[f].hi);
    }
    if (face_count > 0) b->touching = b->contact = true;
}

void Grid::apply_body_forces(Body& b) {
    // gather_contacts has just marked the body's cells, and nothing since has
    // moved any of them or taken a new mark.
    const uint8_t here = tip_epoch;

    b.vy += BODY_GRAVITY;

    // Air drag, against the speed and growing with its square.
    const int64_t speed = isqrt(b.vx * b.vx + b.vy * b.vy);
    const int64_t drag_share = mulq(AIR_DRAG, speed);
    b.vx -= mulq(b.vx, drag_share);
    b.vy -= mulq(b.vy, drag_share);

    // Load and buoyancy are forces at points, and both come down to a sum of
    // weights and a sum of their moments about the centre of mass.
    int64_t push = 0;    // downward, in weight units
    int64_t torque = 0;  // weight units times 16.16 cells, clockwise positive

    // The waterline: the highest liquid beside the body, and how full that cell
    // is. Beside, not above: what a body has displaced lands on top of it as it
    // moves, and water lying on a raft is weight on it, not lift.
    int water_row = INT_MAX;
    int64_t water_fill = 0;  // 16.16 share of a full cell
    int liquid_weight = 0;

    // Where the cells are, on average, against where the centre of mass says the
    // body is: the cells sit at the nearest whole-cell pose, and the difference is
    // how far the body really is below or above them.
    int64_t cell_w = 0, cell_y2 = 0;

    for (const int idx : b.at) {
        const int y = idx / width;
        const int x = idx - y * width;
        const int64_t rx = static_cast<int64_t>(2 * x + 1) * ONE / 2 - b.cx;
        const int64_t cw = material_of(cells[idx].type).weight;
        cell_w += cw;
        cell_y2 += cw * (2 * y + 1);

        // The weight resting on top of this column of the body. See add_load for
        // the column model and why a run taller than LOAD_SCAN is not a load.
        if (y > 0 && tip_visit[idx - width] != here) {
            int64_t column = 0;
            int depth = 0;
            for (int q = idx - width; q >= 0; q -= width) {
                const Element& e = cells[q];
                if (!is_solid(e.type) || tip_visit[q] == here || is_tipping(q)) break;
                column += material_of(e.type).weight;
                if (++depth >= LOAD_SCAN) {
                    column = 0;
                    break;
                }
            }
            push += column;
            torque += column * rx;
        }

        for (int side = -1; side <= 1; side += 2) {
            if (!is_within_bounds(x + side, y)) continue;
            const int q = idx + side;
            if (tip_visit[q] == here || !is_liquid(cells[q].type)) continue;
            const int64_t fill = static_cast<int64_t>(cells[q].mass) * ONE / LIQUID_FULL;
            if (y < water_row || (y == water_row && fill > water_fill)) {
                water_row = y;
                water_fill = fill;
            }
            const int lw = material_of(cells[q].type).weight;
            if (lw > liquid_weight) liquid_weight = lw;
        }
    }

    if (push != 0 || torque != 0) {
        b.vy += push * BODY_GRAVITY / b.mass;
        b.omega += divq(torque * BODY_GRAVITY / ONE / b.mass, b.k2);
    }

    if (water_row == INT_MAX || cell_w == 0) return;

    // Archimedes: lifted by the weight of the liquid it displaces, at the centre
    // of what it displaces -- which is what turns a tilted plank back level, since
    // the side under water is the side pushed up. Each cell counts for the share
    // of it below the waterline, taken from where the body continuously is rather
    // than where its cells were rounded to; counted in whole cells, the lift jumps
    // a row at a time, and a raft chatters between two rows for ever instead of
    // settling between them.
    const int64_t offset = b.cy - cell_y2 * ONE / (2 * cell_w);
    const int64_t waterline = static_cast<int64_t>(water_row + 1) * ONE - water_fill;
    int64_t lift = 0;         // weight units, 16.16
    int64_t lift_torque = 0;  // weight units times cells, 16.16
    int64_t under = 0;        // cells under, 16.16
    for (const int idx : b.at) {
        const int y = idx / width;
        const int x = idx - y * width;
        const int64_t share =
            clamp64(static_cast<int64_t>(y + 1) * ONE + offset - waterline, 0, ONE);
        if (share == 0) continue;
        under += share;
        lift += liquid_weight * share;
        lift_torque +=
            mulq(liquid_weight * share, static_cast<int64_t>(2 * x + 1) * ONE / 2 - b.cx);
    }
    if (under == 0) return;

    // With the liquid it carries along, it is this heavy to anything trying to
    // change its motion (LIQUID_ADDED_MASS), though only its own weight pulls it
    // down. Gravity was given above as if it were in air; the lift takes off the
    // difference, so that gravity and lift together come to
    //   g (m - L) / (m + a L),
    // m its weight, L the liquid's it displaces, a the added share. Taking off
    // g L (1 + a) / (m + a L) does that.
    const int64_t heavy = b.mass * ONE + mulq(LIQUID_ADDED_MASS, lift);  // 16.16
    b.vy -= BODY_GRAVITY * mulq(lift, ONE + LIQUID_ADDED_MASS) / heavy;
    b.omega -= divq(lift_torque * BODY_GRAVITY / heavy, b.k2);

    // Form drag, by the speed squared and the width it pushes through: see
    // LIQUID_FORM_DRAG. Growing with the cells under, so it comes on as a body
    // goes in rather than all at once at the surface.
    const int64_t width_under = isqrt(under * ONE);  // 16.16
    const int64_t form = mulq(LIQUID_FORM_DRAG, width_under) * liquid_weight * ONE / heavy;
    const int64_t speed_in = isqrt(b.vx * b.vx + b.vy * b.vy);
    const int64_t slow = ONE + mulq(form, speed_in);
    b.vx = b.vx * ONE / slow;
    b.vy = b.vy * ONE / slow;

    // And the viscous part, in proportion to how much of it is under.
    const int64_t n = static_cast<int64_t>(b.at.size());
    b.vx -= b.vx * (LIQUID_DRAG * under / ONE) / (256 * n);
    b.vy -= b.vy * (LIQUID_DRAG * under / ONE) / (256 * n);
    b.omega -= b.omega * (LIQUID_SPIN_DRAG * under / ONE) / (256 * n);
    b.touching = true;
}

void Grid::solve_contacts() {
    // Every body's contacts at once, the way a rigid-body engine solves an island:
    // a stack of three blocks is three bodies and two shared contacts, and the
    // passes have to see all of them together to carry the top block's weight
    // down to the floor. Solving each body on its own, against the others' last
    // velocities, has every pair pushing twice in an order that depends on who was
    // stepped first, and a pile of them never stops jittering.
    const auto cross = [](int64_t rx, int64_t ry, int64_t x, int64_t y) {
        return (rx * y - ry * x) / ONE;
    };

    // Per unit of A's mass, the inverse effective mass of a contact along a
    // direction whose lever arm about each centre is rn: 1 + rn^2 / k^2 for each
    // body, B's scaled by how much lighter A is than it.
    const auto inverse_mass = [&](const Contact& c, int64_t ra_n, int64_t rb_n) {
        int64_t k = ONE + ra_n * ra_n / tip_bodies[c.self].k2;
        if (c.other >= 0) k += mulq(c.rho, ONE + rb_n * rb_n / tip_bodies[c.other].k2);
        return k;
    };

    // How fast the contact point on A is moving relative to the one on B.
    const auto relative = [&](const Contact& c, int64_t& vx, int64_t& vy) {
        const Body& a = tip_bodies[c.self];
        vx = a.vx - mulq(a.omega, c.ray);
        vy = a.vy + mulq(a.omega, c.rax);
        if (c.other >= 0) {
            const Body& bb = tip_bodies[c.other];
            vx -= bb.vx - mulq(bb.omega, c.rby);
            vy -= bb.vy + mulq(bb.omega, c.rbx);
        }
    };

    const auto apply = [&](const Contact& c, int64_t jx, int64_t jy) {
        Body& a = tip_bodies[c.self];
        a.vx += jx;
        a.vy += jy;
        a.omega += divq(cross(c.rax, c.ray, jx, jy), a.k2);
        if (c.other >= 0) {
            Body& bb = tip_bodies[c.other];
            const int64_t bx = mulq(c.rho, jx), by = mulq(c.rho, jy);
            bb.vx -= bx;
            bb.vy -= by;
            bb.omega -= divq(cross(c.rbx, c.rby, bx, by), bb.k2);
        }
    };

    for (Contact& c : contacts) {
        const Body& a = tip_bodies[c.self];
        const int64_t px = c.rax, py = c.ray;
        c.rax = px - a.cx;
        c.ray = py - a.cy;
        if (c.other >= 0) {
            const Body& bb = tip_bodies[c.other];
            c.rbx = px - bb.cx;
            c.rby = py - bb.cy;
            c.rho = divq(a.mass, bb.mass);
        }
        const int64_t tx = -c.ny, ty = c.nx;
        c.kn = inverse_mass(c, cross(c.rax, c.ray, c.nx, c.ny), cross(c.rbx, c.rby, c.nx, c.ny));
        c.kt = inverse_mass(c, cross(c.rax, c.ray, tx, ty), cross(c.rbx, c.rby, tx, ty));

        // Restitution is set once, from the speed it arrives at: the solver's
        // passes then aim for that separating speed. target held the pair's
        // bounce until now.
        int64_t vx, vy;
        relative(c, vx, vy);
        const int64_t vn = mulq(vx, c.nx) + mulq(vy, c.ny);
        c.target = vn < -BOUNCE_MIN_SPEED ? -mulq(c.target, vn) : 0;
        c.acc_n = 0;
        c.acc_t = 0;

        // Warm start: begin from the impulse this same contact ended last step
        // with, and apply it now. The passes then only have to find what has
        // changed, rather than rebuild the whole load from nothing every step --
        // which, in a stack, they cannot do: the top block's weight takes a pass
        // per block to reach the bottom, and the bottom block has to feel it to
        // know it is going over.
        const uint8_t tag = tip_bodies[c.self].tag;
        for (int k = warm_from[tag]; k < warm_to[tag]; ++k) {
            const Contact& was = prev_contacts[static_cast<size_t>(k)];
            if (was.id != c.id) continue;
            c.acc_n = mulq(was.acc_n, WARM_START);
            c.acc_t = clamp64(was.acc_t, -mulq(c.grip, c.acc_n), mulq(c.grip, c.acc_n));
            apply(c, mulq(c.acc_n, c.nx) - mulq(c.acc_t, c.ny),
                  mulq(c.acc_n, c.ny) + mulq(c.acc_t, c.nx));
            break;
        }
    }

    for (int pass = 0; pass < BODY_SOLVER_PASSES; ++pass) {
        for (Contact& c : contacts) {
            const int64_t tx = -c.ny, ty = c.nx;
            int64_t vx, vy;

            // Friction first, against the normal impulse so far: Coulomb's cone,
            // |friction| <= grip * normal, with the impulse accumulated over the
            // passes and the clamp applied to the total, not to each pass's share.
            relative(c, vx, vy);
            const int64_t vt = mulq(vx, tx) + mulq(vy, ty);
            const int64_t limit = mulq(c.grip, c.acc_n);
            const int64_t acc_t = clamp64(c.acc_t - divq(vt, c.kt), -limit, limit);
            const int64_t dt = acc_t - c.acc_t;
            c.acc_t = acc_t;
            if (dt != 0) apply(c, mulq(dt, tx), mulq(dt, ty));

            // Then the normal: push until the contact separates at its target
            // speed, never pull.
            relative(c, vx, vy);
            const int64_t vn = mulq(vx, c.nx) + mulq(vy, c.ny);
            const int64_t acc_n = std::max<int64_t>(c.acc_n + divq(c.target - vn, c.kn), 0);
            const int64_t dn = acc_n - c.acc_n;
            c.acc_n = acc_n;
            if (dn != 0) apply(c, mulq(dn, c.nx), mulq(dn, c.ny));
        }
    }

    // Kept for the next step's warm start, indexed by the first body's tag. A
    // body's contacts are gathered together, so each tag's are one run.
    prev_contacts.assign(contacts.begin(), contacts.end());
    std::fill(std::begin(warm_from), std::end(warm_from), 0);
    std::fill(std::begin(warm_to), std::end(warm_to), 0);
    for (size_t k = 0; k < prev_contacts.size(); ++k) {
        const uint8_t tag = tip_bodies[prev_contacts[k].self].tag;
        if (warm_to[tag] == 0) warm_from[tag] = static_cast<int>(k);
        warm_to[tag] = static_cast<int>(k) + 1;
    }

    // Rolling resistance: an angular impulse against the spin, no more than the
    // normal impulse holding the body up times ROLLING_ARM. Applied once, after
    // the contacts have settled, so it opposes what they leave rather than
    // fighting them. B feels a shared contact's push scaled to its own mass.
    for (int i = 0; i < tip_count; ++i) tip_bodies[i].support = 0;
    for (const Contact& c : contacts) {
        tip_bodies[c.self].support += c.acc_n;
        if (c.other >= 0) tip_bodies[c.other].support += mulq(c.rho, c.acc_n);
    }
    for (int i = 0; i < tip_count; ++i) {
        Body& a = tip_bodies[i];
        const int64_t most = divq(mulq(a.support, ROLLING_ARM), a.k2);
        a.omega -= clamp64(a.omega, -most, most);
    }

    // Each body's hardest-pressed contact, which is where it should turn about:
    // see Body::pivot_x.
    // And whether its pivot is at a contact at all: within a cell of one.
    for (int i = 0; i < tip_count; ++i) {
        Body& a = tip_bodies[i];
        a.pressed = 0;
        a.pivot_held = false;
        const fx::SinCos sc = fx::sincos(a.theta);
        const int64_t ax = static_cast<int64_t>(a.pivot_x) * ONE - a.lcx;
        const int64_t ay = static_cast<int64_t>(a.pivot_y) * ONE - a.lcy;
        a.press_x = (ax * sc.c - ay * sc.s) / ONE;  // the pivot, from the centre, for now
        a.press_y = (ax * sc.s + ay * sc.c) / ONE;
    }
    const auto near_pivot = [](Body& a, int64_t rx, int64_t ry) {
        if (abs64(rx - a.press_x) <= ONE && abs64(ry - a.press_y) <= ONE) a.pivot_held = true;
    };
    for (const Contact& c : contacts) {
        near_pivot(tip_bodies[c.self], c.rax, c.ray);
        if (c.other >= 0) near_pivot(tip_bodies[c.other], c.rbx, c.rby);
    }
    for (const Contact& c : contacts) {
        Body& a = tip_bodies[c.self];
        if (c.acc_n > a.pressed) {
            a.pressed = c.acc_n;
            a.press_x = a.cx + c.rax;
            a.press_y = a.cy + c.ray;
        }
        if (c.other >= 0) {
            Body& bb = tip_bodies[c.other];
            const int64_t felt = mulq(c.rho, c.acc_n);
            if (felt > bb.pressed) {
                bb.pressed = felt;
                bb.press_x = bb.cx + c.rbx;
                bb.press_y = bb.cy + c.rby;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Moving
// ---------------------------------------------------------------------------

bool Grid::advance_body(Body& b) {
    // Whether a cell has changed place, which is what moving means to anyone
    // watching; the continuous state can creep a fraction of a cell without it.
    bool shifted = false;
    const auto move = [&]() {
        if (std::equal(tip_next.begin(), tip_next.end(), b.at.begin())) return true;
        if (!tip_move(b)) return false;
        shifted = true;
        return true;
    };

    // As fast as a falling piece and no faster, at the rim as well as the centre:
    // MAX_FALL_SPEED is what the grid can be trusted to resolve a cell at a time.
    const int64_t top = static_cast<int64_t>(MAX_FALL_SPEED) * ONE;
    b.vx = clamp64(b.vx, -top, top);
    b.vy = clamp64(b.vy, -top, top);
    const int64_t spin_top = top / b.reach;
    b.omega = clamp64(b.omega, -spin_top, spin_top);

    // In sub-steps of at most one cell of travel anywhere on the body, each one
    // checked, so nothing fast passes through anything thin.
    const int64_t rim = abs64(b.vx) + abs64(b.vy) + abs64(b.omega) * b.reach;
    int substeps = static_cast<int>((rim + ONE - 1) / ONE);
    if (substeps < 1) substeps = 1;
    if (substeps > 3 * MAX_FALL_SPEED) substeps = 3 * MAX_FALL_SPEED;

    const int64_t cx0 = b.cx, cy0 = b.cy;
    const fx::v th0 = b.theta;
    // Positions along the step in sixteenths of a sub-step, so a blocked one can
    // be split.
    const auto at = [&](int q, int64_t& cx, int64_t& cy, fx::v& th) {
        const int64_t whole = 16 * static_cast<int64_t>(substeps);
        cx = cx0 + b.vx * q / whole;
        cy = cy0 + b.vy * q / whole;
        th = static_cast<fx::v>(th0 + b.omega * q / whole);
    };

    for (int s = 1; s <= substeps; ++s) {
        int64_t cx, cy;
        fx::v th;
        at(16 * s, cx, cy, th);
        if (!tip_pose(b, cx, cy, th) || !move()) {
            // Blocked somewhere in this sub-step. Close the gap by halves, so it
            // stops touching what it hit -- not up to a cell short of it, where the
            // next step finds no contact to take the blow and the speed just
            // drains away instead of landing.
            int lo = 16 * (s - 1), hi = 16 * s;
            while (hi - lo > 1) {
                const int mid = (lo + hi) / 2;
                at(mid, cx, cy, th);
                if (tip_pose(b, cx, cy, th) && move()) {
                    b.cx = cx;
                    b.cy = cy;
                    b.theta = th;
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            if (lo > 0) return shifted;  // it got somewhere; the contacts take it from here
            at(16, cx, cy, th);

            // Nowhere at all. Usually something it was not touching a moment ago,
            // and the next step's contacts deal with it. But the turn and the
            // slide are tried apart first: a shear rounds a rim cell into a corner
            // the contacts cannot see often enough that a body could otherwise
            // hang there, turning into the same cell every step.
            if (tip_pose(b, cx, cy, th0) && move()) {
                b.cx = cx;
                b.cy = cy;
                b.omega /= 2;
                return shifted;
            }
            if (tip_pose(b, cx0, cy0, th) && move()) {
                b.theta = th;
                b.vx /= 2;
                b.vy /= 2;
                return shifted;
            }
            // Truly stuck: lose half of everything, so it comes to rest rather
            // than pushing for ever.
            b.vx /= 2;
            b.vy /= 2;
            b.omega /= 2;
            return false;
        }
        b.cx = cx;
        b.cy = cy;
        b.theta = th;
    }
    return shifted;
}

bool Grid::snap_overhang(int i, int64_t load) {
    // A beam held at one end. What it has to carry at the edge of what holds it
    // is the bending moment of everything past that edge -- each column's weight
    // times how far out it is -- and what it can carry grows with the square of
    // how thick it is there (Material::strength). A slab landing half over a
    // drop, or a long plank left sticking out over a ledge, snaps at the edge
    // when the first is more than the second; `load` is how many times its own
    // weight it is carrying, in 16.16, which is what makes a hard landing break
    // what a gentle one does not.
    Body& b = tip_bodies[i];
    if (static_cast<int>(b.at.size()) < MIN_FRACTURE_CELLS) return false;

    if (++tip_epoch == 0) {
        std::fill(tip_visit.begin(), tip_visit.end(), uint8_t{0});
        tip_epoch = 1;
    }
    const uint8_t here = tip_epoch;
    int min_x = width, max_x = -1;
    for (const int idx : b.at) {
        tip_visit[idx] = here;
        const int x = idx % width;
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
    }

    const int span = max_x - min_x + 1;
    snap_cols.assign(static_cast<size_t>(span), SnapColumn{});
    for (const int idx : b.at) {
        SnapColumn& c = snap_cols[static_cast<size_t>(idx % width - min_x)];
        const Material& mat = material_of(cells[idx].type);
        ++c.cells;
        c.weight += mat.weight;
        if (mat.strength < c.strength) c.strength = mat.strength;
        const int below = idx + width;
        if (below >= width * height || (tip_visit[below] != here && is_solid(cells[below].type)))
            c.held = true;
    }

    int held_lo = -1, held_hi = -1;
    for (int c = 0; c < span; ++c) {
        if (!snap_cols[static_cast<size_t>(c)].held) continue;
        if (held_lo < 0) held_lo = c;
        held_hi = c;
    }
    if (held_lo < 0) return false;  // in the air: nothing to bend over

    for (int side = 1; side >= -1; side -= 2) {
        const int root = side > 0 ? held_hi + 1 : held_lo - 1;  // first column out
        if (root < 0 || root >= span) continue;
        const SnapColumn& r = snap_cols[static_cast<size_t>(root)];
        if (r.cells == 0) continue;

        // In doubled cells, so a column's centre is a whole number out from the
        // edge: 2d + 1 for the column d past the root.
        int64_t moment2 = 0;
        int out = 0;
        for (int c = root; c >= 0 && c < span; c += side) {
            const SnapColumn& col = snap_cols[static_cast<size_t>(c)];
            moment2 += col.weight * (2 * static_cast<int64_t>((c - root) * side) + 1);
            out += col.cells;
        }
        const int64_t strength =
            std::min<int64_t>(r.strength, snap_cols[static_cast<size_t>(root - side)].strength);
        const int64_t capacity2 = 2 * strength * r.cells * r.cells;
        if (mulq(moment2, load) <= capacity2) continue;
        if (out < 4 || static_cast<int>(b.at.size()) - out < 4) continue;  // a chip, not a break

        // It breaks at the edge. The overhang gets a tag of its own and is asked,
        // next step, what is holding it up -- nothing, so it comes away as a body
        // of its own -- and this body loses those cells and carries on.
        const uint8_t fresh = alloc_piece_tag();
        const int edge_x = min_x + root;
        for (const int idx : b.at) {
            const int x = idx % width;
            if (side > 0 ? x < edge_x : x > edge_x) continue;
            cells[idx].piece_tag = fresh;
            const int y = idx / width;
            queue_support_check(x, y);
            mark_dirty(x, y);
        }
        return true;
    }
    return false;
}

void Grid::step_tipping() {
    if (tip_count == 0) return;

    // The writes below must not queue the body against itself; the cells it
    // uncovers are queued by hand in tip_move instead.
    resolving_support = true;

    // A fresh balance pass, for the pieces bodies wake: what the support resolve
    // judged a moment ago was judged standing still, and being struck is a new
    // question.
    balance_pass_start = static_cast<uint8_t>(balance_epoch + 1);
    if (balance_pass_start == 0) {
        std::fill(balance_visit.begin(), balance_visit.end(), uint8_t{0});
        balance_epoch = 0;
        balance_pass_start = 1;
    }

    // Still the body it was? Fire, the brush and the player's dig all write cells
    // through place(), which gives a fresh tag, so a burnt or dug cell shows up
    // here as a cell that is no longer wearing this one. It leaves the body, and
    // the rest carries on with the motion it had; a beam that chars grows lighter
    // where it chars, and that moves its centre.
    for (int i = 0; i < tip_count;) {
        Body& b = tip_bodies[i];
        bool lost = false;
        int64_t m = 0;
        for (size_t k = 0; k < b.at.size();) {
            const Element& e = cells[b.at[k]];
            if (!is_structural(e.type) || e.piece_tag != b.tag) {
                b.at[k] = b.at.back();
                b.at.pop_back();
                b.shape[k] = b.shape.back();
                b.shape.pop_back();
                lost = true;
                continue;
            }
            m += material_of(e.type).weight;
            ++k;
        }
        if (b.at.empty() || ++b.age > BODY_MAX_STEPS) {
            end_tipping(i, false);
            continue;
        }
        if (lost || m != b.mass) weigh_body(b, true);
        ++i;
    }

    // Contacts and forces for every body, then one solve over all of them, then
    // every body moves. Gathering can wake pieces it strikes, which appends them
    // to the pool; the loop reaches them too, since it runs to tip_count as it
    // stands.
    contacts.clear();
    for (int i = 0; i < tip_count; ++i) {
        tip_bodies[i].touching = tip_bodies[i].contact = false;
        gather_contacts(i);
        apply_body_forces(tip_bodies[i]);
    }
    solve_contacts();

    // Leaning wakes what it leans on. A body pressed sideways against a piece at
    // rest -- a domino that has gone over onto the next one, too slowly to count
    // as a blow -- pushes it, and only a body can be pushed; the piece joins next
    // step. Sideways only, the diagonal included: a body lying on a floor presses
    // down on it every step, and the floor has nowhere to go.
    for (size_t k = 0; k < contacts.size(); ++k) {
        const Contact& c = contacts[k];
        if (c.obstacle < 0 || c.acc_n < PUSH_WAKE) continue;
        if (abs64(c.nx) < abs64(c.ny)) continue;  // a diagonal is a lean too: corner on corner
        if (is_tipping(c.obstacle) || !collect_piece(c.obstacle, false)) continue;
        bool stands = false;
        for (const int q : balance_component) {
            const int below = q + width;
            if (below < width * height && balance_visit[below] != balance_epoch &&
                is_solid(cells[below].type)) {
                stands = true;
                break;
            }
        }
        if (stands) start_body(balance_component, 0, 0);
    }

    for (int i = 0; i < tip_count; ++i) {
        Body& b = tip_bodies[i];
        b.prev_cx = b.cx;
        b.prev_cy = b.cy;
        b.prev_theta = b.theta;
    }

    for (int i = 0; i < tip_count;) {
        Body& b = tip_bodies[i];

        // Turn about the contact bearing most of the load. A post tipping over a
        // ledge turns about the ledge's corner; turned about its middle, the shears
        // put its foot a cell out from where the motion has it, and that cell dips
        // below the ledge's top and jams against its face. The change is a pose
        // like any other -- the rounding moves from one end of the body to the
        // other -- so it is checked and kept only if it fits.
        //
        // Only while it is turning, or when where it turns now is touching
        // nothing. At rest the hardest-pressed contact of a body on two feet can
        // change from step to step on rounding alone, and each change moves the
        // rounding to the other foot: a cell of motion out of nothing, which is
        // enough to start a disc lying still rolling again. A post just starting
        // to tip is slow too, but turns about its middle, which touches nothing.
        const bool turning = abs64(b.omega) * b.reach > BODY_SLEEP_DRIFT / 8;
        if (b.pressed > 0 && (turning || !b.pivot_held)) {
            const fx::SinCos sc = fx::sincos(b.theta);
            const int64_t dx = b.press_x - b.cx, dy = b.press_y - b.cy;
            const int64_t lx = (dx * sc.c + dy * sc.s) / ONE + b.lcx;
            const int64_t ly = (-dx * sc.s + dy * sc.c) / ONE + b.lcy;
            const int nx = static_cast<int>(floor_div(lx + ONE / 2, ONE));
            const int ny = static_cast<int>(floor_div(ly + ONE / 2, ONE));
            if (nx != b.pivot_x || ny != b.pivot_y) {
                const int16_t was_x = b.pivot_x, was_y = b.pivot_y;
                b.pivot_x = static_cast<int16_t>(nx);
                b.pivot_y = static_cast<int16_t>(ny);
                // Stopped, it may only re-anchor where that moves nothing -- which
                // upright, at no angle, is always: every pivot gives the same cells.
                const bool fits =
                    tip_pose(b, b.cx, b.cy, b.theta) &&
                    (turning ? tip_move(b)
                             : std::equal(tip_next.begin(), tip_next.end(), b.at.begin()));
                if (!fits) {
                    b.pivot_x = was_x;
                    b.pivot_y = was_y;
                }
            }
        }
        // A hard landing loads it many times its own weight for a moment, and
        // that is when an overhang snaps. Asked before it moves, while it is
        // still touching what it landed on; after, the bounce has lifted it clear.
        if (b.pressed > 2 * BODY_GRAVITY)
            snap_overhang(i, std::min<int64_t>(divq(b.pressed, BODY_GRAVITY), IMPACT_LOAD_MAX));

        advance_body(b);

        // Stopped? Held by something -- a contact, or the liquid it floats in --
        // and gone nowhere for BODY_SLEEP_STEPS: within half a cell anywhere on it
        // of where it was when the count began. Measured as drift over a window
        // rather than as speed, because a body leaning on another, or wedged in a
        // corner, can rock a cell back and forth for ever at a speed that never
        // looks like stopping, while going nowhere at all. A topple that starts
        // from the very edge of balance is slow too, but it drifts further every
        // step, and is past half a cell well inside the window.
        const int64_t drift = abs64(b.cx - b.rest_cx) + abs64(b.cy - b.rest_cy) +
                              abs64(b.theta - b.rest_theta) * b.reach;
        if (b.touching && drift < (b.contact ? BODY_SLEEP_DRIFT : FLOAT_SLEEP_DRIFT)) {
            ++b.still;
        } else {
            b.still = 0;
            b.rest_cx = b.cx;
            b.rest_cy = b.cy;
            b.rest_theta = b.theta;
        }
        // Coming to rest, it is asked once more under its own weight alone: a
        // plank left sticking out too far over a ledge goes, gently.
        if (b.still >= BODY_SLEEP_STEPS && snap_overhang(i, ONE)) b.still = 0;
        if (b.still >= BODY_SLEEP_STEPS) {
            // What it rests on is weighed again only if it went somewhere. One
            // that has rocked in place since it started is where support last
            // left it, on what was already carrying it.
            const int64_t travel = abs64(b.cx - b.start_cx) + abs64(b.cy - b.start_cy) +
                                   abs64(b.theta - b.start_theta) * b.reach;
            square_body(b);
            end_tipping(i, b.moved && travel >= ONE);
            continue;
        }
        ++i;
    }

    resolving_support = false;
}

void Grid::end_tipping(int i, bool requeue) {
    Body& b = tip_bodies[i];
    tag_body[b.tag] = -1;

    if (requeue) {
        if (++tip_epoch == 0) {
            std::fill(tip_visit.begin(), tip_visit.end(), uint8_t{0});
            tip_epoch = 1;
        }
        for (const int idx : b.at) tip_visit[idx] = tip_epoch;
    }

    for (const int idx : b.at) {
        // A body that started mid-fall may have had cells sitting in the deferred
        // queue, which ages them; whatever happens next starts from rest.
        if (is_structural(cells[idx].type) && cells[idx].piece_tag == b.tag) cells[idx].ticks = 0;
        if (requeue) {
            // Not the body itself: it stopped touching something, so it is held up
            // -- the solver has just shown as much, rolling resistance and friction
            // included, which the support rules' centre-over-footprint test cannot
            // see. Asking them would only start a disc lying on its curve rolling
            // again, a little, under a new tag, every time it stopped. What it has
            // come to rest on is another matter: that now carries it, and a block
            // set down on the end of a plank is weighed on that plank.
            const int cy = idx / width;
            const int cx = idx - cy * width;
            mark_dirty(cx, cy);
            const int below = idx + width;
            if (below < width * height && tip_visit[below] != tip_epoch)
                queue_support_check(cx, cy + 1);
        }
    }

    // Swap-remove. The order bodies are stepped in changes when one ends, but it
    // changes the same way every run, which is all determinism asks.
    --tip_count;
    if (i != tip_count) {
        std::swap(tip_bodies[i], tip_bodies[tip_count]);
        tag_body[tip_bodies[i].tag] = static_cast<int16_t>(i);
    }
}

Grid::BodyState Grid::body_state(int i) const {
    const Body& b = tip_bodies[i];
    BodyState out;
    out.x = static_cast<fx::v>(b.cx);
    out.y = static_cast<fx::v>(b.cy);
    out.vx = static_cast<fx::v>(b.vx);
    out.vy = static_cast<fx::v>(b.vy);
    out.theta = b.theta;
    out.omega = static_cast<fx::v>(b.omega);
    out.cells = static_cast<int>(b.at.size());
    out.mass = b.mass;
    out.age = b.age;
    out.prev_x = static_cast<fx::v>(b.prev_cx);
    out.prev_y = static_cast<fx::v>(b.prev_cy);
    out.prev_theta = b.prev_theta;
    out.shape_cx = static_cast<fx::v>(b.lcx);
    out.shape_cy = static_cast<fx::v>(b.lcy);
    out.steps_to_sleep = std::max(BODY_SLEEP_STEPS - b.still, 1);
    int64_t rest_x = 0, rest_y = 0;
    out.squares = square_pose(b, rest_x, rest_y, out.rest_theta);
    out.rest_x = static_cast<fx::v>(rest_x);
    out.rest_y = static_cast<fx::v>(rest_y);
    return out;
}

const std::vector<Grid::BodyCell>& Grid::body_shape(int i) const { return tip_bodies[i].shape; }
const std::vector<int>& Grid::body_cells(int i) const { return tip_bodies[i].at; }
