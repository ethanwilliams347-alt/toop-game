// Structural falling tests.
//
// Wall and Wood hold their shape, which means they can hold it somewhere they
// should not: dig the ground out from under a slab and it hangs in mid-air.
// An unsupported piece falls as one rigid body, shape intact, which is what
// makes it read as masonry rather than as gravel.
//
// These cover that, and -- just as importantly -- the cases where nothing must
// move, because a piece that falls when it shouldn't destroys a level.

#include "liquid.h"
#include "physics/grid.h"
#include "test_util.h"
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {

void fill(Grid& g, int x0, int y0, int x1, int y1, ElementType t) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            g.set_element(x, y, t);
}

int count_of(const Grid& g, ElementType t) {
    int n = 0;
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < g.get_width(); ++x)
            if (g.get_element(x, y).type == t) n++;
    return n;
}

void step(Grid& g, int n) {
    for (int i = 0; i < n; ++i) g.update();
}

// True if a solid rectangle of `t` sits exactly at (x0,y0)-(x1,y1) and nowhere
// in the ring of cells immediately around it. Asserting the negative space is
// the point: it is what proves the piece kept its shape rather than smearing.
bool rect_exactly_at(const Grid& g, int x0, int y0, int x1, int y1, ElementType t) {
    for (int y = y0 - 1; y <= y1 + 1; ++y) {
        for (int x = x0 - 1; x <= x1 + 1; ++x) {
            const bool inside = (x >= x0 && x <= x1 && y >= y0 && y <= y1);
            if (g.get_element(x, y).type == t) {
                if (!inside) return false;
            } else if (inside) {
                return false;
            }
        }
    }
    return true;
}

// Every cell of type `t`, measured from the corner of its bounding box, sorted.
// Two of these compare equal exactly when the material has the same shape in
// both, wherever it happens to have ended up.
std::vector<std::pair<int, int>> shape_of(const Grid& g, ElementType t) {
    std::vector<std::pair<int, int>> pts;
    int min_x = g.get_width(), min_y = g.get_height();
    for (int y = 0; y < g.get_height(); ++y) {
        for (int x = 0; x < g.get_width(); ++x) {
            if (g.get_element(x, y).type != t) continue;
            pts.emplace_back(x, y);
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
        }
    }
    for (std::pair<int, int>& p : pts) {
        p.first -= min_x;
        p.second -= min_y;
    }
    std::sort(pts.begin(), pts.end());
    return pts;
}

// Puts a structure cell down and takes it straight back out again. The removal
// queues support checks on everything around it, so this disturbs a piece into
// being re-examined without altering the piece itself.
void nudge(Grid& g, int x, int y) {
    g.set_element(x, y, ElementType::Wall);
    g.set_element(x, y, ElementType::Empty);
}

// Top row of the topmost cell of `t`, or -1.
int top_row_of(const Grid& g, ElementType t) {
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < g.get_width(); ++x)
            if (g.get_element(x, y).type == t) return y;
    return -1;
}

} // namespace

