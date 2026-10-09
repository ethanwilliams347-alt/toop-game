#include "grid.h"
#include "reaction.h"
#include <algorithm>

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
    // Both start empty: a world of nothing but Empty has nothing to simulate.

    support_visit.resize(width * height, 0);
    support_state.resize(width * height, 0);
    scratch_visit.resize(width * height, 0);
    balance_visit.resize(width * height, 0);

    // The step loop allocates nothing, and these three are why that is true rather
    // than merely usual: they grow by push_back and are only ever .clear()ed, so
    // without a reserve the first collapse or pressure probe to reach a new high-water
    // mark would reallocate mid-step. Each is reserved to a hard bound, not a guess:
    //
    //   support_component -- capped by the `> MAX_SUPPORT_CELLS` check in
    //     resolve_support, which runs after the push, so one past the cap.
    //   support_stack -- not capped by that check at all. Every cell popped pushes up
    //     to eight neighbours, and pops stop pushing once the component passes the
    //     cap, so the stack never holds more than eight per judged cell plus the seed.
    //   pressure_queue -- the `>= MAX_PRESSURE_CELLS` check runs once per cell
    //     dequeued, before its up-to-four neighbours are pushed, so a queue of
    //     MAX_PRESSURE_CELLS - 1 can pass it and still grow by four.
    support_component.reserve(MAX_SUPPORT_CELLS + 1);
    drop_keys.reserve(MAX_SUPPORT_CELLS + 1);
    support_stack.reserve(8 * MAX_SUPPORT_CELLS + 1);
    pressure_queue.reserve(MAX_PRESSURE_CELLS + 3);

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

    pending_support.clear();
    support_stack.clear();
    support_component.clear();
    support_seeds.clear();
    support_deferred.clear();
    std::fill(support_visit.begin(), support_visit.end(), uint8_t{0});
    std::fill(support_state.begin(), support_state.end(), uint8_t{0});
    support_epoch = 0;
    resolving_support = false;

    pressure_queue.clear();
    std::fill(scratch_visit.begin(), scratch_visit.end(), uint8_t{0});
    scratch_epoch = 0;

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

    frame_tag = 0;
    step_count = 0;

    // width, height, chunks_x and chunks_y are not here on purpose: the vectors
    // above are cleared in place rather than resized, which is only correct as long
    // as the grid's dimensions never change out from under them. Nothing in this
    // class exposes a way to change them after construction, and reset() must not
    // become the first.
    //
    // vent_radius, seek_level_on and room_above_on are also not here. They are
    // configuration -- how this engine behaves -- rather than facts about this
    // world, so a reset that silently put them back to their defaults would throw
    // away a setting the caller made on purpose. That makes reset() not quite "a
    // fresh grid with this seed"; this is the exception, and it is pinned by a test
    // in test_grid.cpp.
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
    if (!is_within_bounds(x, y)) return Element{ElementType::Wall, material_of(ElementType::Wall).color, 0};
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
        if (room_above_on) make_room_above(x, y);
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
    // Harmless while no row in REACTIONS turns one structural material into
    // another. A structural-to-structural row would silently reset the tag of every
    // cell it touched, re-welding pieces that had come apart; if one is ever
    // written, decide here whether the tag survives a transformation.

    cells[idx] = el;
    pixels[idx] = el.color;
    mark_dirty(x, y);

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
    pixels[idx1] = cells[idx1].color;
    pixels[idx2] = cells[idx2].color;

    // Mark as updated so neither cell moves again this step.
    cells[idx1].updated_tag = frame_tag;
    cells[idx2].updated_tag = frame_tag;

    // Both ends of the move changed, so both ends and their neighbours have to be
    // awake next step -- this is what lets a falling grain keep falling and what
    // pulls the cells above it into motion behind it.
    mark_dirty(x1, y1);
    mark_dirty(x2, y2);

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
        // piece to answer a question the first one already answered. Sharing the
        // buffer with the pressure search is safe because that search only runs
        // inside the cell sweep, and support resolves before the sweep starts.
        if (++scratch_epoch == 0) {
            std::fill(scratch_visit.begin(), scratch_visit.end(), uint8_t{0});
            scratch_epoch = 1;
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
    // Adopting a neighbour's Moved verdict lands here too, and zeroes a piece that
    // is genuinely still falling. That costs it its run-up, which is a visible but
    // rare stutter -- it needs two separate pieces to touch mid-fall -- and erring
    // towards slower is the direction every other guess in this file errs in.
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
            // At rest, one case is skipped without asking: the fill came straight
            // down through the seed's own material to the bottom of the world.
            // That is terrain, or a piece standing on the world floor, and
            // weighing it means walking it, which on a wide burning slab -- a
            // seed queued every few cells, every step -- costs more than the
            // fire. Crossing into another material on the way, or landing on
            // something other than the floor, still gets asked.
            const bool on_world_floor_alone =
                !was_moving && cy + 1 >= height &&
                material_family(cells[idx].type) == material_family(cells[seed].type);
            const bool tipped = !on_world_floor_alone && topple_if_unbalanced(x, y, was_moving);
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
                    if (!was_moving &&
                        static_cast<SupportState>(support_state[nidx]) == SupportState::Supported &&
                        material_family(cells[nidx].type) != material_family(cells[seed].type)) {
                        topple_if_unbalanced(x, y, false);
                    }
                    settle_marks(SupportState::Supported, idx);
                    return;
                }

                support_visit[nidx] = support_epoch;
                support_state[nidx] = static_cast<uint8_t>(SupportState::Pending);
                support_stack.push_back(nidx);
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
    if (scratch_visit[seed] == scratch_epoch) return;

    fracture_component.clear();
    fracture_component.push_back(seed);
    scratch_visit[seed] = scratch_epoch;

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
                if (scratch_visit[nidx] == scratch_epoch) continue;
                scratch_visit[nidx] = scratch_epoch;
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
        // A swap puts the displaced fluid in the grain's old cell, one row up.
        // That is harmless for a single grain and wrong for a stream: there is
        // always another grain above, so the exchange repeats every step and the
        // fluid is handed up the column without bound, arriving in open air tens
        // of cells over the pool. Nothing in the swap rule refers to where the
        // fluid's surface is, so nothing stops it.
        //
        // So the fluid is sent to that surface instead, when one is close enough
        // to find. vent_fluid leaves Empty behind on success and the swap below
        // then drops the grain into a vacancy rather than trading with the fluid
        // at all; on failure this is exactly the plain swap. Only attempted when
        // there is something to displace, since falling through air is the
        // common case and pays nothing for this.
        if (cells[get_index(x, y + 1)].type != ElementType::Empty) vent_fluid(x, y + 1);
        swap_elements(x, y, x, y + 1);
        return true;
    }

    // A grain that rolls keeps falling in the same step, which removes the
    // horizontal shelves without freezing the pile into columns.
    //
    // swap_elements tags only the two cells it touches, so an entire row can cascade
    // diagonally inside one sweep -- grain at x to (x+1, y+1), grain at x+1 still
    // untagged and on to (x+2, y+1), all the way along. Every grain lands one row
    // down and one column over in the same step, while the row beneath was swept
    // earlier and has not caught up, leaving a one-cell shelf standing proud of the
    // pile with nothing under it.
    //
    // Rather than forbid the move, finish it: a grain that rolls off an edge takes
    // its one further cell of fall immediately, arriving at its resting depth in the
    // same step instead of hanging a row above the slope. There is no moment at
    // which the shelf exists, so nothing has to be forbidden to prevent it, and
    // settled grains still slump freely down a face. Restricting the diagonal
    // instead catches rest as well as motion, and holds piles in vertical columns no
    // powder would.
    const int dir = coin(static_cast<uint64_t>(idx), sim_random::Stream::PowderDirection) ? -1 : 1;

    for (const int d : {dir, -dir}) {
        const int nx = x + d;
        if (!can_displace(mat, nx, y + 1, 1)) continue;

        swap_elements(x, y, nx, y + 1);
        if (can_displace(mat, nx, y + 2, 1)) swap_elements(nx, y + 1, nx, y + 2);
        return true;
    }

    return false;
}

