#include "grid.h"
#include "reaction.h"
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>

namespace {
    int clamp_channel(int v) {
        return v < 0 ? 0 : (v > 255 ? 255 : v);
    }

    // How much heat moves from `a` to `b` in one step, given the rate the pair
    // conducts at. Negative flows the other way. Three properties, one line each:
    //
    //  - A dead band. Two things within a degree of each other exchange nothing.
    //    Without it a pair would trade a unit back and forth forever and no chunk
    //    containing anything warm could ever sleep.
    //  - A floor of one unit. Integer division truncates toward zero, so a slow
    //    conductor across a small difference would compute a flow of zero and heat
    //    would stop partway. One unit is the smallest step this representation has.
    //  - A ceiling of half the difference, so the exchange never overshoots and a
    //    hot cell and a cold one cannot swap places and oscillate.
    int heat_flow(int a, int b, int rate, int divisor) {
        const int delta = a - b;
        if (delta > -2 && delta < 2) return 0; // dead band
        if (rate <= 0) return 0;

        int flow = delta * rate / divisor;
        const int half = delta / 2;  // toward zero, and |half| >= 1 given the band
        if (delta > 0) {
            if (flow < 1) flow = 1;
            if (flow > half) flow = half;
        } else {
            if (flow > -1) flow = -1;
            if (flow < half) flow = half;
        }
        return flow;
    }

    // Which materials appear as the target of any row in REACTIONS. try_react's two
    // loops only ever act on rows whose target is the cell's own type, so for a
    // material no row names they are a full walk of the table that can do nothing
    // -- and Sand, the commonest moving material, is one. Built from the table at
    // compile time so a new row is picked up without anyone remembering this.
    struct ReactionTargets {
        bool is_target[static_cast<int>(ElementType::Count)] = {};
    };
    constexpr ReactionTargets make_reaction_targets() {
        ReactionTargets t{};
        for (const Reaction& r : REACTIONS) t.is_target[static_cast<int>(r.target)] = true;
        return t;
    }
    constexpr ReactionTargets REACTION_TARGETS = make_reaction_targets();
}

Grid::Grid(int width, int height, uint64_t seed) : width(width), height(height), world_seed(seed) {
    cells.resize(width * height, Element{});
    pixels.resize(width * height, material_of(ElementType::Empty).color);

    chunks_x = (width + CHUNK_SIZE - 1) / CHUNK_SIZE;
    chunks_y = (height + CHUNK_SIZE - 1) / CHUNK_SIZE;
    chunk_current.resize(chunks_x * chunks_y);
    chunk_next.resize(chunks_x * chunks_y);
    chunk_warm.assign(static_cast<size_t>(chunks_x) * chunks_y, 0);
    // Both start empty: a world of nothing but Empty has nothing to simulate.

    support_visit.resize(width * height, 0);
    support_state.resize(width * height, 0);
    fracture_visit.resize(width * height, 0);
    level_visit.resize(width * height, 0);
    balance_visit.resize(width * height, 0);
    tip_visit.resize(width * height, 0);

    // The step loop allocates nothing in steady state: every scratch vector below
    // grows by push_back and is only ever .clear()ed, so its capacity survives
    // from step to step. Reserving takes the growth out of the step entirely for
    // every one with a hard bound -- without it, the first collapse or pressure
    // probe to reach a new high-water mark reallocates mid-step, on the step with
    // the most work in it. Each is reserved to that bound, not a guess:
    //
    //   support_component, drop_keys -- capped by the `> MAX_SUPPORT_CELLS` check
    //     in fall_if_unsupported, which runs after the push, so one past the cap.
    //   support_stack, balance_stack -- not capped by those checks at all. Every
    //     cell popped pushes up to eight neighbours, and pops stop pushing once the
    //     component passes the cap, so a stack never holds more than eight per
    //     judged cell plus the seed.
    //   balance_component, tip_next, tip_carry, tip_displaced -- the balance check
    //     gives up one past MAX_SUPPORT_CELLS, and a tipping body is a balance
    //     component, so every one of these is bounded by it. (Each TipBody's own
    //     two vectors are reserved the same way when the pool grows; see
    //     start_tipping.)
    //   fracture_component -- its cap is checked once per cell dequeued, before
    //     that cell's up-to-eight neighbours are pushed, so it can pass the cap by
    //     eight.
    //   fracture_lowest -- one entry per column a piece spans, so the world's width.
    //
    // Three have no bound short of the world itself, and are left to grow to their
    // high-water mark and stay there: pending_support and the two resolve_support
    // queues hold one entry per structure cell disturbed this step, which a large
    // enough collapse makes as big as it likes. The tip_bodies pool likewise holds
    // as many bodies as are tipping at once. tests/test_alloc.cpp holds the line
    // that matters -- a world replaying a step it has already reached allocates
    // nothing.
    support_component.reserve(MAX_SUPPORT_CELLS + 1);
    drop_keys.reserve(MAX_SUPPORT_CELLS + 1);
    support_stack.reserve(8 * MAX_SUPPORT_CELLS + 1);
    balance_component.reserve(MAX_SUPPORT_CELLS + 1);
    balance_stack.reserve(8 * MAX_SUPPORT_CELLS + 1);
    tip_next.reserve(MAX_SUPPORT_CELLS + 1);
    tip_carry.reserve(MAX_SUPPORT_CELLS + 1);
    tip_displaced.reserve(MAX_SUPPORT_CELLS + 1);
    fracture_component.reserve(MAX_SUPPORT_CELLS + 9);
    fracture_lowest.reserve(static_cast<size_t>(width));
    level_queue.reserve(LIQUID_LEVEL_CELLS + 4);

    // Nothing to seed. The seed is stored and read straight out of world_seed by
    // the hash in random.h, so the whole 64 bits reach the work by construction.
}

void Grid::reset(uint64_t seed) {
    world_seed = seed;

    std::fill(cells.begin(), cells.end(), Element{});
    std::fill(pixels.begin(), pixels.end(), material_of(ElementType::Empty).color);

    // Both go back to empty, same as a fresh grid's -- see the comment on these two
    // members for why there are two rather than one.
    std::fill(chunk_current.begin(), chunk_current.end(), DirtyRect{});
    std::fill(chunk_next.begin(), chunk_next.end(), DirtyRect{});
    std::fill(chunk_warm.begin(), chunk_warm.end(), uint8_t{0});
    sweep_chunk = -1;
    sweep_warm = true;

    pending_support.clear();
    support_stack.clear();
    support_component.clear();
    support_seeds.clear();
    support_deferred.clear();
    std::fill(support_visit.begin(), support_visit.end(), uint8_t{0});
    std::fill(support_state.begin(), support_state.end(), uint8_t{0});
    support_epoch = 0;
    resolving_support = false;

    std::fill(fracture_visit.begin(), fracture_visit.end(), uint8_t{0});
    fracture_epoch = 0;

    fracture_component.clear();
    fracture_lowest.clear();
    next_piece_tag = 1;

    // The pool keeps its entries, and each entry its capacity -- the same "cleared,
    // not freed" rule as every other scratch vector here. tip_count is what says
    // none of them is live.
    tip_count = 0;
    std::fill(std::begin(tip_tag_live), std::end(tip_tag_live), false);
    balance_component.clear();
    balance_stack.clear();
    tip_next.clear();
    tip_carry.clear();
    tip_displaced.clear();
    std::fill(balance_visit.begin(), balance_visit.end(), uint8_t{0});
    balance_epoch = 0;
    balance_pass_start = 1;
    std::fill(tip_visit.begin(), tip_visit.end(), uint8_t{0});
    tip_epoch = 0;

    frame_tag = 0;
    step_count = 0;

    // width, height, chunks_x and chunks_y are not here on purpose: the vectors
    // above are cleared in place rather than resized, which is only correct as long
    // as the grid's dimensions never change out from under them. Nothing in this
    // class exposes a way to change them after construction, and reset() must not
    // become the first.
}

// Waking only the cell that changed is the classic dirty-rect bug: erase a
// grain from under a settled pile and the grains above it are still asleep, so
// the pile hangs in the air over the hole. Every write therefore wakes its 3x3
// neighbourhood, and because the neighbourhood is resolved per cell it crosses
// chunk borders correctly -- otherwise the same bug reappears as seams along
// the invisible chunk lines.
//
// The 3x3 is applied a chunk at a time rather than a cell at a time, and the
// two are the same thing: a chunk's rect is a bounding box, so including nine
// cells one by one leaves it covering exactly the part of the 3x3 that falls
// inside that chunk -- which is what including that part as one box does. The
// set of cells woken is identical; only the cost changes. That cost was the
// largest single item in the step: every swap marks both of its ends, so a
// moving grain paid eighteen bounds checks, chunk divisions and rect updates,
// and in `cascading` this function alone was over 60% of Grid::update.
//
// Almost every 3x3 lies inside one chunk -- only cells on a chunk's outer ring
// straddle a border -- so that case gets a path of its own, and the general
// loop below it handles the up-to-four chunks a corner cell can touch.
void Grid::mark_dirty(int x, int y) {
    // That path, asked first and as cheaply as it can be: a cell inside the
    // world's edge and off its chunk's edge rows and columns has its whole 3x3
    // in the world and in one chunk, so there is nothing to clip, and each pair
    // of bounds is one unsigned compare. The general path below gives the same
    // box for it; this one only skips the clipping and the signed divisions.
    // Measured, it took about 6% off `churning` and 7% off `cascading`, where
    // marking is a large part of every step.
    static_assert((CHUNK_SIZE & (CHUNK_SIZE - 1)) == 0, "the fast path masks by CHUNK_SIZE");
    if (static_cast<unsigned>(x - 1) < static_cast<unsigned>(width - 2) &&
        static_cast<unsigned>(y - 1) < static_cast<unsigned>(height - 2) &&
        static_cast<unsigned>((x & (CHUNK_SIZE - 1)) - 1) < static_cast<unsigned>(CHUNK_SIZE - 2) &&
        static_cast<unsigned>((y & (CHUNK_SIZE - 1)) - 1) < static_cast<unsigned>(CHUNK_SIZE - 2)) {
        const int chunk = (y / CHUNK_SIZE) * chunks_x + x / CHUNK_SIZE;
        chunk_next[chunk].include_box(x - 1, y - 1, x + 1, y + 1);
        return;
    }

    // Clip to the world first, so everything below works on cells that exist.
    // Non-negative from here on, which is also what makes the divisions by
    // CHUNK_SIZE plain floors.
    const int x0 = x - 1 > 0 ? x - 1 : 0;
    const int y0 = y - 1 > 0 ? y - 1 : 0;
    const int x1 = x + 1 < width - 1 ? x + 1 : width - 1;
    const int y1 = y + 1 < height - 1 ? y + 1 : height - 1;
    if (x0 > x1 || y0 > y1) return;  // the whole 3x3 is outside the world

    const int cx0 = x0 / CHUNK_SIZE, cx1 = x1 / CHUNK_SIZE;
    const int cy0 = y0 / CHUNK_SIZE, cy1 = y1 / CHUNK_SIZE;

    if (cx0 == cx1 && cy0 == cy1) {
        chunk_next[cy0 * chunks_x + cx0].include_box(x0, y0, x1, y1);
        return;
    }

    for (int cy = cy0; cy <= cy1; ++cy) {
        const int by0 = std::max(y0, cy * CHUNK_SIZE);
        const int by1 = std::min(y1, cy * CHUNK_SIZE + CHUNK_SIZE - 1);
        for (int cx = cx0; cx <= cx1; ++cx) {
            const int bx0 = std::max(x0, cx * CHUNK_SIZE);
            const int bx1 = std::min(x1, cx * CHUNK_SIZE + CHUNK_SIZE - 1);
            chunk_next[cy * chunks_x + cx].include_box(bx0, by0, bx1, by1);
        }
    }
}