int main() {
    // --- an undisturbed structure is never questioned ---
    // The single most important test here. Support is checked on disturbance, never
    // as a global truth, so a platform placed on purpose stays put.
    {
        Grid g(60, 60);
        fill(g, 20, 20, 40, 24, ElementType::Wall);
        step(g, 200);
        check("an untouched floating slab does not move",
              rect_exactly_at(g, 20, 20, 40, 24, ElementType::Wall));
    }

    // --- digging a hole in a grounded structure does not drop it ---
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);
        fill(g, 20, 40, 30, 49, ElementType::Wall);

        g.set_element(25, 45, ElementType::Empty);
        step(g, 60);

        check("a grounded structure stays put when dug",
              g.get_element(20, 40).type == ElementType::Wall &&
              g.get_element(30, 49).type == ElementType::Wall &&
              g.get_element(25, 45).type == ElementType::Empty);
    }

    // --- digging a real hole in a grounded wall does not break pieces off it ---
    // The other tests here erase a single cell. The player's dig erases a whole disc
    // at once, which queues hundreds of support checks spread over several rows, and
    // that is a different thing to ask of the resolver.
    {
        Grid g(60, 60);
        fill(g, 10, 20, 50, 59, ElementType::Wall);  // wall standing on the world floor

        const int hx = 30, hy = 35, r = 3;
        for (int oy = -r; oy <= r; ++oy)
            for (int ox = -r; ox <= r; ++ox)
                if (ox * ox + oy * oy <= r * r)
                    g.set_element(hx + ox, hy + oy, ElementType::Empty);

        step(g, 120);

        bool intact = true;
        for (int y = 20; y <= 59; ++y) {
            for (int x = 10; x <= 50; ++x) {
                const int ox = x - hx, oy = y - hy;
                const bool dug = (ox * ox + oy * oy <= r * r);
                const ElementType want = dug ? ElementType::Empty : ElementType::Wall;
                if (g.get_element(x, y).type != want) intact = false;
            }
        }
        check("digging a hole in a grounded wall leaves the rest of it standing",
              intact,
              std::to_string(count_of(g, ElementType::Wall)) + " wall cells left");
    }

    // --- a round piece keeps its exact shape all the way down and after landing ---
    // The rectangles elsewhere in this file are too forgiving: a rectangle still
    // looks like a rectangle after a row of it slips. A disc has a different number
    // of cells in every row, so any part of it moving without the rest shows up
    // immediately. Landing is included on purpose -- coming to rest on a few contact
    // cells is when a piece is most likely to shed the rest.
    {
        Grid g(34, 26);
        const int cx = 17, cy = 8, r = 6;
        for (int oy = -r; oy <= r; ++oy)
            for (int ox = -r; ox <= r; ++ox)
                if (ox * ox + oy * oy <= r * r)
                    g.set_element(cx + ox, cy + oy, ElementType::Wood);

        // Bite a chunk out of the side, which is both what frees it and what makes
        // the shape asymmetric.
        for (int oy = -3; oy <= 3; ++oy)
            for (int ox = -3; ox <= 3; ++ox)
                if (ox * ox + oy * oy <= 9)
                    g.set_element(12 + ox, 8 + oy, ElementType::Empty);

        const std::vector<std::pair<int, int>> before = shape_of(g, ElementType::Wood);
        step(g, 4);
        check("a round piece is unchanged in shape while falling",
              shape_of(g, ElementType::Wood) == before);

        const int cells_before = count_of(g, ElementType::Wood);
        step(g, 240);  // long enough to reach the floor, roll and settle

        // Landing does not preserve the shape: fracture breaks a piece that arrives
        // with speed on it, and a disc lands on a handful of contact cells, which is
        // the most uneven landing there is. What still has to hold is that it broke
        // rather than smeared -- nothing lost, and no fragment left grinding away
        // against another.
        check("a round piece loses no cells when it lands and breaks",
              count_of(g, ElementType::Wood) == cells_before,
              std::to_string(count_of(g, ElementType::Wood)) + " of " + std::to_string(cells_before));
        check("a round piece settles rather than shedding cells forever",
              !g.has_pending_support_checks());
    }

    // --- a piece resting on one contact cell does not shed the rest of itself ---
    // A big piece touching down on a single cell is the worst case for the support
    // search: one cell of it answers "grounded" and the other few hundred have to be
    // judged from that one answer.
    //
    // It drops a single cell, so it arrives too slowly to break, and it is centred
    // on the stub, so it is balanced there. Anything that moves after that is the
    // support search shedding cells.
    {
        Grid g(60, 60);
        fill(g, 0, 55, 59, 59, ElementType::Wall);  // floor
        g.set_element(30, 54, ElementType::Wall);  // a one-cell stub on top of it
        fill(g, 20, 48, 40, 52, ElementType::Wood);  // slab, one cell above the stub
        fill(g, 20, 53, 20, 54, ElementType::Wood);  // the leg holding it up

        fill(g, 20, 53, 20, 54, ElementType::Empty);  // kick the leg out

        const int before = count_of(g, ElementType::Wood);
        step(g, 300);
        // Where it ends up is the solver's business: a slab that big on a pebble
        // that small may well rock off it onto one end. What may not happen is
        // the search shedding cells, or the slab coming apart over the stub.
        int tag = -1;
        bool one_piece = true;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 60; ++x) {
                const Element e = g.get_element(x, y);
                if (e.type != ElementType::Wood) continue;
                if (tag < 0) tag = e.piece_tag;
                if (e.piece_tag != tag) one_piece = false;
            }
        check("a slab landing on a single stub stays one slab",
              one_piece && count_of(g, ElementType::Wood) == before,
              std::to_string(count_of(g, ElementType::Wood)) + " of " + std::to_string(before));
        check("the stub it landed on is still there",
              g.get_element(30, 54).type == ElementType::Wall);
        check("and the slab comes to rest", !g.has_pending_support_checks());
    }

    // --- the same slab arriving fast breaks over the stub, and loses nothing ---
    // Fracture splits it at the stub; each half is then standing on one edge with
    // its weight hanging off the other side, so it goes over. What must hold is
    // conservation and that it all stops.
    {
        Grid g(60, 60);
        fill(g, 0, 55, 59, 59, ElementType::Wall);
        g.set_element(30, 54, ElementType::Wall);
        fill(g, 20, 40, 40, 44, ElementType::Wood);
        fill(g, 20, 45, 20, 54, ElementType::Wood);
        fill(g, 20, 45, 20, 54, ElementType::Empty);

        const int before = count_of(g, ElementType::Wood);
        step(g, 300);
        check("a slab that breaks over a stub keeps every cell",
              count_of(g, ElementType::Wood) == before,
              std::to_string(count_of(g, ElementType::Wood)) + " of " + std::to_string(before));
        check("and the pieces come to rest", !g.has_pending_support_checks());
    }

    // --- cutting a slab free drops it, shape intact ---
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);  // floor
        fill(g, 20, 30, 30, 34, ElementType::Wall);  // slab, floating
        fill(g, 25, 35, 25, 49, ElementType::Wall);  // the one column holding it up

        const int wall_before = count_of(g, ElementType::Wall);
        step(g, 5);
        check("the propped slab is stable while its support stands",
              rect_exactly_at(g, 20, 30, 30, 34, ElementType::Wall) == false ||
              g.get_element(25, 35).type == ElementType::Wall);

        // Cut the prop out from under it.
        fill(g, 25, 35, 25, 49, ElementType::Empty);

        // It comes loose from rest: nothing on the first step, then faster and
        // faster, as a whole slab at every step.
        step(g, 1);
        check("cut free, the slab comes loose gently rather than lurching a cell",
              rect_exactly_at(g, 20, 30, 30, 34, ElementType::Wall) ||
                  rect_exactly_at(g, 20, 31, 30, 35, ElementType::Wall));

        int top = 30;
        bool whole = true, falling = true;
        for (int s = 0; s < 8; ++s) {
            g.update();
            int now = -1;
            for (int y = 30; y < 50 && now < 0; ++y)
                if (g.get_element(25, y).type == ElementType::Wall) now = y;
            if (now < top) falling = false;
            top = now;
            if (!rect_exactly_at(g, 20, top, 30, top + 4, ElementType::Wall)) whole = false;
        }
        check("it keeps falling on its own without being disturbed again", falling && top > 31,
              "top row " + std::to_string(top));
        check("and stays a whole slab the way down", whole);

        // Not rect_exactly_at() here: the piece has landed on a Wall floor, so there
        // is legitimately Wall directly under it. Check its own cells, and clear air
        // above and to both sides.
        step(g, 200);
        bool resting = true;
        for (int y = 45; y <= 49; ++y) {
            for (int x = 20; x <= 30; ++x)
                if (g.get_element(x, y).type != ElementType::Wall) resting = false;
            if (g.get_element(19, y).type != ElementType::Empty) resting = false;
            if (g.get_element(31, y).type != ElementType::Empty) resting = false;
        }
        for (int x = 20; x <= 30; ++x)
            if (g.get_element(x, 44).type != ElementType::Empty) resting = false;
        check("it comes to rest as a slab sitting on the floor", resting);

        check("nothing was created or destroyed on the way down",
              count_of(g, ElementType::Wall) == wall_before - 15,
              std::to_string(count_of(g, ElementType::Wall)) + " wall");
    }

    // --- a falling piece keeps a non-rectangular shape ---
    // A rectangle would still look right if each column fell independently. An L has
    // to keep its corner to prove the whole piece moves as one.
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);  // floor
        fill(g, 20, 20, 22, 30, ElementType::Wood); // vertical arm
        fill(g, 23, 28, 30, 30, ElementType::Wood); // foot of the L

        g.set_element(20, 20, ElementType::Empty);  // disturb it; nothing is grounded
        const std::vector<std::pair<int, int>> before = shape_of(g, ElementType::Wood);
        step(g, 10);
        check("an L-shaped piece falls with its corner intact",
              shape_of(g, ElementType::Wood) == before && top_row_of(g, ElementType::Wood) > 21,
              "top wood row " + std::to_string(top_row_of(g, ElementType::Wood)));
    }

    // --- Wall and Wood hold each other up ---
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);
        fill(g, 20, 45, 20, 49, ElementType::Wood);  // post standing on the floor
        fill(g, 15, 40, 25, 44, ElementType::Wall);  // slab resting on the post

        g.set_element(25, 40, ElementType::Empty);
        step(g, 30);

        check("a stone slab on a wooden post is supported",
              g.get_element(15, 44).type == ElementType::Wall &&
              g.get_element(20, 45).type == ElementType::Wood);
    }

    // --- powder bears load, liquid does not ---
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);
        fill(g, 20, 45, 30, 49, ElementType::Sand);
        fill(g, 20, 40, 30, 44, ElementType::Wall);
        step(g, 60);  // settle the sand first

        g.set_element(30, 40, ElementType::Empty);
        step(g, 5);

        check("a slab resting on packed sand is supported",
              g.get_element(20, 44).type == ElementType::Wall);
    }
    {
        Grid g(60, 60);
        fill(g, 0, 55, 59, 59, ElementType::Wall);
        fill(g, 20, 45, 30, 54, ElementType::Water);
        fill(g, 20, 40, 30, 44, ElementType::Wall);

        g.set_element(30, 40, ElementType::Empty);
        step(g, 40);

        check("a slab is not held up by water, and sinks through it",
              top_row_of(g, ElementType::Wall) > 40,
              "top wall row " + std::to_string(top_row_of(g, ElementType::Wall)));
        check("the water it displaced was not destroyed",
              liquid::total(g, ElementType::Water) == 11 * 10 * liquid::UNITS_PER_CELL,
              std::to_string(liquid::total(g, ElementType::Water)) + " water");
    }

    // --- a slab drops once the sand under it falls away ---
    // The swap_elements trigger rather than the set_element one: nothing is removed
    // and the slab is never touched, the sand simply leaves.
    {
        Grid g(80, 60);
        fill(g, 0, 50, 79, 59, ElementType::Wall);
        fill(g, 33, 50, 37, 59, ElementType::Empty);  // a shaft through the floor
        fill(g, 35, 46, 35, 49, ElementType::Sand);  // sand pillar over the shaft
        fill(g, 30, 40, 40, 45, ElementType::Wall);  // slab held up by that alone

        check("the slab is up while the sand holds it",
              g.get_element(30, 45).type == ElementType::Wall);

        step(g, 200);

        check("the slab comes down once the sand under it falls away",
              top_row_of(g, ElementType::Wall) > 40,
              "top wall row " + std::to_string(top_row_of(g, ElementType::Wall)));
    }

    // --- a falling piece picks up speed ---
    // At a flat rate everything drifts down the same whatever it is and however
    // far it has fallen, which reads as floating rather than falling. Speed has to
    // start from nothing -- a piece must not jump the moment it comes loose --
    // rise, and never pass the ceiling.
    {
        Grid g(20, 300);
        fill(g, 5, 2, 10, 6, ElementType::Wall);
        nudge(g, 8, 7);  // touch the cell under it, not the piece

        std::vector<int> delta;
        int prev = top_row_of(g, ElementType::Wall);
        for (int i = 0; i < 40; ++i) {
            g.update();
            const int now = top_row_of(g, ElementType::Wall);
            delta.push_back(now - prev);
            prev = now;
        }

        check("a piece comes loose from rest, not at a cell a step", delta.front() <= 1,
              "first step moved " + std::to_string(delta.front()));

        // Speed in whole cells a step can read one less than the step before as
        // the fraction carries over, so "never slows" is judged over pairs.
        bool monotonic = true, capped = true;
        for (size_t i = 0; i < delta.size(); ++i) {
            if (i > 2 && delta[i] + delta[i - 1] < delta[i - 2] + delta[i - 3] - 1)
                monotonic = false;
            if (delta[i] > Grid::MAX_FALL_SPEED) capped = false;
        }
        check("it never slows down on the way", monotonic);
        check("it never travels further in a step than the ceiling allows", capped);
        check("it is falling several cells a step by the end", delta.back() >= 4,
              "last step moved " + std::to_string(delta.back()));
        check("acceleration actually gets it somewhere a flat rate would not",
              prev - 2 > 40,
              "fell " + std::to_string(prev - 2) + " cells in 40 steps");
    }

    // --- a piece at full speed does not step over a floor thinner than its speed ---
    // The reason a fall is eight passes of one cell rather than one pass of eight. A
    // piece moving several cells at a time in a single write would clear a one-cell
    // shelf without ever touching it.
    {
        Grid g(20, 300);
        fill(g, 0, 200, 19, 200, ElementType::Wall);  // shelf, exactly one cell thick
        fill(g, 0, 201, 0, 299, ElementType::Wall);  // leg holding the shelf up
        fill(g, 5, 2, 10, 6, ElementType::Wood);  // the piece, far above it
        nudge(g, 8, 7);

        step(g, 60);  // long enough to reach the shelf at full speed and stop

        check("a piece at full speed lands on a one-cell shelf instead of through it",
              top_row_of(g, ElementType::Wood) == 195,
              "top wood row " + std::to_string(top_row_of(g, ElementType::Wood)));

        bool shelf_intact = true, nothing_below = true;
        for (int x = 0; x < 20; ++x)
            if (g.get_element(x, 200).type != ElementType::Wall) shelf_intact = false;
        for (int y = 201; y <= 299; ++y)
            for (int x = 1; x < 20; ++x)
                if (g.get_element(x, y).type != ElementType::Empty) nothing_below = false;
        check("the shelf it landed on is untouched", shelf_intact);
        check("nothing got past the shelf", nothing_below);

        // --- and the clock resets when it lands ---
        // A piece that kept the speed it landed at would leave the ledge at full
        // pelt the next time it was dug free, minutes later.
        fill(g, 0, 200, 19, 200, ElementType::Empty);  // pull the shelf out
        g.update();
        const int first = top_row_of(g, ElementType::Wood) - 195;
        step(g, 10);
        check("a piece that has landed comes loose from rest again",
              first <= 1 && top_row_of(g, ElementType::Wood) > 196,
              "moved " + std::to_string(first) + " on the first step, top row " +
                  std::to_string(top_row_of(g, ElementType::Wood)) + " ten steps on");
    }

    // --- shape survives the whole speed range ---
    // Everything above about rigid shape was measured at one cell per step. A piece
    // moving eight is re-resolved eight times in a step, so it has eight times as
    // many chances to shear.
    {
        Grid g(40, 300);
        fill(g, 0, 290, 39, 299, ElementType::Wall);  // floor
        const int cx = 20, cy = 10, r = 6;
        for (int oy = -r; oy <= r; ++oy)
            for (int ox = -r; ox <= r; ++ox)
                if (ox * ox + oy * oy <= r * r)
                    g.set_element(cx + ox, cy + oy, ElementType::Wood);

        // Bite a chunk out of the side: frees it, and makes it asymmetric.
        for (int oy = -3; oy <= 3; ++oy)
            for (int ox = -3; ox <= 3; ++ox)
                if (ox * ox + oy * oy <= 9)
                    g.set_element(15 + ox, 10 + oy, ElementType::Empty);

        const std::vector<std::pair<int, int>> before = shape_of(g, ElementType::Wood);
        step(g, 30);  // well past the point where it is moving several cells a step
        check("a round piece is unchanged in shape at full speed",
              shape_of(g, ElementType::Wood) == before,
              "top wood row " + std::to_string(top_row_of(g, ElementType::Wood)));

        const int cells_before = count_of(g, ElementType::Wood);
        step(g, 90);  // reach the floor and settle

        // The landing is not asserted to preserve the shape: a boulder that falls a
        // long way and arrives pristine is the elevator problem in its purest form.
        // What has to survive is everything except the shape -- and on a dead-flat
        // floor the shape may well survive too, since both halves of a break land on
        // the same level. The positive case for fracture is tested against uneven
        // ground further down.
        check("breaking creates and destroys nothing",
              count_of(g, ElementType::Wood) == cells_before,
              std::to_string(count_of(g, ElementType::Wood)) + " of " + std::to_string(cells_before));
        check("the broken pieces come to rest rather than grinding on",
              !g.has_pending_support_checks());
        check("and the wreckage is on the floor",
              top_row_of(g, ElementType::Wood) >= 277,
              "top wood row " + std::to_string(top_row_of(g, ElementType::Wood)));
    }

    // --- fracture: it never starts a collapse, only finishes one unevenly ---
    // The negative case first: a missed fracture is invisible, a wrong one turns a
    // level into rubble. Nothing here is falling, so nothing here may break, and
    // that is guaranteed by fracture being reachable only from a landing with speed
    // on it -- a piece at rest has ticks of zero and never gets near the code.
    {
        Grid g(60, 60);
        fill(g, 0, 55, 59, 59, ElementType::Wall);  // floor
        fill(g, 10, 45, 50, 54, ElementType::Wood);  // a big slab sitting on it

        const std::vector<std::pair<int, int>> before = shape_of(g, ElementType::Wood);
        step(g, 200);
        check("a large piece at rest is never broken",
              shape_of(g, ElementType::Wood) == before);

        // Disturb it hard: dig a hole right through the middle of it. Removing
        // structure is what queues support checks in the first place, so this is the
        // case most likely to reach fracture by accident.
        fill(g, 28, 45, 32, 54, ElementType::Empty);
        step(g, 200);
        bool intact = true;
        for (int y = 45; y <= 54; ++y)
            for (int x = 10; x <= 27; ++x)
                if (g.get_element(x, y).type != ElementType::Wood) intact = false;
        check("digging into a grounded piece does not break the rest of it", intact);
    }

    // --- fracture: a short drop lands intact ---
    // FRACTURE_MIN_TICKS is what separates "it tipped off a ledge" from "it came
    // down". A piece that falls a single cell must arrive whole, or every minor
    // settle in a level turns into rubble.
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);  // floor
        fill(g, 20, 48, 45, 49, ElementType::Wood);  // slab one cell above the floor
        fill(g, 20, 45, 45, 47, ElementType::Wood);
        fill(g, 20, 45, 20, 49, ElementType::Empty);  // free it

        const int cells_before = count_of(g, ElementType::Wood);
        step(g, 200);
        check("a piece that drops one cell onto the floor lands intact",
              rect_exactly_at(g, 21, 45, 45, 49, ElementType::Wood),
              "top wood row " + std::to_string(top_row_of(g, ElementType::Wood)));
        check("and nothing was lost doing it",
              count_of(g, ElementType::Wood) == cells_before);
    }

    // --- fracture: a stone slab dropped onto a step snaps over the drop ---
    // The positive case. Rigid, this slab lands on the high side of the step and
    // stays perfectly level, half of it held out over the drop by the other half.
    // Stone is weak in tension: landing loads that overhang several times its
    // weight, more than its section at the edge can carry, so it snaps there
    // (Grid::snap_overhang) and the half over the low side carries on down. Stone
    // rather than wood because that is which of the two really does this; a
    // wooden slab is strong enough to land whole and stay that way.
    {
        Grid g(80, 80);
        fill(g, 40, 60, 79, 79, ElementType::Wall);  // high ground, right half only
        fill(g, 0, 75, 39, 79, ElementType::Wall);  // low ground, left half
        fill(g, 10, 20, 69, 26, ElementType::Wall);  // a wide stone slab, high up
        // Placing structure deliberately does not queue a support check -- that is
        // what lets the brush draw a floating platform on purpose -- so the slab has
        // to be disturbed before it is asked whether it is standing on anything.
        g.set_element(10, 20, ElementType::Empty);

        // The slab is the stone that is not the ground: nothing of it can end up
        // inside the ground, which stays exactly where it is.
        const auto ground = [](int x, int y) {
            return (x >= 40 && y >= 60) || (x < 40 && y >= 75);
        };
        const auto slab_cells = [&] {
            int n = 0;
            for (int y = 0; y < 80; ++y)
                for (int x = 0; x < 80; ++x)
                    if (!ground(x, y) && g.get_element(x, y).type == ElementType::Wall) ++n;
            return n;
        };
        const int cells_before = slab_cells();
        step(g, 600);

        // Deepest and shallowest row the slab reaches, over the two sides.
        int lowest = -1, highest = 80;
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 80; ++x)
                if (!ground(x, y) && g.get_element(x, y).type == ElementType::Wall) {
                    if (y > lowest) lowest = y;
                    if (y < highest) highest = y;
                }

        check("a stone slab dropped across a step does not stay one rigid level",
              lowest - highest > 6,  // taller than the slab's own 7 rows
              "rows " + std::to_string(highest) + ".." + std::to_string(lowest));
        check("the piece over the drop actually went down it", lowest >= 70,
              "lowest slab row " + std::to_string(lowest));
        check("a crack neither creates nor destroys matter", slab_cells() == cells_before,
              std::to_string(slab_cells()) + " of " + std::to_string(cells_before));
        check("the wreckage comes to rest", !g.has_pending_support_checks());
    }

    // --- a wooden slab dropped onto the same step lands whole ---
    // The other half of the claim: what snaps stone does not snap wood.
    {
        Grid g(80, 80);
        fill(g, 40, 60, 79, 79, ElementType::Wall);
        fill(g, 0, 75, 39, 79, ElementType::Wall);
        fill(g, 10, 20, 69, 26, ElementType::Wood);
        g.set_element(10, 20, ElementType::Empty);
        const int before = count_of(g, ElementType::Wood);
        step(g, 600);
        std::vector<int> tags;
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 80; ++x) {
                const Element e = g.get_element(x, y);
                if (e.type == ElementType::Wood &&
                    std::find(tags.begin(), tags.end(), e.piece_tag) == tags.end())
                    tags.push_back(e.piece_tag);
            }
        check("a wooden slab dropped across a step lands in one piece", tags.size() == 1,
              std::to_string(tags.size()) + " pieces");
        check("and keeps every cell", count_of(g, ElementType::Wood) == before);
    }

    // --- a piece too large to judge is left alone ---
    // Guards the budget's failure direction: over the cap, assume supported. A
    // missed fall is invisible; a wrong one destroys the level.
    {
        const int w = 200, h = 200;  // far over MAX_SUPPORT_CELLS
        Grid g(w, h);
        fill(g, 0, 0, w - 1, h - 2, ElementType::Wall);  // huge, and not grounded
        const int before = count_of(g, ElementType::Wall);

        g.set_element(100, 100, ElementType::Empty);
        step(g, 2);

        check("a piece over the size budget is assumed supported",
              count_of(g, ElementType::Wall) == before - 1 &&
              g.get_element(0, 0).type == ElementType::Wall);
    }

    // ------------------------------------------------------------------------
    // Toppling. A piece that is held up, but whose centre of mass is not over
    // what is holding it, pivots over its last supporting edge until it hits
    // something.
    // ------------------------------------------------------------------------

    // --- a post landing on the lip of a ledge goes over the edge ---
    // The case that used to be an elevator: the post came straight down, touched
    // the ledge with one column and stood there, two thirds of it over nothing.
    {
        Grid g(80, 80);
        fill(g, 0, 60, 30, 79, ElementType::Wall);  // a ledge, top at row 60, edge at x=30
        fill(g, 31, 75, 79, 79, ElementType::Wall);  // low ground beyond it
        fill(g, 30, 30, 32, 44, ElementType::Wood);  // a 3x15 post above the lip
        g.set_element(31, 29, ElementType::Wall);
        g.set_element(31, 29, ElementType::Empty);   // disturb it so it is asked

        const int before = count_of(g, ElementType::Wood);
        bool ever_tipped = false;
        for (int i = 0; i < 400; ++i) {
            g.update();
            if (g.tipping_count() > 0) ever_tipped = true;
        }

        int min_x = 80, max_x = -1, min_y = 80, max_y = -1;
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 80; ++x)
                if (g.get_element(x, y).type == ElementType::Wood) {
                    min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                    min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                }

        check("a post landing on a ledge's lip tips rather than standing on one column",
              ever_tipped);
        check("it ends up lying down, not standing up",
              max_x - min_x > max_y - min_y,
              "box " + std::to_string(max_x - min_x + 1) + "x" + std::to_string(max_y - min_y + 1));
        check("it went over the edge, away from the ledge", max_x > 40,
              "rightmost wood x " + std::to_string(max_x));
        check("tipping creates and destroys nothing",
              count_of(g, ElementType::Wood) == before,
              std::to_string(count_of(g, ElementType::Wood)) + " of " + std::to_string(before));
        check("and it comes to rest", !g.has_pending_support_checks());
    }

    // --- digging out under one side of a post tips it into the hole ---
    // The at-rest case. The post is wood and the floor is stone: the support fill
    // welds them into one piece, which is right for holding things up, but nobody
    // glued the post down, so balance is judged on the post alone.
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);   // floor
        fill(g, 30, 35, 32, 49, ElementType::Wood);  // 3x15 post standing on it
        step(g, 10);
        check("a post standing squarely on the floor stays up",
              rect_exactly_at(g, 30, 35, 32, 49, ElementType::Wood));

        // Dig a pit under the left two columns and away to the left, leaving the
        // post on its right-hand column alone.
        fill(g, 14, 50, 31, 57, ElementType::Empty);

        const int before = count_of(g, ElementType::Wood);
        step(g, 400);

        int min_x = 60, top = 60;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 60; ++x)
                if (g.get_element(x, y).type == ElementType::Wood) {
                    min_x = std::min(min_x, x);
                    top = std::min(top, y);
                }
        check("a post undermined on one side falls over towards the hole",
              min_x < 25 && top > 40,
              "leftmost x " + std::to_string(min_x) + ", top row " + std::to_string(top));
        check("and keeps every cell doing it", count_of(g, ElementType::Wood) == before);
        check("and stops", !g.has_pending_support_checks());
    }

    // --- undermined, but still balanced: it stays put ---
    // The negative case, and the one that matters more: a wrong topple wrecks a
    // level. The centre of mass is over what is left, so nothing moves.
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);
        fill(g, 30, 35, 32, 49, ElementType::Wood);
        fill(g, 20, 50, 30, 57, ElementType::Empty);  // only the leftmost column undermined
        step(g, 200);
        check("a post with its centre still over its footing does not tip",
              rect_exactly_at(g, 30, 35, 32, 49, ElementType::Wood) && g.tipping_count() == 0);
    }

    // --- a plank tipping off the end of a post comes down, it does not hang ---
    // The frozen cantilever. A long plank's centre starts a cell or so above the
    // corner it turns about and forty cells out, so it is "below the pivot" after
    // about a degree. The release slid it one cell outward, which still left its
    // end on a post three cells wide; support then found a piece held up with its
    // centre below its corner, refused to tip it, and it hung there, tilted, in the
    // air. burn_probe's plank on two posts did this whenever it burnt through.
    {
        Grid g(80, 40);
        fill(g, 60, 25, 62, 39, ElementType::Wall);  // a post, top at row 25
        fill(g, 10, 22, 62, 24, ElementType::Wood);  // 53x3, only its end on the post
        nudge(g, 30, 21);

        const int before = count_of(g, ElementType::Wood);
        step(g, 600);

        int lowest = -1;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 80; ++x)
                if (g.get_element(x, y).type == ElementType::Wood) lowest = y;
        check("a plank tipping off a post swings down to the floor", lowest == 39,
              "lowest wood row " + std::to_string(lowest) + " of 39");
        check("and keeps every cell", count_of(g, ElementType::Wood) == before);
        check("and comes to rest", g.tipping_count() == 0 && !g.has_pending_support_checks());
    }

    // --- a piece hooked over a ledge, with nowhere to go, is left alone ---
    // Its centre is level with the corner it would turn about, so the first sliver
    // of a turn reads as "below the pivot", and the release cannot slide it clear
    // because a pillar is in the way. That used to end the turn there, unmoved, and
    // requeue it -- so the same topple started again the next step, and every step
    // after, each time under a new piece tag. Now it has to have turned before it
    // is released, and it cannot turn: it is wedged and stays exactly as it is.
    {
        Grid g(40, 40);
        fill(g, 0, 20, 9, 39, ElementType::Wall);    // a ledge, corner at (10, 20)
        fill(g, 12, 20, 12, 39, ElementType::Wall);  // a pillar two cells beyond it
        g.set_element(9, 19, ElementType::Wood);     // on the ledge
        g.set_element(10, 19, ElementType::Wood);
        g.set_element(10, 20, ElementType::Wood);    // against the ledge's face
        g.set_element(11, 20, ElementType::Wood);    // against the pillar
        nudge(g, 9, 18);
        step(g, 120);

        std::vector<Element> settled;
        for (int y = 0; y < 40; ++y)
            for (int x = 0; x < 40; ++x) settled.push_back(g.get_element(x, y));
        int changed = 0;
        for (int s = 0; s < 120; ++s) {
            g.update();
            for (int y = 0, i = 0; y < 40; ++y)
                for (int x = 0; x < 40; ++x, ++i) {
                    const Element e = g.get_element(x, y);
                    if (e.type != settled[i].type || e.piece_tag != settled[i].piece_tag) {
                        ++changed;
                        settled[i] = e;
                    }
                }
        }
        check("a wedged hook does not restart a topple every step", changed == 0,
              std::to_string(changed) + " cell changes after it settled");
        check("and keeps every cell", count_of(g, ElementType::Wood) == 4);
    }

    // --- a piece tipping into water moves the water, it does not delete it ---
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);
        fill(g, 30, 35, 32, 49, ElementType::Wood);
        fill(g, 10, 50, 31, 57, ElementType::Empty);
        fill(g, 10, 52, 31, 57, ElementType::Water);  // a pool in the pit

        const int64_t water = liquid::total(g, ElementType::Water);
        const int wood = count_of(g, ElementType::Wood);
        g.set_element(31, 50, ElementType::Empty);  // and undermine the post
        step(g, 400);
        check("tipping into a pool conserves the water",
              liquid::total(g, ElementType::Water) == water,
              std::to_string(liquid::total(g, ElementType::Water)) + " of " +
                  std::to_string(water));
        check("and the wood", count_of(g, ElementType::Wood) == wood);
    }

    // --- wood that catches stays in the piece it was part of ---
    // Wood catching into Charred is a reaction, and reactions write through place(),
    // which starts every cell as a piece of its own with tag 0. On untagged wood
    // that changes nothing. On wood that has tipped, landed or broken, and so wears
    // a tag, each cell that caught came away from its piece. Hanging in the air,
    // it fell off and took the fire with it. burn_probe's plank on two posts
    // stalls that way: it burns through, the far part tips about its post and is
    // tagged, and the fire never crosses it. That was 7 seeds of 12.
    //
    // The same scene, smaller, checked at the cause rather than the symptom: at
    // every step, each tagged cell that was Wood and is now Charred wears the same
    // tag as a neighbour that was the same piece's Wood and still is. That, rather
    // than "the tag it had", because a piece can be re-tagged as a whole in the
    // step a cell of it catches -- it starts tipping, or breaks -- and the cell
    // should go with it. Several worlds, because which part of the beam tips
    // first, and whether it is still burning when it does, depends on the seed.
    {
        constexpr int LEN = 80, THICK = 3, GW = LEN + 20, GH = THICK + 40, X0 = 10, Y0 = 10;
        int ignitions = 0, tagged = 0, kept = 0;
        for (uint64_t seed = 1; seed <= 4; ++seed) {
            Grid g(GW, GH, seed);
            for (int i = 0; i < 3; ++i) {
                fill(g, X0 + i, Y0 + THICK, X0 + i, GH - 1, ElementType::Wall);
                fill(g, X0 + LEN - 1 - i, Y0 + THICK, X0 + LEN - 1 - i, GH - 1, ElementType::Wall);
            }
            fill(g, X0, Y0, X0 + LEN - 1, Y0 + THICK - 1, ElementType::Wood);
            fill(g, X0, Y0, X0, Y0 + THICK - 1, ElementType::Charred);

            std::vector<int> wood_tag(static_cast<size_t>(GW) * GH);
            for (int s = 0; s < 1500; ++s) {
                for (int y = 0; y < GH; ++y)
                    for (int x = 0; x < GW; ++x) {
                        const Element e = g.get_element(x, y);
                        wood_tag[static_cast<size_t>(y) * GW + x] =
                            e.type == ElementType::Wood ? e.piece_tag : -1;
                    }
                g.update();
                for (int y = 0; y < GH; ++y)
                    for (int x = 0; x < GW; ++x) {
                        const int was = wood_tag[static_cast<size_t>(y) * GW + x];
                        const Element e = g.get_element(x, y);
                        if (was < 0 || e.type != ElementType::Charred) continue;
                        ++ignitions;
                        if (was == 0) continue;

                        // The piece's tag now, read off an unburnt neighbour of it.
                        int piece = -1;
                        for (int dy = -1; dy <= 1 && piece < 0; ++dy)
                            for (int dx = -1; dx <= 1 && piece < 0; ++dx) {
                                const int nx = x + dx, ny = y + dy;
                                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= GW ||
                                    ny >= GH)
                                    continue;
                                const Element n = g.get_element(nx, ny);
                                if (wood_tag[static_cast<size_t>(ny) * GW + nx] == was &&
                                    n.type == ElementType::Wood)
                                    piece = n.piece_tag;
                            }
                        if (piece < 0)
                            continue;  // nothing of its piece left beside it to compare with
                        ++tagged;
                        if (e.piece_tag == piece) ++kept;
                    }
            }
        }
        check("a beam that burns through tips, and its tagged wood catches", tagged > 0,
              std::to_string(tagged) + " of " + std::to_string(ignitions) +
                  " ignitions on tagged wood");
        check("every cell that catches stays in its piece", kept == tagged,
              std::to_string(kept) + " of " + std::to_string(tagged) + " stayed in their piece");
    }

    // --- toppling is deterministic ---
    // It is the one part of the simulation with angles in it, so it is the one most
    // likely to hide a float or an order dependence. Same writes, same world.
    {
        const auto run = [] {
            Grid g(80, 80, 1234);
            fill(g, 0, 60, 30, 79, ElementType::Wall);
            fill(g, 31, 75, 79, 79, ElementType::Wall);
            fill(g, 30, 30, 32, 44, ElementType::Wood);
            fill(g, 20, 20, 27, 23, ElementType::Wood);  // a second, overlapping topple
            nudge(g, 31, 29);
            nudge(g, 21, 19);
            step(g, 300);
            return g.get_pixels();
        };
        check("two runs of the same topple end pixel-identical", run() == run());
    }

    // ------------------------------------------------------------------------
    // Loose bodies. A piece that topples is a rigid body with momentum: it
    // pushes what it hits, carries what it is carrying, floats if it is lighter
    // than what it is in, and slides down what is steeper than its grip. Each of
    // these was impossible while toppling was a hinge about one corner.
    // tests/body_probe.cpp shows the same scenes as pictures.
    // ------------------------------------------------------------------------

    // --- a row of posts goes down like dominoes ---
    // Only the first is off balance. Every other one is knocked over by the one
    // before it, which takes a body striking, or leaning on, a piece at rest
    // and waking it.
    {
        Grid g(120, 40);
        fill(g, 0, 34, 119, 39, ElementType::Wall);
        for (int i = 0; i < 8; ++i) fill(g, 10 + i * 10, 18, 11 + i * 10, 33, ElementType::Wood);
        fill(g, 12, 18, 17, 20, ElementType::Wood);  // a head that puts the first past its foot
        nudge(g, 11, 17);

        const int before = count_of(g, ElementType::Wood);
        int most = 0;
        for (int s = 0; s < 900; ++s) {
            g.update();
            most = std::max(most, g.tipping_count());
        }
        bool last_down = true;
        for (int y = 18; y < 24; ++y)
            for (int x = 80; x <= 81; ++x)
                if (g.get_element(x, y).type == ElementType::Wood) last_down = false;
        check("every domino in the row is knocked over", last_down && most == 8,
              std::to_string(most) + " bodies at most");
        check("and the row keeps every cell", count_of(g, ElementType::Wood) == before);
        check("and comes to rest", !g.has_pending_support_checks());
    }

    // --- a plank across a post tips under sand poured on one end ---
    // Load is weighed with the piece: the sand is not part of the plank, but it
    // is resting on it, and that is what moves the plank's centre past the post.
    {
        Grid g(80, 40);
        fill(g, 0, 36, 79, 39, ElementType::Wall);
        fill(g, 38, 30, 41, 35, ElementType::Wall);  // fulcrum
        fill(g, 15, 28, 64, 29, ElementType::Wood);  // balanced across it
        nudge(g, 40, 27);
        step(g, 60);
        check("a plank balanced on a post stands until it is loaded",
              rect_exactly_at(g, 15, 28, 64, 29, ElementType::Wood));

        for (int s = 0; s < 240; ++s) {
            if (s % 2 == 0)
                for (int x = 56; x <= 62; ++x) g.set_element(x, 2, ElementType::Sand);
            g.update();
        }
        step(g, 300);
        int left = -1, right = -1;  // lowest wood row at each end
        for (int y = 0; y < 40; ++y) {
            if (g.get_element(16, y).type == ElementType::Wood) left = y;
            if (g.get_element(63, y).type == ElementType::Wood) right = y;
        }
        check("sand heaped on one end tips the plank down on that end", right > left + 4,
              "left end row " + std::to_string(left) + ", right end row " + std::to_string(right));
    }

    // --- wood floats; stone sinks ---
    {
        Grid g(80, 50);
        fill(g, 0, 45, 79, 49, ElementType::Wall);
        fill(g, 0, 20, 0, 44, ElementType::Wall);
        fill(g, 79, 20, 79, 44, ElementType::Wall);
        fill(g, 1, 25, 78, 44, ElementType::Water);
        fill(g, 15, 5, 26, 10, ElementType::Wood);
        fill(g, 50, 5, 57, 10, ElementType::Wall);
        nudge(g, 20, 4);
        nudge(g, 53, 4);

        const int64_t water = liquid::total(g, ElementType::Water);
        const int wood = count_of(g, ElementType::Wood);
        step(g, 900);

        int wood_top = -1, wood_bottom = -1, stone_bottom = -1;
        for (int y = 0; y < 45; ++y)
            for (int x = 1; x < 79; ++x) {
                const ElementType t = g.get_element(x, y).type;
                if (t == ElementType::Wood) {
                    if (wood_top < 0) wood_top = y;
                    wood_bottom = y;
                }
                if (t == ElementType::Wall) stone_bottom = y;
            }
        check("a wooden block dropped in a pool floats with its top out of the water",
              wood_top >= 0 && wood_top < 25 && wood_bottom >= 25,
              "wood rows " + std::to_string(wood_top) + ".." + std::to_string(wood_bottom));
        check("a stone block dropped in the same pool sinks to the bottom", stone_bottom == 44,
              "lowest stone row " + std::to_string(stone_bottom));
        check("floating and sinking conserve the water",
              liquid::total(g, ElementType::Water) == water);
        check("and the wood", count_of(g, ElementType::Wood) == wood);
        check("and both come to rest", !g.has_pending_support_checks());
    }

    // --- a stack that leans past its base comes down ---
    // Every block is balanced on the one under it; the stack as a whole is not.
    // Only weighing each block with everything resting on it finds that.
    {
        Grid g(60, 50);
        fill(g, 0, 45, 59, 49, ElementType::Wall);
        for (int i = 0; i < 6; ++i) {
            const ElementType t = i % 2 == 0 ? ElementType::Wood : ElementType::Wall;
            fill(g, 20 + 2 * i, 40 - 5 * i, 27 + 2 * i, 44 - 5 * i, t);
        }
        const int wood = count_of(g, ElementType::Wood);
        nudge(g, 31, 14);
        step(g, 900);
        check("a stack leaning past its base does not stay as it was built",
              !rect_exactly_at(g, 30, 15, 37, 19, ElementType::Wall));
        check("and keeps every cell", count_of(g, ElementType::Wood) == wood);
        check("and comes to rest", !g.has_pending_support_checks());
    }

    // --- a block dropped on a slope goes down it ---
    // The block lands on one corner of a staircase. It used to hang there by
    // that corner -- the support fill joins diagonal neighbours -- and a body
    // would have caught on each step of the staircase without normals read off
    // the slope's shape.
    {
        Grid g(80, 60);
        for (int x = 0; x < 80; ++x)
            fill(g, x, 59 - (x < 50 ? 50 - x : 0), x, 59, ElementType::Wall);
        fill(g, 12, 10, 17, 13, ElementType::Wood);
        nudge(g, 14, 9);
        step(g, 600);
        int min_x = 80;
        for (int y = 0; y < 60; ++y)
            for (int x = 0; x < 80; ++x)
                if (g.get_element(x, y).type == ElementType::Wood) min_x = std::min(min_x, x);
        check("a block dropped on a slope ends at its foot", min_x >= 45,
              "leftmost wood x " + std::to_string(min_x));
        check("and keeps every cell", count_of(g, ElementType::Wood) == 24);
        check("and comes to rest", !g.has_pending_support_checks());
    }

    // --- bodies replay exactly ---
    // Contacts, impulses and buoyancy are where a float or an order dependence
    // would hide, so the busiest scene is run twice.
    {
        const auto run = [] {
            Grid g(120, 40, 77);
            fill(g, 0, 34, 119, 39, ElementType::Wall);
            for (int i = 0; i < 8; ++i)
                fill(g, 10 + i * 10, 18, 11 + i * 10, 33, ElementType::Wood);
            fill(g, 12, 18, 17, 20, ElementType::Wood);
            fill(g, 95, 25, 118, 33, ElementType::Water);
            nudge(g, 11, 17);
            step(g, 400);
            return g.get_pixels();
        };
        check("two runs of the same domino run end pixel-identical", run() == run());
    }

    return report();
}