// dy is +1 for liquids (settle downwards) and -1 for gases (rise).
bool Grid::step_fluid(int x, int y, const Material& mat, int dy) {
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

    // Blocked vertically, so flow sideways to find a level. Travelling several
    // cells per step is what makes a pool settle quickly instead of oozing.
    //
    // A lateral move has to land somewhere it can rest or descend from, and that
    // condition is why a pool can sleep. Without it, the last partial row of any
    // body of liquid slides back and forth across its own flat surface forever: it
    // cannot sink (equal density fails can_displace), and seek_level will not take
    // it either, because a one-cell head is inside MIN_PRESSURE_HEAD's hysteresis.
    // Bare Empty to the side was otherwise reason enough to move, so a tank filled
    // to one cell over an exact multiple of its width never settled.
    //
    // Sideways travel therefore runs along a floor and never up onto one. Refusing
    // only the move whose destination is perched on more of the same liquid is half
    // the pointless moves; the other half is its mirror, a surface cell stepping off
    // its own body onto a solid shoulder at the surface row. That move is one-way --
    // the return trip fails, since the cell it came from is now liquid-floored -- so
    // a cell that takes it is stranded permanently as a film one row proud of the
    // water, a body of exactly one cell that seek_level cannot rescue.
    //
    // on_solid makes perched-ness strictly decrease across every lateral move, which
    // is why this cannot jitter: the old defect needed a cell able to move back and
    // forth across its own surface, and one direction is now closed. Puddle
    // spreading along a floor is solid-to-solid and untouched.
    const bool on_solid = is_solid(get_element(x, y + 1).type);
    const auto can_rest_at = [&](int nx) {
        if (can_displace(mat, nx, y + 1, 1)) return true;  // it can carry on down
        return on_solid;  // otherwise only ever along a floor, never off one onto another
    };

    // Nothing may travel sideways across open air. `spread` is a distance a fluid
    // may cover along a surface to find its level, and it was being spent crossing
    // empty space as well, because can_rest_at() is satisfied by "I could fall from
    // there", which every point in mid-air satisfies. A cell inside a falling
    // stream cannot descend, so it reached this scan, found several cells of
    // nothing beside it, and teleported to the far end -- a horizontal spike shot
    // out of the side of a pouring stream.
    //
    // Checked against y + dy rather than y + 1 so it reads the same way for a gas:
    // the cell that has to hold something up is the one gravity would pull it
    // towards, which is below for a liquid and above for a gas.
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

            // One cell of overhang is allowed and then the run stops. That one cell
            // is what pouring off the edge of a ledge is -- reach the lip, step past
            // it, fall -- so forbidding it outright would strand water on a shelf.
            // What it must not become is a licence to keep going: past the lip there
            // is nothing to flow along, so there is nothing left for `spread` to
            // measure.
            if (over_void(cx)) break;
        }
        if (best != x) {
            swap_elements(x, y, best, y);
            return true;
        }
    }

    // Out of ordinary moves. A liquid gets one last question -- see the comment on
    // MAX_PRESSURE_CELLS in the header. Gases are excluded: dy < 0 has already
    // spent its vertical move going up, and a gas finding its level is not a thing
    // anyone has asked to see.
    if (dy > 0 && seek_level_on && seek_level(x, y)) return true;

    return false;
}