void Grid::note_warm(int x, int y) {
    const int x0 = x - 1 > 0 ? x - 1 : 0;
    const int y0 = y - 1 > 0 ? y - 1 : 0;
    const int x1 = x + 1 < width - 1 ? x + 1 : width - 1;
    const int y1 = y + 1 < height - 1 ? y + 1 : height - 1;
    if (x0 > x1 || y0 > y1) return;
    for (int cy = y0 / CHUNK_SIZE; cy <= y1 / CHUNK_SIZE; ++cy) {
        for (int cx = x0 / CHUNK_SIZE; cx <= x1 / CHUNK_SIZE; ++cx) {
            const int c = cy * chunks_x + cx;
            chunk_warm[static_cast<size_t>(c)] = 1;
            // Keeps the sweep's cached flag true to the array when the chunk it is
            // walking turns warm partway through.
            if (c == sweep_chunk) sweep_warm = true;
        }
    }
}

int Grid::active_chunk_count() const {
    int n = 0;
    for (const DirtyRect& r : chunk_next) {
        if (!r.is_empty()) n++;
    }
    return n;
}

bool Grid::chunk_awake_at(int x, int y) const {
    if (!is_within_bounds(x, y)) return false;
    return !chunk_next[(y / CHUNK_SIZE) * chunks_x + (x / CHUNK_SIZE)].is_empty();
}

bool Grid::is_within_bounds(int x, int y) const {
    return x >= 0 && x < width && y >= 0 && y < height;
}

int Grid::get_index(int x, int y) const {
    return y * width + x;
}

Element Grid::get_element(int x, int y) const {
    // Out of bounds reads as solid so the world is sealed by its own border.
    if (!is_within_bounds(x, y)) {
        Element border;
        border.type = ElementType::Wall;
        border.color = material_of(ElementType::Wall).color;
        return border;
    }
    return cells[get_index(x, y)];
}