int Grid::find_lower_surface(int x, int y, ElementType type) {
    // Fresh epoch per search. Unlike the support fill's, these marks must not
    // outlive the one search that made them: two adjacent surface cells of the same
    // body ask genuinely different questions, because the threshold is measured
    // from the asking cell's own row.
    if (++scratch_epoch == 0) {  // wrapped, so old marks can no longer be told apart
        std::fill(scratch_visit.begin(), scratch_visit.end(), uint8_t{0});
        scratch_epoch = 1;
    }

    const int target_row = y + MIN_PRESSURE_HEAD;

    pressure_queue.clear();
    const int seed = get_index(x, y);
    pressure_queue.push_back(seed);
    scratch_visit[seed] = scratch_epoch;

    for (size_t head = 0; head < pressure_queue.size(); ++head) {
        const int idx = pressure_queue[head];
        const int cy = idx / width;
        const int cx = idx - cy * width;

        // A surface of this body, low enough to be worth moving to. This can never
        // be the asking cell itself, which is at row y.
        if (cy >= target_row && cells[idx - width].type == ElementType::Empty) return idx;

        // Checked after the test above, so a body that reaches the cap still gets
        // to answer with what it found rather than being cut off one cell short.
        if (static_cast<int>(pressure_queue.size()) >= MAX_PRESSURE_CELLS) return -1;

        static constexpr int DX[4] = { 0,  0, -1, 1 };
        static constexpr int DY[4] = {-1,  1,  0, 0 };
        for (int k = 0; k < 4; ++k) {
            const int nx = cx + DX[k];
            const int ny = cy + DY[k];
            if (!is_within_bounds(nx, ny)) continue;

            const int nidx = get_index(nx, ny);
            if (cells[nidx].type != type) continue;
            if (scratch_visit[nidx] == scratch_epoch) continue;

            scratch_visit[nidx] = scratch_epoch;
            pressure_queue.push_back(nidx);
        }
    }

    return -1;
}

bool Grid::seek_level(int x, int y) {
    // Only a surface cell moves. Anything with liquid or solid on top of it is
    // pinned by what is above it, and letting a buried cell go would tunnel a hole
    // through the middle of a body rather than lower its surface.
    //
    // This also bounds the cost: in a settled pool it is one array read per cell,
    // and only the thin surface line reaches the search. The row-0 case falls out
    // of the same test, since get_element reads out-of-bounds as Wall.
    if (get_element(x, y - 1).type != ElementType::Empty) return false;

    const int target = find_lower_surface(x, y, cells[get_index(x, y)].type);
    if (target < 0) return false;

    // Onto the receiving surface, not into it. The cell above the target is the
    // Empty that find_lower_surface required, so this is a swap with Empty and
    // conserves matter for the same reason every other move in this file does.
    const int ty = target / width;
    swap_elements(x, y, target - ty * width, ty - 1);
    return true;
}

bool Grid::vent_fluid(int fx, int fy) {
    const int idx = get_index(fx, fy);
    const ElementType fluid = cells[idx].type;

    // The box, clipped to the world once rather than tested a cell at a time. This
    // scan was the largest cost in `churning` -- a grain sinking through bulk water
    // looks at every cell of the box and almost never finds one free -- so the loops
    // below walk rows directly instead of paying a bounds check and an index
    // multiply per cell.
    const int r = vent_radius;
    const int x_lo = fx - r > 0 ? fx - r : 0;
    const int x_hi = fx + r < width - 1 ? fx + r : width - 1;
    const int y_lo = fy - r > 0 ? fy - r : 0;
    const int y_hi = fy + r < height - 1 ? fy + r : height - 1;

    // Every destination is an Empty cell inside the box, so a box with none in it
    // has no answer whatever order it is read in. Checked first, in plain memory
    // order, because that is the common case and it lets the direction hash below
    // be skipped along with the ordered scan. The result is unchanged: the ordered
    // scan visits exactly these cells, only in a different order.
    bool any_empty = false;
    for (int ny = y_lo; ny <= y_hi && !any_empty; ++ny) {
        const Element* row = &cells[static_cast<size_t>(ny) * width];
        for (int nx = x_lo; nx <= x_hi; ++nx) {
            if (row[nx].type == ElementType::Empty) {
                any_empty = true;
                break;
            }
        }
    }
    if (!any_empty) return false;

    // The destination must be this fluid's own free surface -- an Empty cell with
    // more of the same fluid directly beneath it -- and not merely any Empty within
    // reach. Any-Empty reproduces the defect this was written to fix: the nearest
    // empty cell to a grain entering the water is very often the air just above the
    // sand pile, so the water is deposited back on top of the pile.
    const int dir = coin(static_cast<uint64_t>(idx), sim_random::Stream::FluidDirection) ? -1 : 1;

    // The same clip expressed as offsets, in the mirrored frame: nx = fx + ox * dir,
    // and ox still runs low to high, so ties break in exactly the order they did
    // when every cell of the full box was visited and the out-of-world ones skipped.
    const int ox_lo = dir > 0 ? x_lo - fx : fx - x_hi;
    const int ox_hi = dir > 0 ? x_hi - fx : fx - x_lo;

    // Two kinds of destination. Sending the fluid to its own surface fixes the
    // pour, and then the defect returns once the sand pile has grown into a cone
    // standing proud of the water: the thin films of water clinging to the cone's
    // flanks have sand underneath, not water, so there is no surface of their own
    // kind to go to, and grains rolling down the slope go back to handing them
    // upwards one cell at a time. The pile itself becomes the conveyor.
    //
    // So a flank film may also drain to somewhere it can rest, and that route is
    // restricted to ny >= fy -- level or downhill, never up. Water running down the
    // outside of a pile is what should happen; the defect is water going the other
    // way.
    int drain_x = -1, drain_y = -1, drain_score = 0;

    int best_x = -1, best_y = -1, best_score = 0;
    for (int ny = y_lo; ny <= y_hi; ++ny) {
        const int oy = ny - fy;
        const Element* row = &cells[static_cast<size_t>(ny) * width];
        // The bottom row of the world stands on the border, which reads as Wall --
        // the same answer get_element gives for the cell past the edge.
        const Element* below = ny + 1 < height ? row + width : nullptr;
        for (int ox = ox_lo; ox <= ox_hi; ++ox) {
            // `dir` mirrors the scan rather than steering it, so the choice between
            // two equally good cells either side of a grain is not always the left
            // one. Same trick and same stream as the fluid direction pick: a fixed
            // order here would comb every pour in one direction.
            const int nx = fx + ox * dir;
            if (row[nx].type != ElementType::Empty) continue;

            const ElementType under = below ? below[nx].type : ElementType::Wall;
            const int score = std::abs(ox) + std::abs(oy);

            if (under == fluid) {
                if (best_x < 0 || score < best_score) {
                    best_x = nx;
                    best_y = ny;
                    best_score = score;
                }
            } else if (ny >= fy && is_solid(under)) {
                // Ranked by depth before distance, so a film takes the furthest step
                // down the slope it can see rather than shuffling one cell at a time
                // against a pile that is being fed from above.
                const int depth = (ny - fy) * 16 - score;
                if (drain_x < 0 || depth > drain_score) {
                    drain_x = nx;
                    drain_y = ny;
                    drain_score = depth;
                }
            }
        }
    }

    if (best_x < 0 && drain_x >= 0) {
        best_x = drain_x;
        best_y = drain_y;
    }

    // No surface within reach, so the caller falls back to the plain swap. This is
    // the right answer rather than a giving-up: a grain sinking deep inside a large
    // body of water has no conveyor above it feeding the exchange, which is the
    // only thing that made the one-cell lift add up to anything.
    if (best_x < 0) return false;

    swap_elements(fx, fy, best_x, best_y);
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

    // A cell that is exactly at ambient does no thermal work at all, and this one
    // line is the difference between heat costing a fifth of the frame in the worst
    // case and costing nothing measurable. Without it, a scenario containing no fire
    // that never gets warm still pays eight neighbour probes a cell a step to
    // confirm that nothing changed.
    //
    // It is exact rather than an approximation, because conduction is symmetric.
    // Every exchange writes both ends by the same amount, so it does not matter
    // which of the pair initiates it: a neighbour more than a degree from ambient
    // is not skipped here and will do the exchange itself when the sweep reaches
    // it, and one within a degree had nothing to do either way.
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
    if (cells[idx].temperature == AMBIENT_TEMPERATURE && mat.heat_source == 0) return false;

    bool changed = false;

    // A source holds itself up rather than settling with its surroundings. Only
    // upwards: a flame in a furnace is not cooled by being in a furnace.
    if (mat.heat_source > cells[idx].temperature) {
        cells[idx].temperature = mat.heat_source;
        changed = true;
    }

    // All eight neighbours, not the four orthogonal ones, and deliberately not the
    // same call the pressure search makes. There, a diagonal step would let two
    // pools that merely touch at a corner equalise into each other, which moves
    // matter through a seam with no area. Heat through a corner is harmless, and
    // refusing it is what breaks:
    //
    // an ignited Wood cell becomes Fire, which is a gas, so it rises out of the
    // beam on the next step. The flame that should light the next cell along is
    // then sitting diagonally above it and nowhere else, and with a four-neighbour
    // rule the fire front stalls after exactly one cell. No conductivity makes heat
    // cross a gap the rule says does not exist.
    //
    // It also puts heat on the same 8-neighbourhood as has_neighbor and mark_dirty,
    // so contact means one thing throughout the engine.
    if (mat.conductivity > 0) {
        static constexpr int DX[8] = { 0,  0, -1, 1, -1,  1, -1, 1 };
        static constexpr int DY[8] = {-1,  1,  0, 0, -1, -1,  1, 1 };
        for (int k = 0; k < 8; ++k) {
            const int nx = x + DX[k];
            const int ny = y + DY[k];
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
    // never move. Fire's own movement only calls mark_dirty when step_fluid actually
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
            set_element(x, y, r.result);
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

    // Offset the index so that this draw and the yes/no draw above cannot be the
    // same number. They are two decisions about one cell on one step, and the stream
    // tag only separates different streams -- within one, the index is all that
    // distinguishes them.
    const int slot = pick(n, static_cast<uint64_t>(idx) + 0x9E3779B9ull, sim_random::Stream::Emission);

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
        set_element(x, y, ElementType::Water);
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
        case MoveKind::Liquid: step_fluid(x, y, mat, 1); break;
        case MoveKind::Gas:    step_fluid(x, y, mat, -1); break;
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

                for (int x = start_x; x != end_x; x += step) {
                    step_cell(x, y);
                }
            }
        }
    }
}