// The shade is a function of the cell's position and nothing else, so a cell
// erased and repainted in the same spot comes back the same colour. A one-time
// authored value has no business moving every time the world is touched, and
// pinning it to the spot makes the look of a scene reproducible from the seed
// alone. If it ever reads as too regular under a slow brush, mix a placement
// counter into the index rather than reaching for a generator.
//
// Cells carry their colour when they move, because swap_elements moves the
// whole Element, so a falling grain keeps the shade it was painted with.
uint32_t Grid::jittered_color(const Material& mat, uint64_t index) const {
    if (mat.color_jitter == 0) return mat.color;

    const int j = static_cast<int>(mat.color_jitter);
    const int delta = authored_spread(j, index, sim_random::Stream::ColorJitter);

    const int r = clamp_channel(static_cast<int>((mat.color >> 16) & 0xFF) + delta);
    const int g = clamp_channel(static_cast<int>((mat.color >> 8) & 0xFF) + delta);
    const int b = clamp_channel(static_cast<int>(mat.color & 0xFF) + delta);

    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

void Grid::set_element(int x, int y, ElementType type) {
    if (!is_within_bounds(x, y)) return;
    const int idx = get_index(x, y);
    place(x, y, type, jittered_color(material_of(type), static_cast<uint64_t>(idx)));
}

void Grid::paint(int x, int y, ElementType type, uint32_t color) {
    if (!is_within_bounds(x, y)) return;
    place(x, y, type, color);
}

void Grid::displace(int x, int y, ElementType type) {
    if (!is_within_bounds(x, y)) return;
    const ElementType old_type = cells[get_index(x, y)].type;

    // Four cases pass straight through to a plain write, each one a case where
    // there is nothing to conserve:
    //
    //  - the eraser (`type` is Empty). Deleting is the whole point of it.
    //  - an empty cell. Nothing to move.
    //  - painting a material onto itself, which is most of a drag stroke. Without
    //    it, every step of a stroke would lift the cell it just painted and stack a
    //    column of its own material above the cursor.
    //  - a static occupant. These do not move under their own physics and it would
    //    read as a bug if they moved under the brush.
    if (type != ElementType::Empty && old_type != ElementType::Empty &&
        old_type != type && material_of(old_type).move != MoveKind::Static) {
        make_room_above(x, y);
    }

    set_element(x, y, type);
}

bool Grid::make_room_above(int x, int y) {
    for (int step = 1; step <= MAX_DISPLACE_RISE; ++step) {
        const int ny = y - step;
        if (!is_within_bounds(x, ny)) return false;

        const ElementType t = cells[get_index(x, ny)].type;
        if (t == ElementType::Empty) {
            // A swap and not a copy, for the same reason every move in this file is a
            // swap: it is the only write that cannot change how much of anything
            // exists. The Empty comes down to (x, y) and the caller writes over it.
            swap_elements(x, y, x, ny);
            return true;
        }
        if (material_of(t).move == MoveKind::Static) return false;
    }
    return false;
}

// Shared by set_element and paint, which differ only in where the colour comes
// from -- a jittered draw from the material table versus one the caller names
// outright. Everything downstream of "here is the colour" has to stay identical
// between the two, or authored terrain and brush-placed terrain would behave
// differently the moment someone touched one write path without the other.
void Grid::place(int x, int y, ElementType type, uint32_t color) {
    const int idx = get_index(x, y);
    const ElementType old_type = cells[idx].type;

    Element el;
    el.type = type;
    el.color = color;
    el.updated_tag = frame_tag;  // freshly placed cells wait until the next step to move
    // A liquid or steam cell placed by anything but a flow starts full: the
    // brush, a scene, a reaction. Reactions that turn one carrier of an amount
    // into another hand it across themselves (try_react, step_steam).
    if (carries_mass(type)) el.mass = LIQUID_FULL;

    // Heat belongs to the spot rather than to what is standing in it, so a cell
    // written into a hot region arrives hot and a reaction product keeps the
    // temperature that caused it. Materials that are hot by definition say so in
    // the table and override this; Empty names ambient there, which is what makes
    // erasing a cell also clear its heat.
    const uint8_t spawn = material_of(type).spawn_temperature;
    el.temperature = spawn != 0 ? spawn : cells[idx].temperature;

    // `ticks` and `piece_tag` are reset here: `el` is a fresh Element, so every
    // field not named above goes back to its default, and only `temperature` argues
    // for itself. A cell written into a broken piece therefore does not join it --
    // it becomes a piece of its own with tag 0, so patching a crack with the brush
    // leaves a seam the support fill can see and nobody else can.
    //
    // A reaction is not a new cell, though, and Wood -> Charred turns one
    // structural material into another. try_react decides that one: it carries
    // the tag and the fall clock across, so a burning cell stays in its piece.
    // The reset here stays right for the brush, which is what the seam is for.

    cells[idx] = el;
    pixels[idx] = pixel_of(el);
    mark_dirty(x, y);
    note_warm_if(x, y, el.temperature);

    // Structure was removed here, so anything leaning on it has to be re-examined.
    // Removal only: placing a new structure cell does not trigger a check, which is
    // what lets the brush draw a floating platform on purpose.
    if (!resolving_support && is_structural(old_type) && !is_structural(type)) {
        for (int ny = y - 1; ny <= y + 1; ++ny)
            for (int nx = x - 1; nx <= x + 1; ++nx)
                queue_support_check(nx, ny);
    }
}

void Grid::swap_elements(int x1, int y1, int x2, int y2) {
    const int idx1 = get_index(x1, y1);
    const int idx2 = get_index(x2, y2);

    std::swap(cells[idx1], cells[idx2]);
    pixels[idx1] = pixel_of(cells[idx1]);
    pixels[idx2] = pixel_of(cells[idx2]);

    // Mark as updated so neither cell moves again this step.
    cells[idx1].updated_tag = frame_tag;
    cells[idx2].updated_tag = frame_tag;

    // Both ends of the move changed, so both ends and their neighbours have to be
    // awake next step -- this is what lets a falling grain keep falling and what
    // pulls the cells above it into motion behind it.
    mark_dirty(x1, y1);
    mark_dirty(x2, y2);

    // Temperature travels with the cell, so a warm cell arriving somewhere new
    // makes that somewhere warm.
    note_warm_if(x1, y1, cells[idx1].temperature);
    note_warm_if(x2, y2, cells[idx2].temperature);

    // The other way structure loses its footing: nothing was removed, but the sand
    // that was holding a slab up just slid out from under it. Only the cell directly
    // above each end can have been standing on what moved.
    //
    // This is the hottest path in the engine, so the check is written to be cheap;
    // it does not register on a bracketed A/B.
    if (!resolving_support) {
        queue_support_check(x1, y1 - 1);
        queue_support_check(x2, y2 - 1);
    }
}

bool Grid::is_grounded(int x, int y) const {
    const int below_y = y + 1;

    // The bottom of the world holds everything up. The side borders deliberately do
    // not: a shelf bolted to the left edge with nothing beneath it is unsupported,
    // same as anywhere else.
    if (below_y >= height) return true;

    const Element& under = cells[get_index(x, below_y)];

    // More of the same structure is not support -- whether it is held up is the
    // question the flood fill is already answering.
    //
    // "The same structure" means the same piece, not merely the same kind of
    // material. A fragment that has broken off and come to rest on the slab it
    // broke away from is standing on something, exactly as it would be on unrelated
    // masonry, and the fill will never reach across the crack to discover
    // otherwise. Without this, a heap of broken pieces would be one
    // mutually-unsupported tower that sinks through itself.
    if (is_structural(under.type) && under.piece_tag == cells[get_index(x, y)].piece_tag) return false;

    return is_solid(under.type);
}

void Grid::queue_support_check(int x, int y) {
    if (!is_within_bounds(x, y)) return;
    const int idx = get_index(x, y);
    if (!is_structural(cells[idx].type)) return;
    pending_support.push_back(idx);
}

int Grid::fall_speed(uint8_t ticks) {
    const int s = 1 + static_cast<int>(ticks) / TICKS_PER_SPEEDUP;
    return s < MAX_FALL_SPEED ? s : MAX_FALL_SPEED;
}

void Grid::resolve_support() {
    if (pending_support.empty()) return;

    resolving_support = true;

    // Persistent scratch (see grid.h): cleared here, not reallocated. `deferred`
    // still accumulates across the passes below.
    support_seeds.clear();
    support_deferred.clear();

    // One pass per cell of travel. Everything queued gets the first pass; each pass
    // after that is only for the pieces that have been in the air long enough to
    // have earned it. Re-running the whole question rather than taking a longer
    // stride is what keeps a piece at speed 8 from stepping over a one-cell floor.
    for (int pass = 0; pass < MAX_FALL_SPEED && !pending_support.empty(); ++pass) {
        // Taken by value: a piece that falls re-queues itself into pending_support,
        // and that must not extend the loop running now, or one piece would fall the
        // whole way down inside a single pass.
        support_seeds.clear();
        support_seeds.swap(pending_support);

        // A fresh epoch per pass. Every cell of travel is a new question about a
        // world that has just changed, so last pass's verdicts must not be read as
        // answers to this one.
        if (++support_epoch == 0) {  // wrapped, so old marks can no longer be told apart
            std::fill(support_visit.begin(), support_visit.end(), uint8_t{0});
            support_epoch = 1;
        }

        // Fracture's own visited marks, advanced with the pass rather than per call,
        // so one landing costs one fill instead of one per seed. A slab lands with
        // every cell of it queued, and each seed would otherwise re-walk the whole
        // piece to answer a question the first one already answered.
        if (++fracture_epoch == 0) {
            std::fill(fracture_visit.begin(), fracture_visit.end(), uint8_t{0});
            fracture_epoch = 1;
        }
        // Balance is stamped per check rather than per pass (see balance_visit),
        // so a pass only marks where its own stamps begin.
        balance_pass_start = static_cast<uint8_t>(balance_epoch + 1);
        if (balance_pass_start == 0) {
            std::fill(balance_visit.begin(), balance_visit.end(), uint8_t{0});
            balance_epoch = 0;
            balance_pass_start = 1;
        }

        for (const int seed : support_seeds) {
            if (!is_structural(cells[seed].type)) continue;
            if (support_visit[seed] == support_epoch) continue;  // its piece already moved this pass

            // Part of a piece that is tipping over. step_tipping() owns it until it
            // stops, and re-queues every cell of it then; asking the fill about it
            // now would have one system drop a piece the other is rotating.
            if (is_tipping(seed)) continue;

            // Too slow for this pass. It keeps its place in the queue so the next step
            // picks it up again; it just does not travel any further this one.
            if (fall_speed(cells[seed].ticks) <= pass) {
                support_deferred.push_back(seed);
                continue;
            }

            const int y = seed / width;
            fall_if_unsupported(seed - y * width, y);
        }
    }

    for (const int idx : support_deferred) pending_support.push_back(idx);

    // Whatever is still queued moved at some point during this step, so it is in
    // the air and has now been for one step longer. A tick is a step, not a cell:
    // speed has to follow time in the air, or a piece would speed up because it was
    // already fast. Pieces that came to rest are not here -- settle_marks() put them
    // back to zero and stopped re-queueing them.
    for (const int idx : pending_support) {
        if (cells[idx].ticks < 255) cells[idx].ticks++;
    }

    resolving_support = false;
}

void Grid::settle_marks(SupportState state, int extra) {
    const uint8_t s = static_cast<uint8_t>(state);

    // Settling on Supported is the only way a piece ever stops falling, so it is
    // also where the clock goes back to zero. Without that, a slab that fell a long
    // way and landed would keep the speed it landed at, and digging it free a
    // minute later would have it leave at full pelt.
    //
    // Only Supported. A fill that adopts a neighbour's Moved verdict is part of a
    // piece still in the air and settles as Moved, keeping its clock (see the
    // adoption in fall_if_unsupported).
    const bool at_rest = (state == SupportState::Supported);

    // Every cell this fill marked is either already filed in the component or still
    // waiting on the stack; between them they are the whole marked set.
    const auto settle = [&](int idx) {
        support_state[idx] = s;
        if (at_rest) cells[idx].ticks = 0;
    };

    if (extra >= 0) settle(extra);
    for (const int idx : support_component) settle(idx);
    for (const int idx : support_stack) settle(idx);
}

void Grid::fall_if_unsupported(int x, int y) {
    support_stack.clear();
    support_component.clear();

    const int seed = get_index(x, y);
    // The piece being judged is the run of connected structure sharing this tag.
    // Everything below tests against it rather than against "is this structural",
    // which is what makes a crack a real boundary.
    const uint8_t tag = cells[seed].piece_tag;

    // Read now, because settle_marks zeroes ticks the moment this concludes.
    const bool was_falling = cells[seed].ticks >= FRACTURE_MIN_TICKS;
    const bool was_moving = cells[seed].ticks > 0;

    // Whether everything this fill has reached is the seed's own material. Kept
    // per push rather than per cell visited, so it covers the stack as well as the
    // trail -- a superset, which only ever errs towards asking the balance
    // question. See the landing below for what it buys.
    const ElementType seed_family = material_family(cells[seed].type);
    bool one_family = true;

    support_stack.push_back(seed);
    support_visit[seed] = support_epoch;
    support_state[seed] = static_cast<uint8_t>(SupportState::Pending);

    while (!support_stack.empty()) {
        const int idx = support_stack.back();
        support_stack.pop_back();

        const int cy = idx / width;
        const int cx = idx - cy * width;

        // One grounded cell anywhere is enough to hold the whole structure up.
        // Everything reached on the way here is part of that same piece, so it is
        // held up too -- recording that is what stops a later seed from re-deciding
        // the question with half the piece walled off from it.
        if (is_grounded(cx, cy)) {
            // It has landed. If it arrived with speed on it, this is the moment it
            // breaks -- before the marks are settled, so the fresh tags are in place
            // for the next step's fills, and before settle_marks zeroes the ticks
            // fracture reads.
            //
            // Seeded from where the fill started, not from the grounded cell it ended
            // at. is_grounded refuses to count more of the same structure as support,
            // so a slab coming to rest on a stone floor is not grounded where it
            // touches: the fill walks on down through the floor and answers
            // "grounded" at the bottom of the world, far away and stationary. Handing
            // that cell to fracture asks about the wrong piece, and fails quietly,
            // because the seed is at rest and nothing ever breaks.
            //
            // Held up -- but held up by what? A piece standing on one corner with
            // the rest of it hanging in the air is "supported" by the rule above
            // and is going over regardless, so balance is asked first, and a piece
            // that is going over does not also break. The two split the cases
            // between them: land with most of the weight over the ledge and the
            // overhang snaps off; land with most of it over the drop and the whole
            // thing tips off. Breaking first would split a tall post lengthwise
            // down its supported column, which no post has ever done. The halves
            // of a break are re-queued and get asked about balance on their own
            // next time round.
            //
            // At rest, one case is not asked at all: this fill came down to the
            // bottom of the world through nothing but the seed's own material.
            // topple_if_unbalanced would answer "standing" there anyway -- its
            // walk follows the same material downhill and gives up the moment it
            // reaches the world floor -- so asking would only repeat that walk,
            // which on a wide burning slab is a seed every few cells, every step.
            // This is that rule's cheap pre-check, not a second rule: it is
            // keyed on every cell the fill reached, so a wooden post standing on
            // stone that stands on the floor is still weighed. (An earlier copy
            // looked only at the cell the fill ended on, and skipped that post.)
            const bool down_own_material_to_floor = !was_moving && cy + 1 >= height && one_family;
            const bool tipped =
                !down_own_material_to_floor && topple_if_unbalanced(x, y, was_moving);
            if (!tipped && was_falling) fracture_landing(x, y);

            settle_marks(SupportState::Supported, idx);
            return;
        }

        support_component.push_back(idx);
        if (static_cast<int>(support_component.size()) > MAX_SUPPORT_CELLS) {
            settle_marks(SupportState::Supported, -1);  // too big to judge: assume held up
            return;
        }

        // Neighbours are pushed in reading order, so the last ones pushed -- and
        // therefore the first ones popped off the stack -- are the row below. The
        // search runs downhill, which is where the ground is.
        for (int ny = cy - 1; ny <= cy + 1; ++ny) {
            for (int nx = cx - 1; nx <= cx + 1; ++nx) {
                if (nx == cx && ny == cy) continue;
                if (!is_within_bounds(nx, ny)) continue;

                const int nidx = get_index(nx, ny);
                if (!is_structural(cells[nidx].type)) continue;
                if (cells[nidx].piece_tag != tag) continue;  // across a crack: a different piece

                if (support_visit[nidx] == support_epoch) {
                    if (static_cast<SupportState>(support_state[nidx]) == SupportState::Pending) {
                        continue;  // already in this fill
                    }
                    // Touching a cell an earlier fill already settled. The two are adjacent,
                    // so they are one piece, and its answer is this piece's answer:
                    // Supported means held up, and Moved means it has already had its turn
                    // this pass, so this half waits for the next one rather than falling on
                    // its own. Either way nothing here moves now.
                    //
                    // Held up still gets the balance question, though. The earlier
                    // fill that answered for this piece asked it about whatever
                    // part it started in, and balance is judged per material (see
                    // topple_if_unbalanced), so a wooden post on a stone floor is
                    // only ever weighed by a seed that is in the post.
                    //
                    // Only across a joint, though: if the settled neighbour is the
                    // same material, this is the same run of it, and the fill
                    // that settled it has nearly always asked already. When it
                    // has not -- its trail crossed in from another material --
                    // the cost is a piece that stays standing, the harmless way.
                    const auto verdict = static_cast<SupportState>(support_state[nidx]);
                    if (!was_moving && verdict == SupportState::Supported &&
                        material_family(cells[nidx].type) != material_family(cells[seed].type)) {
                        topple_if_unbalanced(x, y, false);
                    }
                    // Filed under the neighbour's own verdict, not always as held
                    // up. A Moved neighbour is a piece still falling, and this half
                    // is part of it from the next pass on -- the fill that runs from
                    // the cells it requeued walks straight into this one -- so it
                    // keeps its fall clock. Filing it Supported zeroed the clock of
                    // a piece in mid-air, and the joined piece then fell at the
                    // speed of a piece just come loose.
                    settle_marks(verdict, idx);
                    return;
                }

                support_visit[nidx] = support_epoch;
                support_state[nidx] = static_cast<uint8_t>(SupportState::Pending);
                support_stack.push_back(nidx);
                if (material_family(cells[nidx].type) != seed_family) one_family = false;
            }
        }
    }

    // The whole piece was explored and none of it was standing on anything.
    drop_component();
}

uint8_t Grid::alloc_piece_tag() {
    // pending_support is every cell currently asking to be re-checked, so a piece
    // still in flight has its cells in it. Scanning it is O(queue) per fracture,
    // the same order as the fill that just ran, and a fracture is rare next to a
    // step.
    for (int attempt = 0; attempt < 255; ++attempt) {
        const uint8_t candidate = next_piece_tag;
        next_piece_tag = static_cast<uint8_t>(next_piece_tag + 1);
        if (next_piece_tag == 0) next_piece_tag = 1;  // 0 means "never broken"

        // A tipping body is in the air too, but is not in pending_support while it
        // turns, so its tag has to be refused by name.
        bool in_use = tip_tag_live[candidate];
        for (const int idx : pending_support) {
            if (cells[idx].ticks > 0 && cells[idx].piece_tag == candidate) {
                in_use = true;
                break;
            }
        }
        if (!in_use) return candidate;
    }

    // All 255 tags are in the air at once. There is no non-colliding answer, so
    // give the counter's and let the two bodies merge -- the same outcome a bare
    // counter had, now only in the case that genuinely has no better one.
    return next_piece_tag;
}

void Grid::fracture_landing(int x, int y) {
    const int seed = get_index(x, y);
    const uint8_t tag = cells[seed].piece_tag;

    // Its own fill, and deliberately not support_visit's: the fill that called this
    // is mid-flight and its marks are load-bearing for the rest of the pass. The
    // epoch is advanced once per pass by resolve_support, not here, which is what
    // makes the early return work -- every other cell of a piece that has already
    // been through here is stamped, so a landing costs one fill however many of its
    // cells were queued.
    if (fracture_visit[seed] == fracture_epoch) return;

    fracture_component.clear();
    fracture_component.push_back(seed);
    fracture_visit[seed] = fracture_epoch;

    int min_x = x, max_x = x;

    for (size_t head = 0; head < fracture_component.size(); ++head) {
        const int idx = fracture_component[head];
        const int cy = idx / width;
        const int cx = idx - cy * width;

        if (cx < min_x) min_x = cx;
        if (cx > max_x) max_x = cx;

        // Too big to break is the same answer as too big to judge, and for the same
        // reason: when the question costs more than it is worth, guess the way that
        // leaves the level standing.
        if (static_cast<int>(fracture_component.size()) > MAX_SUPPORT_CELLS) return;

        for (int ny = cy - 1; ny <= cy + 1; ++ny) {
            for (int nx = cx - 1; nx <= cx + 1; ++nx) {
                if (nx == cx && ny == cy) continue;
                if (!is_within_bounds(nx, ny)) continue;
                const int nidx = get_index(nx, ny);
                if (!is_structural(cells[nidx].type)) continue;
                if (cells[nidx].piece_tag != tag) continue;
                // Still in flight. This is what identifies the piece that just landed,
                // and connectivity cannot: the instant a slab touches the floor the two
                // are one structural component, so a fill following structure alone
                // would walk out of the slab, across the entire floor, past
                // MAX_SUPPORT_CELLS, and give up -- which looks from the outside like
                // fracture simply not firing. ticks is zero for everything at rest and
                // non-zero for everything that has been moving, and it is read here
                // before settle_marks clears it.
                if (cells[nidx].ticks == 0) continue;
                if (fracture_visit[nidx] == fracture_epoch) continue;
                fracture_visit[nidx] = fracture_epoch;
                fracture_component.push_back(nidx);
            }
        }
    }

    if (static_cast<int>(fracture_component.size()) < MIN_FRACTURE_CELLS) return;

    // A seam needs a piece on both sides of it, so a piece under three cells wide
    // has nowhere to crack. The seam is drawn between columns rather than along
    // one, which is what makes fracture cost no matter: no cell is removed,
    // relabelled out of existence, or duplicated.
    const int span = max_x - min_x + 1;
    if (span < 3) return;

    // The crack goes where the support ends, and that is the whole rule.
    //
    // A break only does anything if it separates a part that is held up from a part
    // that is not. Put it anywhere else -- near the middle with a random offset,
    // say -- and both fragments still rest on the same ground, so nothing moves and
    // the only trace of the break is a tag nobody can see.
    //
    // Reading the ground instead splits along the stress: this is the piece's own
    // footprint, column by column, and the crack is the line between the columns
    // that landed on something and the columns that landed on nothing. It also
    // means a piece that lands flat on flat ground does not break at all -- there
    // is no boundary to break at -- which is what stops every routine landing
    // burning a tag out of the 255 there are.
    fracture_lowest.assign(static_cast<size_t>(span), -1);
    for (const int idx : fracture_component) {
        const int c = (idx - (idx / width) * width) - min_x;
        if (fracture_lowest[c] < 0 || idx > fracture_lowest[c]) fracture_lowest[c] = idx;  // larger index = lower row
    }

    // Scanning from the left is arbitrary but deterministic. A piece with several
    // boundaries breaks at one of them now and is asked again the next time a
    // fragment lands, which keeps one landing's work bounded.
    //
    // This asks a plainer question than is_grounded does. There, more of the same
    // structure is not support, because whether that is held up is what the fill is
    // working out. Here the piece has stopped and the only question is whether this
    // column arrived on top of anything -- the floor it just landed on counts, and
    // it is not part of the piece, because the piece is the set of cells that were
    // in flight.
    const auto landed_on_something = [&](int idx) {
        const int cy = idx / width;
        const int cx = idx - cy * width;
        if (cy + 1 >= height) return true;  // the bottom of the world
        return is_solid(cells[get_index(cx, cy + 1)].type);
    };

    int crack = -1;
    bool prev_grounded = false, have_prev = false;
    for (int c = 0; c < span; ++c) {
        if (fracture_lowest[c] < 0) continue;  // a gap in the footprint is not a boundary
        const bool g = landed_on_something(fracture_lowest[c]);
        if (have_prev && g != prev_grounded) { crack = min_x + c; break; }
        prev_grounded = g;
        have_prev = true;
    }
    if (crack < 0) return;  // it landed evenly: there is nothing to break

    // A cell of jitter either way, so a collapse does not read as a machine cut
    // exactly along the lip of the ledge every time.
    crack += sim_random::spread(1, world_seed, step_count,
                                static_cast<uint64_t>(seed), sim_random::Stream::Fracture);
    if (crack < min_x + 1) crack = min_x + 1;
    if (crack > max_x) crack = max_x;

    const uint8_t fresh = alloc_piece_tag();

    for (const int idx : fracture_component) {
        const int cy = idx / width;
        const int cx = idx - cy * width;
        if (cx >= crack) cells[idx].piece_tag = fresh;

        // Both halves are re-queued, not just the one that was relabelled. The
        // half that keeps its tag is usually the one now hanging over nothing --
        // it is the far side of the ledge that has to fall, and it is the side
        // the crack did not rename. Queuing only the renamed cells leaves the
        // overhang settled, unwoken and hanging in the air.
        queue_support_check(cx, cy);
        mark_dirty(cx, cy);
    }
}

void Grid::drop_component() {
    // Bottom-up within each column, so a cell is only ever moved into space its
    // lower neighbour has already left. Doing this per column also handles a column
    // containing two separate parts of the same piece -- an arch, say -- without
    // needing to find the runs explicitly.
    //
    // Sorted as precomputed keys rather than with a comparator that splits each
    // index into x and y: that was two divisions and two modulos per comparison,
    // ~49k comparisons for a component at the MAX_SUPPORT_CELLS cap. Here each index
    // is split once. The key orders by x ascending, then y descending (as H-1-y
    // ascending), and carries the index itself in the low 32 bits -- so keys are
    // unique, the order is exactly the old comparator's, and reading an index back
    // needs no division. 64 bits because x << 20 in an int already overflows at
    // x = 2048; 16 bits each for x and H-1-y hold any grid up to 65535 on a side.
    drop_keys.clear();
    for (const int idx : support_component) {
        const int cy = idx / width;
        const int cx = idx - cy * width;
        drop_keys.push_back((static_cast<uint64_t>(cx) << 48) |
                            (static_cast<uint64_t>(height - 1 - cy) << 32) |
                            static_cast<uint32_t>(idx));
    }
    std::sort(drop_keys.begin(), drop_keys.end());
    for (size_t k = 0; k < drop_keys.size(); ++k)
        support_component[k] = static_cast<int>(static_cast<uint32_t>(drop_keys[k]));

    for (const int idx : support_component) {
        const int cy = idx / width;
        const int cx = idx - cy * width;

        // Always legal. Unsupported means no cell of the piece has anything solid
        // under it, so every cell about to move is moving into Empty, into a
        // fluid, or into space another cell of the piece just left. Structural
        // materials are denser than every fluid, so the swap sends whatever was
        // below up to the top of the piece rather than deleting it -- which is
        // also why a slab sinks through water instead of resting on it.
        swap_elements(cx, cy, cx, cy + 1);

        // Mark and re-queue the cell's new home, so the piece is recognised as
        // already-moved for the rest of this pass and gets another look on the
        // next one. This is the only thing that makes it keep falling: the queue
        // it leaves behind is the input to whichever pass comes next.
        //
        // The vacated cell is marked too. Anything of this piece still above it
        // is about to move into it, and until this pass is over no other fill
        // should treat that space as a fresh question.
        const int moved = get_index(cx, cy + 1);
        support_visit[moved] = support_epoch;
        support_state[moved] = static_cast<uint8_t>(SupportState::Moved);
        support_visit[idx] = support_epoch;
        support_state[idx] = static_cast<uint8_t>(SupportState::Moved);
        pending_support.push_back(moved);
    }
}

bool Grid::can_displace(const Material& mover, int tx, int ty, int dy) const {
    if (!is_within_bounds(tx, ty)) return false;

    const Element& target = cells[get_index(tx, ty)];
    if (target.type == ElementType::Empty) return true;

    const Material& t = material_of(target.type);
    if (t.move == MoveKind::Static) return false;

    // Only swap through another material when gravity would sort them that way,
    // otherwise the two cells would trade places forever.
    if (dy > 0) return mover.density > t.density;  // sinking
    if (dy < 0) return mover.density < t.density;  // rising
    return false;  // sideways: Empty only
}

// A grain falls exactly one cell per step. Giving powder a speed that grows
// with time in the air does not smooth the motion -- a whole-cell mover on a
// fixed tick just takes longer jumps -- and it stratifies any continuously fed
// stream into sheets, because `ticks` is per cell and place() resets it, so a
// brush stamps fresh speed-1 grains on top of grains already at speed 2.

bool Grid::step_powder(int x, int y, const Material& mat) {
    const int idx = get_index(x, y);

    if (can_displace(mat, x, y + 1, 1)) {
        // A plain swap puts the liquid in the grain's old cell, one row up. That
        // is harmless for one grain and wrong for a stream: there is always
        // another grain above, so the exchange repeats every step and the water
        // is handed up the column without bound, arriving in open air tens of
        // cells over the pool.
        //
        // So the grain first presses the liquid it is sinking into into the
        // liquid around it (disperse_liquid), and pressure carries it from there
        // to the surface -- the whole pool rises, which is what pouring sand into
        // a pool does. Only what had nowhere to go rides up with the swap.
        if (is_liquid(cells[get_index(x, y + 1)].type)) disperse_liquid(x, y + 1);
        swap_elements(x, y, x, y + 1);
        return true;
    }

    // A grain that cannot fall rolls one cell diagonally, a cell a step like every
    // other powder move.
    //
    // swap_elements tags only the two cells it touches, so an entire row can cascade
    // diagonally inside one sweep -- grain at x to (x+1, y+1), grain at x+1 still
    // untagged and on to (x+2, y+1), all the way along. Every grain lands one row
    // down and one column over in the same step, while the row beneath was swept
    // earlier and has not caught up, leaving a one-cell shelf standing proud of the
    // pile with nothing under it.
    //
    // The roll is refused while the cell beside the grain is solid, which is what
    // the cascade relies on: each grain in it rolls past a neighbour that was still
    // standing beside it. A surface grain on a slope has open air beside it and
    // rolls as before, so settled grains still slump freely down a face.
    //
    // Two other fixes were tried. Refusing the roll unless its destination is
    // already supported removes the shelves but catches rest as well as motion,
    // and holds piles in vertical columns no powder would. Letting a rolled grain
    // take a second cell of fall in the same step hid the shelf, but made powder
    // fall twice as far on a diagonal as straight down. grid_test holds both
    // failures: shelves while pouring, and vertical faces once settled.
    const int dir = coin(static_cast<uint64_t>(idx), sim_random::Stream::PowderDirection) ? -1 : 1;

    for (const int d : {dir, -dir}) {
        const int nx = x + d;
        if (!can_displace(mat, nx, y + 1, 1)) continue;
        // A grain rolls past the corner only if the cell beside it is clear. The
        // cascade above needs the opposite: each grain in it rolls through the
        // spot its neighbour has only just left, past a cell the neighbour was
        // standing in when the row was set up. With the side checked, a row of
        // grains on a ledge rolls off one at a time from its open end instead of
        // all at once.
        if (is_solid(get_element(nx, y).type)) continue;

        if (is_liquid(cells[get_index(nx, y + 1)].type)) disperse_liquid(nx, y + 1);
        swap_elements(x, y, nx, y + 1);
        return true;
    }

    return false;
}

// A gas moves as whole cells: it rises, spreads sideways to find room, and swaps
// through anything heavier. Liquids used to share this path, with dy +1; they
// now move as an amount (step_liquid), and what is left here is the gas half,
// written against the direction gravity would pull it so it reads the same way.
bool Grid::step_gas(int x, int y, const Material& mat) {
    constexpr int dy = -1;
    if (can_displace(mat, x, y + dy, dy)) {
        swap_elements(x, y, x, y + dy);
        return true;
    }

    // Its own stream, separate from the powder pick. A cell is only ever one or the
    // other, so the two never collide in practice, but the tag costs one xor and
    // means neither function's behaviour depends on the other's existing.
    const int dir = coin(static_cast<uint64_t>(get_index(x, y)), sim_random::Stream::FluidDirection) ? -1 : 1;
    if (can_displace(mat, x + dir, y + dy, dy)) {
        swap_elements(x, y, x + dir, y + dy);
        return true;
    }
    if (can_displace(mat, x - dir, y + dy, dy)) {
        swap_elements(x, y, x - dir, y + dy);
        return true;
    }

    // Blocked from rising, so spread sideways, several cells a step, which is what
    // lets a pocket of steam fill the underside of a ceiling instead of standing
    // in a column.
    //
    // These rules were written when liquids shared this path, and each closes a
    // way a cell could move back and forth forever: a sideways move has to land
    // somewhere the cell can carry on from or rest on something solid
    // (can_rest_at), and it never crosses open air (over_void). A gas inherits
    // them unchanged; liquids now move as an amount (step_liquid) and need none.
    const bool on_solid = is_solid(get_element(x, y + 1).type);
    const auto can_rest_at = [&](int nx) {
        if (can_displace(mat, nx, y + 1, 1)) return true;  // it can carry on down
        return on_solid;  // otherwise only ever along a floor, never off one onto another
    };

    // Nothing may travel sideways across open air: `spread` is a distance along a
    // surface, and spent crossing empty space it teleports a cell to the far end
    // of the gap. Checked against y + dy, the side the cell would otherwise move
    // towards -- above, for a gas.
    const auto over_void = [&](int nx) {
        return get_element(nx, y + dy).type == ElementType::Empty;
    };

    for (const int d : {dir, -dir}) {
        int cx = x;
        int best = x;
        for (int i = 0; i < mat.spread; ++i) {
            const int nx = cx + d;
            if (!is_within_bounds(nx, y)) break;
            if (cells[get_index(nx, y)].type != ElementType::Empty) break;
            cx = nx;
            // Furthest usable landing, not merely furthest reachable: a cell may
            // pass over a stretch it could not stop on to get to one it can.
            if (can_rest_at(cx)) best = cx;

            // One cell of overhang is allowed and then the run stops: reaching a lip
            // and stepping past it is how a gas gets out from under an edge, and
            // past the lip there is nothing left for `spread` to measure.
            if (over_void(cx)) break;
        }
        if (best != x) {
            swap_elements(x, y, best, y);
            return true;
        }
    }

    return false;
}

int Grid::stable_lower(int total) { return total < LIQUID_FULL ? total : LIQUID_FULL; }

uint32_t Grid::pixel_of(const Element& e) {
    if (!is_liquid(e.type)) return e.color;
    const int m = e.mass;
    // Thin liquid lets the backdrop through; never fully, so a film still reads
    // as wet.
    constexpr int FLOOR_ALPHA = 48;
    const int alpha =
        m >= LIQUID_FULL ? 255 : FLOOR_ALPHA + (m * (255 - FLOOR_ALPHA)) / LIQUID_FULL;
    return (static_cast<uint32_t>(alpha) << 24) | (e.color & 0x00FFFFFFu);
}

void Grid::transfer_liquid(int fx, int fy, int tx, int ty, int amount) {
    if (amount <= 0) return;
    // Coordinates in, not indices: every caller has them, and two divisions by
    // the width to get them back were a measurable part of `churning`.
    const int from = fy * width + fx;
    const int to = ty * width + tx;
    Element& s = cells[from];
    Element& d = cells[to];
    if (d.type == ElementType::Empty) {
        // Liquid arriving in empty space: a new cell of it, carrying the
        // source's heat, in the colour its own spot would have. It arrived this
        // step, so it moves from the next -- otherwise an amount could be handed
        // along a whole row inside one sweep.
        const uint8_t heat = s.temperature;
        d = Element{};
        d.type = s.type;
        d.color = jittered_color(material_of(s.type), static_cast<uint64_t>(to));
        d.temperature = heat;
        d.updated_tag = frame_tag;
    }
    // Momentum is mass times velocity, and it moves with the mass. Two equal
    // velocities mix to the same velocity exactly, so the division -- the
    // dearest thing in a transfer -- is only done when they differ.
    const int dm = d.mass;
    if (d.flow != s.flow)
        d.flow = static_cast<int8_t>((d.flow * dm + s.flow * amount) / (dm + amount));
    d.mass = static_cast<uint16_t>(dm + amount);
    s.mass = static_cast<uint16_t>(s.mass - amount);
    if (s.mass == 0) {
        // Emptied: Empty, at ambient, the way erasing a cell leaves it.
        s = Element{};
        s.updated_tag = frame_tag;
    }
    pixels[from] = pixel_of(s);
    pixels[to] = pixel_of(d);
    mark_dirty(tx, ty);
    note_warm_if(tx, ty, d.temperature);
}

namespace {

// The cells disperse_liquid presses into, in the order it tries them: rings
// outward, nearest first, and only level with the cell or below it. Within a
// ring, row by row from the top, and along each row from -dx to +dx for a
// positive draw (mirrored for a negative one).
//
// A table rather than the three nested loops it is built from: this runs for
// every grain that sinks into liquid -- most of what `churning` is -- and the
// loops walked 49 (dx, dy) pairs to visit the 27 on a ring, every call.
struct RingOffset {
    int dx;
    int dy;
};
template <int R> struct DisperseRing {
    RingOffset at[2 * R * (R + 1) + R];
};
template <int R> constexpr DisperseRing<R> make_disperse_ring() {
    DisperseRing<R> ring{};
    int k = 0;
    for (int r = 1; r <= R; ++r)
        for (int dy = 0; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx)
                if (dy == r || dx == r || dx == -r) ring.at[k++] = {dx, dy};
    return ring;
}

}  // namespace

void Grid::disperse_liquid(int x, int y) {
    const int idx = get_index(x, y);
    const ElementType type = cells[idx].type;
    // Rings outward, nearest first, and only level with the cell or below it: the
    // cells above are the grain's own column, where water pressed in would ride up
    // with the next grain. Which side of a ring comes first is drawn per cell, so
    // a pour does not comb its water one way.
    //
    // The rings fill the block of SPAN x (R + 1) cells around and below the cell
    // exactly, so which candidates have room is gathered first, a bit each,
    // without a branch. In a band of sinking sand many calls find none -- the
    // water around is pressed full already; 43% of them in `churning` -- and
    // return there, before even the draw. Room cannot change during the walk: a
    // transfer changes only the source and the one candidate it lands in, and no
    // candidate is visited twice.
    //
    // The source is not woken here (transfer_liquid wakes only where liquid
    // lands): the grain's swap into it, which always follows, does that.
    constexpr int R = LIQUID_DISPERSE_RADIUS;
    constexpr int SPAN = 2 * R + 1;
    static_assert(SPAN * (R + 1) <= 32, "the open mask below is one bit a candidate");
    static constexpr DisperseRing<R> RING = make_disperse_ring<R>();
    constexpr int CELLS = static_cast<int>(sizeof(RING.at) / sizeof(RING.at[0]));

    uint32_t open = 0;  // bit dy * SPAN + dx + R
    if (x >= R && x < width - R && y + R < height) {
        // Room is "this liquid, holding under LIQUID_PUSH_CAP", and both live in a
        // cell's first four bytes -- type, flow, mass. With the cap a power of
        // two, one masked compare asks both: the type byte matches, and mass has
        // no bit set at or above the cap's. One load a candidate instead of two.
        static_assert(offsetof(Element, type) == 0 && offsetof(Element, mass) == 2 &&
                          sizeof(ElementType) == 1 && sizeof(Element::mass) == 2 &&
                          std::endian::native == std::endian::little,
                      "the room test reads type and mass as one little-endian word");
        static_assert((LIQUID_PUSH_CAP & (LIQUID_PUSH_CAP - 1)) == 0 && LIQUID_PUSH_CAP < 65536,
                      "the room test masks mass at LIQUID_PUSH_CAP");
        constexpr uint32_t KEY = 0xFFu | ((0xFFFFu & ~uint32_t{LIQUID_PUSH_CAP - 1}) << 16);
        const uint32_t want = static_cast<uint32_t>(type);
        const Element* row = &cells[static_cast<size_t>(idx - R)];
        for (int dy = 0; dy <= R; ++dy, row += width)
            for (int c = 0; c < SPAN; ++c) {
                uint32_t head;
                std::memcpy(&head, &row[c], sizeof(head));
                open |= static_cast<uint32_t>((head & KEY) == want) << (dy * SPAN + c);
            }
        open &= ~(1u << R);  // the cell itself
        if (open == 0) return;
    } else {
        for (int dy = 0; dy <= R; ++dy)
            for (int dx = -R; dx <= R; ++dx) {
                if (dx == 0 && dy == 0) continue;
                if (!is_within_bounds(x + dx, y + dy)) continue;
                const Element& e = cells[idx + dy * width + dx];
                open |= static_cast<uint32_t>((e.type == type) & (e.mass < LIQUID_PUSH_CAP))
                        << (dy * SPAN + dx + R);
            }
    }

    // The open candidates in the order the rings try them for this draw, bit k
    // for RING.at[k], walked lowest bit first.
    const int dir = coin(static_cast<uint64_t>(idx), sim_random::Stream::LiquidDisperse) ? -1 : 1;
    uint32_t ordered = 0;
    for (int k = 0; k < CELLS; ++k)
        ordered |= ((open >> (RING.at[k].dy * SPAN + RING.at[k].dx * dir + R)) & 1u) << k;
    while (ordered != 0) {
        const RingOffset& o = RING.at[std::countr_zero(ordered)];
        ordered &= ordered - 1;
        const int dx = o.dx * dir;
        const int room = LIQUID_PUSH_CAP - cells[idx + o.dy * width + dx].mass;
        const int have = cells[idx].mass;
        transfer_liquid(x, y, x + dx, y + o.dy, room < have ? room : have);
        if (room >= have) return;  // all of it has gone
    }
}

bool Grid::step_liquid(int x, int y, const Material& mat) {
    const int idx = get_index(x, y);
    const ElementType type = cells[idx].type;

    // Sinking through something lighter -- oil under water, or a gas -- is a
    // whole-cell swap, as every move between unlike materials is.
    if (y + 1 < height) {
        const ElementType below = cells[idx + width].type;
        if (below != ElementType::Empty && below != type && can_displace(mat, x, y + 1, 1)) {
            swap_elements(x, y, x, y + 1);
            return true;
        }
    }

    // How much of this liquid cell n holds, 0 for empty space it could flow into,
    // or -1 when n is anything else and takes none.
    const auto holds = [&](int n) -> int {
        const ElementType t = cells[n].type;
        if (t == type) return cells[n].mass;
        if (t == ElementType::Empty) return 0;
        return -1;
    };
    bool moved = false;
    // transfer_liquid wakes only where liquid lands; this cell is woken here, once
    // for everything that leaves it, rather than once a transfer.
    bool woken = false;

    // --- down: as much as the cell below will hold ---
    if (y + 1 < height) {
        const int b = idx + width;
        const int mb = holds(b);
        if (mb >= 0) {
            const int m = cells[idx].mass;
            int flow = stable_lower(m + mb) - mb;
            if (flow > m) flow = m;
            if (flow > 0) {
                transfer_liquid(x, y, x, y + 1, flow);
                moved = true;
                if (cells[idx].type != type) {  // all of it fell
                    mark_dirty(x, y);
                    return true;
                }
            }
        }
    }

    // --- sideways: the difference, and the momentum ---
    //
    // Amounts are seen only up to full. What a sinking grain presses into a cell
    // past full is the up flow's to carry to the surface; spread sideways it went
    // round and round a band of water capped by sand, every cell trading with
    // every neighbour every step, which kept the band awake and was most of what
    // sand sinking through water cost.
    {
        const int m = cells[idx].mass < LIQUID_FULL ? cells[idx].mass : LIQUID_FULL;
        // Dry ground beside a cell too thin to spread onto it is a wall to that
        // cell, for momentum as well as for flow: otherwise the empty side keeps
        // pushing a velocity the cell can never act on, and the edge of every
        // puddle stays awake for good.
        // Empty space over the same liquid is not dry ground but the body's own
        // surface, and a film spreads across it however thin: otherwise the last
        // part of a cell left on top of a pool stands as a patch of film a row
        // proud of it, for good.
        const auto side = [&](int n) {
            const int h = holds(n);
            if (h == 0 && m < LIQUID_MIN_SPREAD &&
                (n + width >= static_cast<int>(cells.size()) || cells[n + width].type != type))
                return -1;
            return h < LIQUID_FULL ? h : LIQUID_FULL;
        };
        const int ml = x > 0 ? side(idx - 1) : -1;
        const int mr = x + 1 < width ? side(idx + 1) : -1;

        // Momentum. Pushed by the difference in amount either side -- a wall
        // pushes back as hard as this cell does -- and damped every step, so it
        // dies away in a body at rest and is never stored where there is no flow.
        int grad = (ml < 0 ? m : ml) - (mr < 0 ? m : mr);
        if (grad > -LIQUID_DEAD && grad < LIQUID_DEAD) grad = 0;
        int v = cells[idx].flow * LIQUID_FLOW_SCALE + grad * LIQUID_ACCEL / 256;
        v = v * LIQUID_DAMP / 256;
        int vmax = mat.spread * LIQUID_SPEED_PER_SPREAD;
        vmax = vmax < 127 * LIQUID_FLOW_SCALE ? vmax : 127 * LIQUID_FLOW_SCALE;
        v = v > vmax ? vmax : (v < -vmax ? -vmax : v);
        if ((v > 0 && mr < 0) || (v < 0 && ml < 0)) v = 0;  // into a wall: stopped
        // Stored to the byte's resolution, rounded up or down at random in
        // proportion to the remainder, so the stored velocity is right on average.
        // Truncating instead threw away every push smaller than a step of the
        // byte, and on a gentle slope that is every push: momentum never built and
        // the last fraction of a cell levelled by diffusion alone, over thousands
        // of steps. A dying velocity still reaches zero -- the damping shrinks it
        // below a step, and then it rounds to nothing as often as not.
        int q = v / LIQUID_FLOW_SCALE;
        const int rem = v - q * LIQUID_FLOW_SCALE;  // same sign as v
        if (rem != 0 && pick(LIQUID_FLOW_SCALE, static_cast<uint64_t>(idx),
                             sim_random::Stream::LiquidFlowRound) < (rem < 0 ? -rem : rem))
            q += rem < 0 ? -1 : 1;
        cells[idx].flow = static_cast<int8_t>(q);
        v = q * LIQUID_FLOW_SCALE;

        // Spreading: a share of the difference to each lower side, the share set
        // by how runny the material is, plus the momentum in its direction.
        const int spread = mat.spread > 0 ? mat.spread : 1;
        const int share = 2 + 10 / spread;
        // Rounded up, so any difference past the dead band moves at least a unit:
        // rounded down, every cell crossed holds back a few units, and pressure
        // dies out along a tunnel. Never more than half the difference, so
        // spreading alone cannot overshoot.
        const auto spread_to = [&](int n) {
            const int d = m - n;
            return (n >= 0 && d >= LIQUID_DEAD) ? (d + share - 1) / share : 0;
        };
        int out_r = spread_to(mr);
        int out_l = spread_to(ml);
        // Momentum in its own direction: downhill always, uphill only as a surge.
        if (v > 0 && mr >= 0 && (mr < m || v >= LIQUID_MIN_SURGE)) out_r += v;
        if (v < 0 && ml >= 0 && (ml < m || -v >= LIQUID_MIN_SURGE)) out_l -= v;
        // Never past full: a full cell has no room, and a surge into one would
        // only be pressed straight back up.
        if (mr >= 0 && out_r > LIQUID_FULL - mr) out_r = LIQUID_FULL - mr;
        if (ml >= 0 && out_l > LIQUID_FULL - ml) out_l = LIQUID_FULL - ml;
        if (out_r + out_l > m) {
            out_r = out_r * m / (out_r + out_l);
            out_l = m - out_r;
        }
        if (out_r > 0) {
            transfer_liquid(x, y, x + 1, y, out_r);
            moved = true;
        }
        if (out_l > 0 && cells[idx].type == type) {
            transfer_liquid(x, y, x - 1, y, out_l);
            moved = true;
        }
        // Once for everything that left it so far; still flowing, it stays awake.
        if (moved || v != 0) mark_dirty(x, y);
        if (cells[idx].type != type) return true;
        woken = moved || v != 0;
    }

    // --- up: whatever it holds past full ---
    //
    // All of it, so pressed water reaches the surface in a sweep rather than
    // creeping there: rows run bottom to top, and each cell it lands in passes it
    // on the same step. Up to LIQUID_PUSH_CAP in the cell above, which is what
    // keeps a column capped by sand -- where it can go no further -- inside the
    // 16 bits of Element::mass.
    if (y > 0) {
        const int a = idx - width;
        const int ma = holds(a);
        if (ma >= 0) {
            const int m = cells[idx].mass;
            int up = m - stable_lower(m + ma);
            if (up > LIQUID_PUSH_CAP - ma) up = LIQUID_PUSH_CAP - ma;
            if (up > 0) {
                transfer_liquid(x, y, x, y - 1, up);
                moved = true;
                if (!woken) mark_dirty(x, y);
            }
        }
    }

    // --- pressure: only once nothing nearer moved it ---
    //
    // Last because it is the expensive question and the rarely needed one: a cell
    // with a downhill neighbour gets there by flowing, and only a body levelling
    // through somewhere flow cannot -- a tunnel of full cells -- needs the search.
    if (!moved) moved = level_liquid(x, y);
    return moved;
}

bool Grid::level_liquid(int x, int y) {
    // Only a surface cell gives. Anything with liquid or solid on top of it is
    // pinned by what is above it, and taking from a buried cell would tunnel a
    // hole through the middle of a body rather than lower its surface. Row 0 falls
    // out of the same test, since get_element reads out of bounds as Wall.
    if (get_element(x, y - 1).type != ElementType::Empty) return false;
    const int idx = get_index(x, y);
    const ElementType type = cells[idx].type;
    const int m = cells[idx].mass;

    // Heights in units, measured up from the bottom row, so a cell's surface is
    // its row's floor plus what it holds and a difference reads straight off.
    const auto height_of = [&](int row, int held) {
        return (height - 1 - row) * LIQUID_FULL + held;
    };
    const int source = height_of(y, m);

    // Nearest-first through the body, orthogonally: a diagonal step would let two
    // pools that merely touch at a corner level into each other.
    level_queue.clear();
    level_queue.push_back(idx);
    level_visit[idx] = 1;
    int to = -1, amount = 0;
    for (size_t head = 0; head < level_queue.size(); ++head) {
        const int n = level_queue[head];
        const int ny = n / width;
        const int nx = n - ny * width;

        // A surface of this body with room: a cell short of full with nothing on
        // it, or the Empty on top of a full one.
        if (n != idx) {
            int r = -1;
            if (get_element(nx, ny - 1).type == ElementType::Empty)
                r = cells[n].mass < LIQUID_FULL ? n : n - width;
            if (r >= 0) {
                const int ry = r / width;
                const int rm = cells[r].type == type ? cells[r].mass : 0;
                const int d = source - height_of(ry, rm);
                if (d >= LIQUID_LEVEL_HEAD) {
                    // Half the difference levels the two exactly and cannot
                    // overshoot, so nothing is sent back the other way.
                    amount = d / 2;
                    if (amount > m) amount = m;
                    if (amount > LIQUID_FULL - rm) amount = LIQUID_FULL - rm;
                    if (amount > LIQUID_LEVEL_RATE) amount = LIQUID_LEVEL_RATE;
                    to = r;
                    break;
                }
            }
        }

        // Checked after the test above, so a body that reaches the cap still gets
        // to answer with what it found rather than being cut off one cell short.
        if (static_cast<int>(level_queue.size()) >= LIQUID_LEVEL_CELLS) continue;
        static constexpr int DX[4] = {0, 0, -1, 1};
        static constexpr int DY[4] = {-1, 1, 0, 0};
        for (int k = 0; k < 4; ++k) {
            const int cx = nx + DX[k], cy = ny + DY[k];
            if (!is_within_bounds(cx, cy)) continue;
            const int c = get_index(cx, cy);
            if (cells[c].type != type || level_visit[c] != 0) continue;
            level_visit[c] = 1;
            level_queue.push_back(c);
        }
    }
    for (const int n : level_queue) level_visit[n] = 0;

    if (to < 0 || amount <= 0) return false;
    // An idle cell has no momentum to give, and liquid arriving by pressure brings
    // none: it rose from below, not in from the side.
    cells[idx].flow = 0;
    const int ty = to / width;
    transfer_liquid(x, y, to - ty * width, ty, amount);
    mark_dirty(x, y);
    return true;
}

bool Grid::has_neighbor(int x, int y, ElementType t) const {
    for (int ny = y - 1; ny <= y + 1; ++ny) {
        for (int nx = x - 1; nx <= x + 1; ++nx) {
            if (nx == x && ny == y) continue;
            if (!is_within_bounds(nx, ny)) continue;
            if (cells[get_index(nx, ny)].type == t) return true;
        }
    }
    return false;
}

bool Grid::step_thermal(int x, int y, const Material& mat) {
    const int idx = get_index(x, y);

    // Each pair of neighbours exchanges heat once per step, and the pair belongs to
    // one of its two cells: a cell owns the exchanges with its four "forward"
    // neighbours -- east, and the three below -- and the cell on the far side of
    // each of the other four owns that one. Every one of the eight pairings is
    // then reached exactly once.
    //
    // It used to be every cell with all eight, writing both ends, so a pair whose
    // cells were both visited exchanged twice and a pair with one end asleep or
    // skipped exchanged once. How fast heat moved then depended on which chunks
    // were awake and on the order the sweep met the two cells, and the
    // conduction rate below was tuned against that mixture.
    static constexpr int FX[4] = {1, -1, 0, 1};  // E, SW, S, SE
    static constexpr int FY[4] = {0, 1, 1, 1};

    // A cell at ambient whose forward neighbours are within a degree of ambient
    // has no exchange to make and nothing to bleed, so it does no thermal work at
    // all. That check is the difference between heat costing a fifth of the frame
    // in the worst case and costing nothing measurable. Without it, a scenario
    // containing no fire that never gets warm still pays the neighbour probes and
    // the arithmetic a cell a step to confirm that nothing changed.
    //
    // The forward neighbours have to be looked at, not only this cell: this cell
    // owns those pairs, and a hot neighbour on its forward side has nobody else to
    // exchange with it. Four temperature reads, from cells beside this one.
    //
    // Note this does not rest on the stronger claim that nothing can be off ambient
    // and asleep. The dead band makes that false -- a cell one degree off ambient
    // exchanges nothing with ambient and nothing with a neighbour at the same
    // temperature, so it sleeps sitting there, and a burnt-out world settles
    // holding a few stranded units. What has to hold is only that a cell with
    // somewhere for its heat to go is awake.
    //
    // Heat sources are excluded: Fire is at ambient for exactly one moment, the
    // step it is placed, and skipping it then would leave it cold forever.
    if (cells[idx].temperature == AMBIENT_TEMPERATURE && mat.heat_source == 0) {
        // Nothing near here has been warm since the last reset: no neighbour can
        // be off ambient, and the reads below would find that out the slow way.
        // sweep_warm is this cell's chunk's flag; step_thermal runs only from the
        // sweep, which sets it.
        if (!sweep_warm) return false;
        bool quiet = true;
        for (int k = 0; k < 4 && quiet; ++k) {
            const int nx = x + FX[k];
            const int ny = y + FY[k];
            if (!is_within_bounds(nx, ny)) continue;
            const int d =
                static_cast<int>(cells[get_index(nx, ny)].temperature) - AMBIENT_TEMPERATURE;
            if (d >= 2 || d <= -2) quiet = false;  // outside the dead band
        }
        if (quiet) return false;
    }

    bool changed = false;

    // A source holds itself up rather than settling with its surroundings. Only
    // upwards: a flame in a furnace is not cooled by being in a furnace.
    if (mat.heat_source > cells[idx].temperature) {
        cells[idx].temperature = mat.heat_source;
        note_warm(x, y);
        changed = true;
    }

    // All eight neighbours between them, diagonals included, and deliberately not
    // the four orthogonal ones the pressure search uses. There, a diagonal step
    // would let two pools that merely touch at a corner equalise into each other,
    // which moves matter through a seam with no area. Heat through a corner is
    // harmless, and refusing it is what breaks:
    //
    // an ignited Wood cell becomes Fire, which is a gas, so it rises out of the
    // beam on the next step. The flame that should light the next cell along is
    // then sitting diagonally above it and nowhere else, and with a four-neighbour
    // rule the fire front stalls after exactly one cell. No conductivity makes heat
    // cross a gap the rule says does not exist.
    //
    // It also puts heat on the same 8-neighbourhood as has_neighbor and mark_dirty,
    // so contact means one thing throughout the engine. This cell's four forward
    // neighbours are its half of that neighbourhood; see the ownership note above.
    if (mat.conductivity > 0) {
        for (int k = 0; k < 4; ++k) {
            const int nx = x + FX[k];
            const int ny = y + FY[k];
            // The world's border is an insulator. get_element() would read it as Wall,
            // which conducts, and every edge of the world would then act as an
            // infinite heat sink -- the same trap has_neighbor() documents.
            if (!is_within_bounds(nx, ny)) continue;

            const int nidx = get_index(nx, ny);
            const int nrate = material_of(cells[nidx].type).conductivity;
            if (nrate == 0) continue;  // Empty, or anything else outside the system

            // The pair conducts at the lower of the two, so an insulator between two
            // conductors stops the heat rather than averaging with it.
            const int rate = mat.conductivity < nrate ? mat.conductivity : nrate;
            const int flow = heat_flow(cells[idx].temperature, cells[nidx].temperature,
                                       rate, CONDUCTION_DIVISOR);
            if (flow == 0) continue;

            // Applied to both ends by the same amount, so conduction moves heat and
            // never creates or destroys it. The neighbour is written whether or not
            // it has been visited this step: temperature is not gated on
            // updated_tag, because it is not a move.
            cells[idx].temperature = static_cast<uint8_t>(cells[idx].temperature - flow);
            cells[nidx].temperature = static_cast<uint8_t>(cells[nidx].temperature + flow);
            note_warm_if(x, y, cells[idx].temperature);
            note_warm_if(nx, ny, cells[nidx].temperature);
            changed = true;
        }
    }

    // And the world forgets. Without this the total heat in a world could only ever
    // go up, since Fire adds and nothing removes, and a scene would slowly cook
    // itself. This is the only place heat leaves the simulation, and it is also what
    // eventually puts a burnt-out scene back to sleep.
    const int bleed = heat_flow(cells[idx].temperature, AMBIENT_TEMPERATURE,
                                mat.conductivity, AMBIENT_DIVISOR);
    if (bleed != 0) {
        cells[idx].temperature = static_cast<uint8_t>(cells[idx].temperature - bleed);
        changed = true;
    }

    if (changed) mark_dirty(x, y);
    return changed;
}

bool Grid::try_react(int x, int y) {
    const int idx = get_index(x, y);
    Element& cell = cells[idx];

    // Exactly what both loops below would conclude, without walking them.
    if (!REACTION_TARGETS.is_target[static_cast<int>(cell.type)]) return false;

    // This spot's own ignition point. Both loops below go through it, and that is
    // load-bearing rather than tidy: the first decides whether a cell is allowed to
    // stay awake, the second decides whether it transforms, and a cell whose
    // jittered threshold sits below the row's stated one would otherwise become
    // eligible while nothing was keeping it awake to notice.
    const auto floor_of = [&](const Reaction& r) {
        if (r.temp_jitter == 0) return static_cast<int>(r.min_temp);
        // Downwards only. min_temp is the hardest any cell is, not the average.
        const int t = static_cast<int>(r.min_temp) -
                      authored_pick(static_cast<int>(r.temp_jitter) + 1,
                                    static_cast<uint64_t>(idx),
                                    sim_random::Stream::IgnitionPoint);
        return t < 0 ? 0 : t;
    };

    // Spontaneous-reaction targets must keep re-checking every frame even if they
    // never move. Fire's own movement only calls mark_dirty when step_gas actually
    // relocates it; a fully boxed-in flame -- surrounded by Wood with no Empty cell
    // to rise into -- would generate zero dirty marks after the frame it was created
    // and freeze forever, never decaying and never given another chance to ignite
    // the Wood it is touching.
    //
    // The temperature window is part of that test and not an afterthought. Wood and
    // Water are spontaneous rows too, so marking on the row alone would wake every
    // wooden beam and every pool in the world every step, handing back the entire
    // cost of the sleep system. A cell only self-marks while it is inside the window
    // that could transform it, and getting into that window means its temperature is
    // moving, which step_thermal is already marking it for.
    for (const Reaction& r : REACTIONS) {
        if (r.catalyst == ElementType::Count && r.target == cell.type &&
            cell.temperature >= floor_of(r) && cell.temperature <= r.max_temp) {
            mark_dirty(x, y);
            break;
        }
    }

    for (const Reaction& r : REACTIONS) {
        if (r.target != cell.type) continue;
        if (cell.temperature < floor_of(r) || cell.temperature > r.max_temp) continue;
        if (r.catalyst != ElementType::Count && !has_neighbor(x, y, r.catalyst)) continue;
        // One roll per cell per step: the loop commits to the first eligible row
        // and returns either way, so this never draws twice from the same
        // coordinates -- which it would otherwise do, getting the same answer.
        if (chance_per_myriad(static_cast<int>(r.chance_per_myriad), static_cast<uint64_t>(get_index(x, y)),
                             sim_random::Stream::Reaction)) {
            // Water boiling to steam keeps its amount, so condensing gives it
            // back; anything else is a new cell and starts full (place()).
            const uint16_t carried = cell.mass;
            const bool hand_over = carries_mass(cell.type) && carries_mass(r.result);

            // Wood catching into Charred is the same cell of the same structure in a
            // new state, so it stays in the piece it was in: its piece_tag, and its
            // fall clock, which is what ticks means for both (tick_role). place()
            // starts every cell as a piece of its own, which is right for the brush
            // and was wrong here. On a piece that had ever broken, landed or tipped,
            // and so wore a non-zero tag, every cell that caught became a separate
            // tag-0 piece and fell off, taking the fire with it. The flame front
            // could not walk along that piece at all. A tipping body also read each
            // ignition as its shape changing and stopped. Carrying ticks is safe
            // because tick_role in element.h gives every structural material the
            // fall-clock role, so the byte means the same on both sides.
            const bool same_structure = is_structural(cell.type) && is_structural(r.result);
            const uint8_t tag = cell.piece_tag;
            const uint8_t fall_clock = cell.ticks;

            set_element(x, y, r.result);
            if (hand_over) {
                cells[idx].mass = carried;
                pixels[idx] = pixel_of(cells[idx]);
            }
            if (same_structure) {
                cells[idx].piece_tag = tag;
                cells[idx].ticks = fall_clock;
            }
            return true;
        }
        return false;  // first eligible row commits the cell for this frame, win or lose
    }
    return false;
}

bool Grid::emit_flame(int x, int y, const Material& mat) {
    const int idx = get_index(x, y);

    if (!chance(static_cast<int>(mat.emit_chance), static_cast<uint64_t>(idx),
                sim_random::Stream::Emission))
        return false;

    // Gather the empty neighbours, then choose among them, rather than choosing a
    // direction and giving up if it is occupied. A blind pick makes a cell with one
    // exposed face emit at an eighth of its stated rate, so a flame front along a
    // flat surface -- where every cell has exactly one free side -- would be eight
    // times thinner than the same wood burning at a corner. The rate belongs to the
    // material, not to the shape of what is around it.
    int free_idx[8];
    int free_x[8];
    int free_y[8];
    int n = 0;
    for (int ny = y - 1; ny <= y + 1; ++ny) {
        for (int nx = x - 1; nx <= x + 1; ++nx) {
            if (nx == x && ny == y) continue;
            if (!is_within_bounds(nx, ny)) continue;
            const int nidx = get_index(nx, ny);
            if (cells[nidx].type != ElementType::Empty) continue;
            free_idx[n] = nidx;
            free_x[n] = nx;
            free_y[n] = ny;
            ++n;
        }
    }
    // Buried cells emit nothing, and that is the feature. Fire reading as a layer
    // clinging to exposed surfaces rather than as something that fills the inside of
    // a solid is not a rule written anywhere in this engine; it is what this early
    // return does when a burning cell has no air beside it.
    if (n == 0) return false;

    // Its own stream, not the yes/no draw's: two decisions about one cell on one
    // step have to be two streams, or they are the same number.
    const int slot = pick(n, static_cast<uint64_t>(idx), sim_random::Stream::EmissionSlot);

    set_element(free_x[slot], free_y[slot], mat.emits);

    // Keyed on the destination, not on the emitter. Two flames thrown from the same
    // burning cell on different steps already differ, because the step is folded
    // into every draw, but a flame's lifetime should vary with where it is, so that
    // a row of flames standing side by side is ragged rather than merely changing
    // shape over time.
    cells[free_idx[slot]].ticks = static_cast<uint8_t>(
        FLAME_LIFETIME_MEAN + spread(FLAME_LIFETIME_SPREAD,
                                     static_cast<uint64_t>(free_idx[slot]),
                                     sim_random::Stream::FlameLifetime));
    return true;
}

bool Grid::step_fire(int x, int y) {
    const int idx = get_index(x, y);
    Element& cell = cells[idx];

    // A flame with no age was not made by emit_flame -- it was painted with the
    // brush, or is a reaction product like Oil flashing. Give it a full life here
    // rather than in every place that can create one, the same reason
    // spawn_temperature lives in the table rather than at call sites.
    if (cell.ticks == 0) {
        cell.ticks = FLAME_LIFETIME_MEAN;
        return false;
    }

    if (--cell.ticks == 0) {
        set_element(x, y, ElementType::Empty);
        return true;
    }

    // The colour ramp, and the reason flame lifetime is a countdown. A flame is
    // white-hot where it is made and dim red where it dies, and that gradient is
    // most of what separates fire from an orange blob on screen. It is drawn from
    // age because age is the only thing that varies between two flame cells: they
    // share a MATERIALS row, so anything read from the table would give every flame
    // in the world the same colour.
    //
    // Jitter is re-drawn from the cell's own index so that neighbouring flames of
    // the same age still differ. Without it the ramp turns a flame front into clean
    // concentric bands, which reads as a gradient rather than as fire.
    //
    // Three stops, not two. A single linear interpolation from near-white to dull
    // red passes through greys and dusty salmons, because the shortest path between
    // those two points in RGB goes nowhere near saturated orange -- so the
    // most-visible middle of a flame's life spends itself in the least saturated
    // colours available. Bending the ramp through a saturated orange at mid-life
    // keeps every stage on the outside of the colour space, at the cost of one extra
    // branch rather than a wider colour model.
    //
    // Saturation is also why the hot end is yellow-white rather than white: white is
    // the absence of hue, and a flame that peaks there reads as a blown-out
    // highlight rather than as something hot.
    const int life = static_cast<int>(cell.ticks);

    // Age runs against the widest life any flame can have, not against this flame's
    // own. Flames live different lengths of time and there is no spare byte to
    // record which length this one drew, so a short-lived flame starts partway down
    // the ramp and never reaches the hot end. That reads correctly rather than
    // merely being affordable -- a flame that dies quickly is a cooler one -- and it
    // makes colour a function of remaining life, which is what the eye reads as
    // temperature.
    const int den = FLAME_LIFETIME_MAX;
    const int num = life > den ? den : life;

    struct Stop { int r, g, b; };
    constexpr Stop HOT{0xFF, 0xE9, 0x8C};  // yellow-white, freshly thrown
    constexpr Stop MID{0xFF, 0x8A, 0x1E};  // saturated orange, the body of the flame
    constexpr Stop DIM{0x9E, 0x22, 0x08};  // deep ember red, at death
    constexpr int KNEE = 55;  // percent of life where MID sits

    const int pct = num * 100 / den;
    const Stop& lo = pct >= KNEE ? MID : DIM;
    const Stop& hi = pct >= KNEE ? HOT : MID;
    const int seg_num = pct >= KNEE ? pct - KNEE : pct;
    const int seg_den = pct >= KNEE ? 100 - KNEE : KNEE;

    const int jit = authored_spread(material_of(ElementType::Fire).color_jitter,
                                    static_cast<uint64_t>(idx), sim_random::Stream::ColorJitter);
    const auto lerp = [&](int a, int b) {
        const int v = a + (b - a) * seg_num / seg_den + jit;
        return static_cast<uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    cell.color = 0xFF000000u | (lerp(lo.r, hi.r) << 16) | (lerp(lo.g, hi.g) << 8) | lerp(lo.b, hi.b);
    pixels[idx] = cell.color;
    mark_dirty(x, y);
    return false;
}

bool Grid::step_steam(int x, int y) {
    const int idx = get_index(x, y);
    Element& cell = cells[idx];

    // A steam cell with no clock has just been created -- by the brush, by water
    // boiling, or by water dousing a flame -- and is seeded here rather than at each
    // of those three sites. place() zeroes `ticks`, so "no clock" and "new" are the
    // same condition and there is nothing to keep in sync.
    if (cell.ticks == 0) {
        cell.ticks = static_cast<uint8_t>(
            STEAM_LIFETIME_MEAN + spread(STEAM_LIFETIME_SPREAD,
                                         static_cast<uint64_t>(idx),
                                         sim_random::Stream::SteamLifetime));
        return false;
    }

    // A ceiling is anything solid overhead, and the world's own top edge counts.
    // get_element answers Wall out of bounds, so a pocket that has risen as far as
    // it can go condenses against the sky the same way it does against stone, with
    // no special case. Under an open sky that is the only thing that stops steam
    // being permanent, and it is why this needs no separate give-up rule.
    //
    // Only the cell in contact ages. Everything under it is waiting its turn, which
    // is the whole of the collect-and-drip behaviour -- see the constants in grid.h
    // for why that is the rule rather than a plain countdown.
    if (!is_solid(get_element(x, y - 1).type)) return false;

    if (--cell.ticks == 0) {
        // Back to water, with the amount the steam was carrying.
        const uint16_t carried = cell.mass;
        set_element(x, y, ElementType::Water);
        cells[idx].mass = carried;
        pixels[idx] = pixel_of(cells[idx]);
        return true;
    }

    // A cell whose clock is running has to stay awake, the same rule step_thermal
    // states for a temperature that is still moving and try_react states for a
    // spontaneous target. A steam cell packed against a ceiling does not move, so
    // nothing else marks it, and without this it would sleep with time left on it
    // and hang there forever.
    //
    // The standing cost is bounded twice over: only the contact layer of a pocket is
    // ever ageing, and a cell that is ageing is at most its full lifetime from being
    // water, which sleeps. A pocket's interior generates no marks at all and is
    // woken a row at a time by the set_element above.
    mark_dirty(x, y);
    return false;
}

void Grid::step_cell(int x, int y) {
    Element& current = cells[get_index(x, y)];
    if (current.type == ElementType::Empty || current.updated_tag == frame_tag) return;

    // Claim the cell for this step whether or not it ends up moving, so it is never
    // visited twice in one sweep.
    current.updated_tag = frame_tag;

    const Material& mat = material_of(current.type);

    // Before the reaction, not after: the row that transforms this cell is gated on
    // the temperature it has now, so a cell that reaches its ignition point this
    // step ignites on this step rather than one later.
    step_thermal(x, y, mat);

    // Chained rather than written as two independent checks, because nothing that
    // emits is itself a flame and this runs for every awake cell in the world every
    // step. The two branches are predictable, so the form is chosen for clarity
    // rather than speed.
    //
    // A flame ages before anything else looks at it, so the step it runs out is the
    // step it disappears. Burning cells throw flame before they are given their own
    // chance to decay, so a cell always emits at least once -- one that lost its
    // very first decay roll would otherwise appear and vanish having shown nothing.
    //
    // Steam's clock sits in the same chain and for the same reason: it is the other
    // cell in the table with a lifetime on `ticks`, and a cell that has just
    // condensed must not then be moved as though it were still a gas.
    if (current.type == ElementType::Fire) {
        if (step_fire(x, y)) return;  // gone; nothing left to react or move
    } else if (current.type == ElementType::Steam) {
        if (step_steam(x, y)) return;  // now water; it moves as water, next step
    } else if (mat.emits != ElementType::Count) {
        emit_flame(x, y, mat);
    }

    if (try_react(x, y)) return;  // converted; let the new material move starting next frame

    // Nothing anchors a flame. Holding a fuelled flame in place so it stayed in
    // contact with what it was consuming works and models the wrong thing: it makes
    // the flame the fuel. The fuel is Charred, which is Static and never had
    // anywhere to go, and flame is free to rise the way a gas should -- it is
    // decoration with a short life, and propagation happens underneath it through
    // Charred's heat.
    //
    // A flame sits still one step in ten, which is how a whole-cell mover expresses
    // 0.9 cells per step. Fire only; slowing Steam would change a material nobody
    // asked about.
    //
    // Placed after the reaction check and before the move so a skipped step is only
    // a skipped move: the flame still ages, still ramps its colour, and can still be
    // doused. Skipping the whole cell instead would make flames live 11% longer as a
    // side effect of being asked to travel slower.
    if (current.type == ElementType::Fire &&
        chance(FLAME_RISE_SKIP_PERCENT, static_cast<uint64_t>(get_index(x, y)),
               sim_random::Stream::FlameRise)) {
        return;
    }

    switch (mat.move) {
        case MoveKind::Static: break;
        case MoveKind::Powder: step_powder(x, y, mat); break;
        case MoveKind::Liquid: step_liquid(x, y, mat); break;
        case MoveKind::Gas:    step_gas(x, y, mat); break;
    }
}

void Grid::update() {
    // First of all, so that every part of this step agrees on which step it is: the
    // support resolve below, the sweep after it, and the randomness hash. It
    // deliberately does not sit next to ++frame_tag further down -- that one has to
    // stay below resolve_support(), and the two counters answer different questions.
    ++step_count;

    // Before the sweep and before the chunk swap, so the cells a falling piece
    // vacates land in the bounds that are about to be simulated -- whatever was
    // displaced out from under it starts flowing on this step rather than the next.
    resolve_support();

    // After support, so a piece that started tipping during this step's resolve
    // turns for the first time on this step rather than sitting out one first.
    step_tipping();

    ++frame_tag;

    // Take the work accumulated during the previous step and start a fresh set of
    // bounds for the work this step generates.
    chunk_current.swap(chunk_next);
    for (DirtyRect& r : chunk_next) r.clear();

    // Chunk rows are walked bottom to top, and so are the cell rows inside them,
    // because a falling cell has to land in rows that have already settled. The
    // world is still swept row by row rather than chunk by chunk -- processing a
    // whole chunk at a time would let material fall further on one side of a chunk
    // border than the other and produce visible seams.
    for (int cy = chunks_y - 1; cy >= 0; --cy) {
        // Union of the dirty bounds across this chunk row, so a row of sleeping
        // chunks costs one pass over chunks_x rects instead of CHUNK_SIZE passes
        // over the full world width.
        int row_min_y = 0, row_max_y = -1;
        for (int cx = 0; cx < chunks_x; ++cx) {
            const DirtyRect& r = chunk_current[cy * chunks_x + cx];
            if (r.is_empty()) continue;
            if (row_max_y < row_min_y) {
                row_min_y = r.min_y;
                row_max_y = r.max_y;
            } else {
                row_min_y = std::min(row_min_y, r.min_y);
                row_max_y = std::max(row_max_y, r.max_y);
            }
        }
        if (row_max_y < row_min_y) continue;  // the whole chunk row is asleep

        for (int y = row_max_y; y >= row_min_y; --y) {
            // Alternate the sweep direction to stop piles leaning one way.
            //
            // Keyed on the row, and it takes a stream tag anyway: row y and cell
            // index y are the same number, and cell index y is a real cell, so
            // without a tag a row's direction would be drawn from the same value as
            // that cell's own decisions.
            const bool leftward = coin(static_cast<uint64_t>(y), sim_random::Stream::SweepDirection);

            for (int i = 0; i < chunks_x; ++i) {
                const int cx = leftward ? (chunks_x - 1 - i) : i;
                const DirtyRect& r = chunk_current[cy * chunks_x + cx];
                if (r.is_empty() || y < r.min_y || y > r.max_y) continue;

                const int start_x = leftward ? r.max_x : r.min_x;
                const int end_x = leftward ? r.min_x - 1 : r.max_x + 1;
                const int step = leftward ? -1 : 1;

                // The chunk's warm flag, looked up once for the run of cells rather
                // than once per cell -- see sweep_warm.
                sweep_chunk = cy * chunks_x + cx;
                sweep_warm = chunk_warm[static_cast<size_t>(sweep_chunk)] != 0;
                for (int x = start_x; x != end_x; x += step) {
                    step_cell(x, y);
                }
            }
        }
    }
}





