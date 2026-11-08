// Player physics tests.
//
// The player reads the grid but never writes it, so these run headlessly for the
// same reason the grid tests do. Every scenario steps the grid alongside the
// player rather than freezing it, because a player standing on terrain that is
// still settling is the normal case, not the exception.
//
// Every dimension below is derived from Player::WIDTH / HEIGHT /
// MAX_STEP_HEIGHT rather than written as a number. As literals they do not
// merely need updating when the body is resized -- some of them silently change
// what is being tested. A "four-cell ledge" case asserting that a ledge taller
// than MAX_STEP_HEIGHT stops the player becomes, at a step height of 5, a ledge
// the player is supposed to walk up: the test keeps its name, flips its meaning,
// and fails for the right reason by accident. Standing spawns written as
// literals put the body's feet below the floor at a larger size, so several
// scenarios begin buried and pass through resolve_overlap() instead of through
// the physics they name.

#include "physics/player.h"
#include "test_util.h"
#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

namespace {

// A world roomy enough for the body at any plausible size: tall enough for a
// long fall above the floor, wide enough that a second of walking at MOVE_SPEED
// never reaches the far border.
constexpr int WORLD_W = 8 * Player::WIDTH + 240;
constexpr int WORLD_H = 5 * Player::HEIGHT;
constexpr int FLOOR_Y = WORLD_H - Player::HEIGHT;

// Spawn heights. STAND_Y drops the body a short way onto the floor -- enough
// that it is genuinely falling, not so far that it is still in the air after the
// settle steps. AIR_Y is the long-drop spawn.
constexpr int STAND_Y = FLOOR_Y - Player::HEIGHT - Player::HEIGHT / 2;
constexpr int AIR_Y = 2;

// Where obstacles go: far enough right that the player is at full speed when it
// arrives, with room beyond to keep walking afterwards.
constexpr int OBSTACLE_X = WORLD_W / 2;

void step(Grid& g, Player& p, const PlayerInput& in, int n) {
    for (int i = 0; i < n; ++i) {
        g.update();
        p.update(g, in);
    }
}

// A world with a solid floor whose top surface is row `floor_y`.
Grid make_world(int w, int h, int floor_y) {
    Grid g(w, h);
    for (int y = floor_y; y < h; ++y)
        for (int x = 0; x < w; ++x)
            g.set_element(x, y, ElementType::Wall);
    return g;
}

// Where the player's feet are, as a row index. Equal to floor_y when standing on
// that floor, which makes the resting position exactly assertable -- the point
// of storing position as whole cells plus a remainder.
int feet(const Player& p) { return p.cell_y() + Player::HEIGHT; }

// The body's drawn position: the whole cell plus the sub-cell remainder, the
// conversion present::player_visual makes for the renderer. Written out here
// because this suite links the simulation alone.
float visual_x(const Player& p) {
    return static_cast<float>(p.cell_x()) + fx::to_float(p.remainder_x());
}
float visual_y(const Player& p) {
    return static_cast<float>(p.cell_y()) + fx::to_float(p.remainder_y());
}

std::string feet_detail(const Player& p) {
    return "feet=" + std::to_string(feet(p)) + " x=" + std::to_string(p.cell_x());
}

const PlayerInput NOTHING{};

PlayerInput held_right() {
    PlayerInput in;
    in.right = true;
    return in;
}

// --- for the loose-body scenarios at the end ---

void fill(Grid& g, int x0, int y0, int x1, int y1, ElementType t) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) g.set_element(x, y, t);
}

// Puts a structure cell down and takes it straight back out, which queues the
// piece around it to be asked what holds it up (test_collapse.cpp's nudge).
void nudge(Grid& g, int x, int y) {
    g.set_element(x, y, ElementType::Wall);
    g.set_element(x, y, ElementType::Empty);
}

// step(), with the pushes the body makes on loose bodies queued after it, the
// way Run::step queues them.
void step_coupled(Grid& g, Player& p, const PlayerInput& in, int n) {
    for (int i = 0; i < n; ++i) {
        g.update();
        p.update(g, in);
        for (int k = 0; k < p.push_count(); ++k) {
            const BoxPush& q = p.push(k);
            g.queue_impulse(q.x, q.y, q.jx, q.jy);
        }
    }
}

int left_of(const Grid& g, ElementType t) {
    for (int x = 0; x < g.get_width(); ++x)
        for (int y = 0; y < g.get_height(); ++y)
            if (g.get_element(x, y).type == t) return x;
    return -1;
}

int top_of(const Grid& g, ElementType t) {
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < g.get_width(); ++x)
            if (g.get_element(x, y).type == t) return y;
    return -1;
}

} // namespace

int main() {
    // --- gravity, and coming to rest exactly on the surface ---
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        Player p(10, AIR_Y);

        const int start_y = p.cell_y();
        step(g, p, NOTHING, 10);
        check("player falls under gravity", p.cell_y() > start_y, feet_detail(p));

        step(g, p, NOTHING, 200);
        check("player lands exactly on the floor surface", feet(p) == FLOOR_Y, feet_detail(p));
        check("player reports on_ground once landed", p.is_on_ground());
        check("a landed player is not inside terrain", !p.overlaps_solid(g, p.cell_x(), p.cell_y()));
    }

    // --- no tunnelling, even at terminal velocity through a one-cell floor ---
    // Movement resolves one cell at a time, so this is meant to be impossible by
    // construction rather than by being fast enough. The drop is stated in terms of
    // MAX_FALL_SPEED rather than as a fixed height, which is what keeps it a
    // terminal-velocity test after a retune.
    {
        // fx::trunc rather than a cast: the speeds are fixed point, and a plain
        // cast of one would be a drop of millions of cells.
        const int drop = fx::trunc(Player::MAX_FALL_SPEED) * 2;
        const int thin_floor_y = drop + Player::HEIGHT;
        Grid g(WORLD_W, thin_floor_y + 4);
        for (int x = 0; x < WORLD_W; ++x) g.set_element(x, thin_floor_y, ElementType::Wall);

        Player p(10, 0);
        step(g, p, NOTHING, 400);
        check("player does not tunnel through a one-cell floor", feet(p) == thin_floor_y,
              feet_detail(p));
    }

    // --- walking ---
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);

        const int start_x = p.cell_x();
        step(g, p, held_right(), 60);  // one second of walking

        // Most of a second's worth of MOVE_SPEED, not a fixed cell count: the slack
        // absorbs the steps spent landing, and the claim survives a retune of
        // MOVE_SPEED, which a literal would not.
        const int expected = fx::trunc(Player::MOVE_SPEED) * 66 / 100;
        check("player walks along flat ground", p.cell_x() - start_x > expected,
              "moved=" + std::to_string(p.cell_x() - start_x) + " cells, wanted >" +
                  std::to_string(expected));
    }

    // --- a wall stops the player, and is not passed through ---
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        for (int y = FLOOR_Y - Player::HEIGHT - 2; y < FLOOR_Y; ++y)
            g.set_element(OBSTACLE_X, y, ElementType::Wall);

        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);
        step(g, p, held_right(), 300);

        check("player stops flush against a wall", p.cell_x() + Player::WIDTH == OBSTACLE_X,
              feet_detail(p));
        check("player does not pass through a wall", !p.overlaps_solid(g, p.cell_x(), p.cell_y()));
    }

    // --- a one-cell lip is a step, not a wall ---
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        g.set_element(OBSTACLE_X, FLOOR_Y - 1, ElementType::Wall);
        g.set_element(OBSTACLE_X + 1, FLOOR_Y - 1, ElementType::Wall);

        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);
        step(g, p, held_right(), 300);

        check("player steps up over a one-cell lip", p.cell_x() > OBSTACLE_X + 2, feet_detail(p));
    }

    // --- a ledge of exactly MAX_STEP_HEIGHT is still a step ---
    // The upper edge of the promise. Without it, MAX_STEP_HEIGHT could be off by one
    // in the blocking direction and only the test below would notice, which would
    // still pass.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        for (int y = FLOOR_Y - Player::MAX_STEP_HEIGHT; y < FLOOR_Y; ++y)
            for (int x = OBSTACLE_X; x < OBSTACLE_X + 4; ++x)
                g.set_element(x, y, ElementType::Wall);

        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);
        step(g, p, held_right(), 300);

        check("player steps up a ledge of exactly MAX_STEP_HEIGHT",
              p.cell_x() > OBSTACLE_X + 4, feet_detail(p));
    }

    // --- but a ledge one cell taller than MAX_STEP_HEIGHT is not ---
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        for (int y = FLOOR_Y - Player::MAX_STEP_HEIGHT - 1; y < FLOOR_Y; ++y)
            g.set_element(OBSTACLE_X, y, ElementType::Wall);

        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);
        step(g, p, held_right(), 300);

        check("player cannot step up a ledge taller than MAX_STEP_HEIGHT",
              p.cell_x() + Player::WIDTH == OBSTACLE_X, feet_detail(p));
    }

    // --- jumping ---
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);

        const int rest_y = p.cell_y();

        PlayerInput jump;
        jump.jump = true;
        step(g, p, jump, 1);

        int highest = p.cell_y();
        for (int i = 0; i < 120; ++i) {
            step(g, p, NOTHING, 1);
            if (p.cell_y() < highest) highest = p.cell_y();
        }

        // One body height of clearance. JUMP_SPEED against GRAVITY gives about one
        // and a half, so this asserts the jump is useful rather than asserting the
        // exact arc, which is tuning and not a promise.
        check("jump lifts the player clear of the ground", rest_y - highest >= Player::HEIGHT,
              "peak=" + std::to_string(rest_y - highest) + " cells");
        check("player lands back on the ground after a jump",
              p.cell_y() == rest_y && p.is_on_ground(), feet_detail(p));
    }

    // --- liquids are not solid: the player sinks through a pool ---
    // The pool is a full body deep, so this is a submerged body rather than a
    // shallow puddle the feet happen to clear.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        for (int y = FLOOR_Y - Player::HEIGHT; y < FLOOR_Y; ++y)
            for (int x = 0; x < WORLD_W; ++x)
                g.set_element(x, y, ElementType::Water);

        Player p(WORLD_W / 2, AIR_Y);
        step(g, p, NOTHING, 300);
        check("player falls through water rather than standing on it",
              feet(p) == FLOOR_Y && p.is_on_ground(), feet_detail(p));
    }

    // --- powder is solid: the player walks over it instead of through it ---
    // The slab is left to slump into its own settled slope first, so this is a walk
    // over real uneven powder rather than over a flat block that happens to be made
    // of sand. Sized to the body: a pile shorter than the step height is one the
    // player steps over without ever climbing it.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        const int pile_h = Player::MAX_STEP_HEIGHT + 3;
        const int pile_end = OBSTACLE_X + 6 * Player::WIDTH;
        for (int y = FLOOR_Y - pile_h; y < FLOOR_Y; ++y)
            for (int x = OBSTACLE_X; x < pile_end; ++x)
                g.set_element(x, y, ElementType::Sand);

        Player p(10, STAND_Y);
        step(g, p, NOTHING, 200);  // player lands, sand settles

        bool walked_above_floor = false;
        const PlayerInput right = held_right();
        for (int i = 0; i < 300; ++i) {
            step(g, p, right, 1);
            if (feet(p) < FLOOR_Y) walked_above_floor = true;
        }

        check("player walks over settled powder, not through it", walked_above_floor,
              feet_detail(p));
        check("player crosses the powder and keeps going", p.cell_x() > pile_end, feet_detail(p));
        check("player does not end up inside the powder",
              !p.overlaps_solid(g, p.cell_x(), p.cell_y()));
    }

    // --- a buried player digs itself out instead of freezing ---
    // The grid does not know the player exists, so terrain can and will end up
    // inside the body. Without resolve_overlap() every direction is blocked and the
    // player is stuck for the rest of the run.
    //
    // The body is buried deeper than MAX_UNSTUCK_RADIUS can see on purpose: that is
    // the grind-upward path at the end of resolve_overlap(), and it is the one that
    // has to hold when a collapse drops a hillside on the player. The step count has
    // to cover that grind, one cell per step.
    {
        const int surface = Player::HEIGHT;
        Grid g(WORLD_W, WORLD_H);
        for (int y = surface; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x)
                g.set_element(x, y, ElementType::Wall);

        Player p(10, surface + 2 * Player::HEIGHT);  // fully inside the block
        check("a buried player starts out overlapping terrain",
              p.overlaps_solid(g, p.cell_x(), p.cell_y()));

        step(g, p, NOTHING, 4 * Player::HEIGHT + 60);
        check("a buried player pushes itself back out of terrain",
              !p.overlaps_solid(g, p.cell_x(), p.cell_y()), feet_detail(p));
    }

    // --- the grind stops at a ceiling the body was not inside ---
    // Buried in sand deeper than the search reaches, under a wall slab. Grinding up
    // through the sand is the escape; grinding on up through the slab would be a
    // way through terrain, the same crossing the ring search refuses below. The
    // body stops under the slab, where the dig tool can open the way.
    {
        constexpr int CEILING_Y = 10;
        constexpr int SAND_TOP = CEILING_Y + 1;
        Grid g(WORLD_W, WORLD_H);
        for (int x = 0; x < WORLD_W; ++x) {
            for (int y = 0; y <= CEILING_Y; ++y) g.set_element(x, y, ElementType::Wall);
            for (int y = SAND_TOP; y < WORLD_H; ++y) g.set_element(x, y, ElementType::Sand);
        }
        const int start_y = SAND_TOP + 2 * Player::MAX_UNSTUCK_RADIUS + Player::HEIGHT;
        Player p(10, start_y);
        step(g, p, NOTHING, start_y + 60);
        check("a body buried in sand grinds up through the sand", p.cell_y() < start_y,
              feet_detail(p));
        check("...and stops at a ceiling rather than grinding through it", p.cell_y() >= SAND_TOP,
              feet_detail(p));
    }

    // --- the escape must be reachable, not merely empty ---
    // The ring search tests the destination box and never the path to it, so a body
    // buried against a wall can be relocated to the far side of that wall: being
    // poured on becomes a way through terrain. A containment failure, not a cosmetic
    // one.
    //
    // The geometry is chosen so the bug is the cheaper answer and the search
    // therefore prefers it. Climbing out of the sand costs SAND_DEPTH rings;
    // stepping clear of a one-cell wall the body is flush against costs WIDTH + 1.
    // Make the sand the deeper of the two and the first free position the ring
    // reaches is across the wall.
    {
        constexpr int WALL_X = OBSTACLE_X;
        constexpr int SAND_DEPTH = Player::WIDTH + 4;
        constexpr int SAND_TOP = FLOOR_Y - SAND_DEPTH;
        static_assert(SAND_DEPTH > Player::WIDTH + 1,
                      "the wall crossing must be the nearer escape, or the scenario "
                      "passes without testing anything");
        static_assert(SAND_DEPTH <= Player::MAX_UNSTUCK_RADIUS,
                      "the legitimate escape must be inside the search radius, or "
                      "this tests the grind-upward path instead");

        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        for (int y = 0; y < FLOOR_Y; ++y)
            g.set_element(WALL_X, y, ElementType::Wall);
        for (int y = SAND_TOP; y < FLOOR_Y; ++y)
            for (int x = 0; x < WALL_X; ++x)
                g.set_element(x, y, ElementType::Sand);

        // Right edge flush against the wall's near face, feet on the floor, sand up
        // over the waist.
        Player p(WALL_X - Player::WIDTH, FLOOR_Y - Player::HEIGHT);
        check("a player buried beside a wall starts out overlapping terrain",
              p.overlaps_solid(g, p.cell_x(), p.cell_y()));

        bool crossed = false;
        for (int i = 0; i < 4 * Player::HEIGHT; ++i) {
            step(g, p, NOTHING, 1);
            if (p.cell_x() + Player::WIDTH > WALL_X) crossed = true;
        }

        check("the unstuck search does not carry the body through a wall", !crossed,
              feet_detail(p) + " wall at " + std::to_string(WALL_X));
        check("a player buried beside a wall still gets out of the terrain",
              !p.overlaps_solid(g, p.cell_x(), p.cell_y()), feet_detail(p));
    }

    // --- the sub-cell remainder must not carry the body into terrain ---
    //
    // These assert on visual_x()/visual_y() -- the cell plus the sub-cell
    // remainder, as the renderer draws it -- which is the only thing in the suites
    // that does and is meant to stay that way -- but the bug they cover is a
    // simulation bug, not a rendering one. rem_y left holding pending motion that
    // had been decided against sinks a standing player a full cell into the floor
    // over a couple of dozen steps and snaps it back. Every whole-cell assertion
    // above passes throughout, because the body's cell never moves.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        Player p(10, AIR_Y);
        step(g, p, NOTHING, 120);  // land and settle

        const float rest_y = visual_y(p);
        float lowest = rest_y;
        for (int i = 0; i < 120; ++i) {
            step(g, p, NOTHING, 1);
            if (visual_y(p) > lowest) lowest = visual_y(p);
        }

        check("a standing player does not sink into the floor",
              lowest - rest_y < 0.001f,
              "sank " + std::to_string(lowest - rest_y) + " cells below rest");
        check("a standing player does not drift off the floor", visual_y(p) == rest_y,
              feet_detail(p));
    }

    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        for (int y = FLOOR_Y - Player::HEIGHT - 2; y < FLOOR_Y; ++y)
            g.set_element(OBSTACLE_X, y, ElementType::Wall);

        Player p(OBSTACLE_X - 4 * Player::WIDTH, STAND_Y);
        step(g, p, held_right(), 120);  // walk into the wall and keep pushing

        // The body's right edge must never cross the wall's near face, not even
        // fractionally. Held against it, rem_x otherwise accumulates most of a cell
        // before a whole-cell step is attempted, drawing the player inside the wall
        // it is flush against.
        const float right_edge = visual_x(p) + Player::WIDTH;
        check("a player held against a wall does not phase into it",
              right_edge <= static_cast<float>(OBSTACLE_X),
              "right edge at " + std::to_string(right_edge) + ", wall face at " +
                  std::to_string(OBSTACLE_X));
    }

    // --- a settled sand pile is still walkable, whatever MAX_STEP_HEIGHT is ---
    //
    // Every lip case above is built from Wall, so all of them test the constant
    // against a cliff. The terrain the player actually meets is powder, and a
    // settled pile is not a cliff: it is a slope of one-cell steps whose total rise
    // is far more than any step height would ever be. Two different promises, and
    // only the cliff half is covered by them.
    //
    // This is the failure direction that lowering MAX_STEP_HEIGHT risks: it trades
    // towards settled sand becoming a staircase you have to jump up, and the cliff
    // tests are written in terms of the constant, so they follow it down silently.
    // The assertion deliberately names no number -- it says the player crosses the
    // pile, which has to stay true at any step height a settled slope's one-cell
    // steps can clear.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        const int pile_w = 2 * Player::WIDTH;
        for (int y = FLOOR_Y - Player::HEIGHT / 2; y < FLOOR_Y; ++y)
            for (int x = OBSTACLE_X; x < OBSTACLE_X + pile_w; ++x)
                g.set_element(x, y, ElementType::Sand);

        // Let it slump before the player arrives -- a pile still collapsing is a
        // different scenario, and a less repeatable one.
        int quiet = 0;
        for (int i = 0; i < 4000 && quiet < 30; ++i) {
            g.update();
            quiet = g.active_chunk_count() == 0 ? quiet + 1 : 0;
        }

        // The pile's real extent after slumping, rather than the block that was
        // painted: sand spreads, and asserting against the painted width would be
        // asserting the player cleared less than it did.
        int pile_right = OBSTACLE_X;
        int pile_peak = FLOOR_Y;
        for (int x = 0; x < WORLD_W; ++x)
            for (int y = 0; y < FLOOR_Y; ++y)
                if (g.get_element(x, y).type == ElementType::Sand) {
                    if (x > pile_right) pile_right = x;
                    if (y < pile_peak) pile_peak = y;
                }
        const std::string pile = "pile spans to x=" + std::to_string(pile_right) +
                                 ", peak row " + std::to_string(pile_peak) + " (rise " +
                                 std::to_string(FLOOR_Y - pile_peak) + "), step height " +
                                 std::to_string(Player::MAX_STEP_HEIGHT);

        // If the slump left a mound no taller than a single step, this scenario is
        // not testing what it claims to and would pass for the wrong reason.
        check("the settled pile is taller than one step", FLOOR_Y - pile_peak > Player::MAX_STEP_HEIGHT, pile);

        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);
        step(g, p, held_right(), 600);

        check("player walks over a settled sand pile", p.cell_x() > pile_right,
              feet_detail(p) + "; " + pile);
    }

    // ========================================================================
    // The body can be hurt, and by exactly two things.
    //
    // Every number asserted below is derived from the constants rather than written
    // out, for the reason this file's header gives about the body's dimensions: a
    // retune would otherwise leave these tests with their names intact and their
    // meanings changed. What is pinned here is the shape of each rule -- free, then
    // linear, then clamped -- not the tuning, which lives in TUNING.md.
    // ========================================================================

    // --- the spawn drop costs nothing, and the second identical fall does ---
    //
    // These two are one scenario deliberately, because the whole content of the
    // "first landing is free" rule is the difference between them: run the same
    // fall twice and the first is 0 and the second is priced. Split into two tests
    // against two worlds, a change that made the rule permanent instead of one-shot
    // would pass both.
    {
        // Tall enough to reach MAX_FALL_SPEED with room to spare, so what is being
        // measured is the clamp and not the height of this world. Terminal velocity
        // arrives after v^2 / 2*GRAVITY cells, and this is a multiple of that,
        // stated against the constant so it stays true if either is retuned.
        const int fall_cells = fx::trunc(Player::MAX_FALL_SPEED);
        const int TALL_H = fall_cells + 4 * Player::HEIGHT;
        const int TALL_FLOOR = TALL_H - Player::HEIGHT;
        const int PLATFORM_Y = TALL_FLOOR - fall_cells;

        Grid g = make_world(WORLD_W, TALL_H, TALL_FLOOR);
        for (int x = 0; x < WORLD_W; ++x) g.set_element(x, PLATFORM_Y, ElementType::Wall);

        // Spawned a few cells over the platform: the first landing is a short one
        // anyway, so this scenario does not lean on the free rule to say that a
        // gentle landing is free.
        Player p(10, PLATFORM_Y - Player::HEIGHT - 3);
        step(g, p, NOTHING, 30);
        check("S0: a gentle first landing costs nothing",
              p.health() == Player::MAX_HEALTH && feet(p) == PLATFORM_Y,
              "hp=" + std::to_string(p.health()) + " " + feet_detail(p));

        // Now take the floor away and let the same body fall the long way. It has
        // landed once, so this one is charged.
        for (int x = 0; x < WORLD_W; ++x) g.set_element(x, PLATFORM_Y, ElementType::Empty);
        step(g, p, NOTHING, 4 * fall_cells);

        const int expected = fx::trunc(Player::MAX_FALL_SPEED - Player::SAFE_FALL_SPEED) /
                             Player::FALL_DAMAGE_DIVISOR;
        check("S0: a terminal-velocity landing costs the full fall-damage price",
              feet(p) == TALL_FLOOR && p.health() == Player::MAX_HEALTH - expected,
              "hp=" + std::to_string(p.health()) + " expected " +
                  std::to_string(Player::MAX_HEALTH - expected) + "; " + feet_detail(p));

        // And the same again kills, which is the design stated at SAFE_FALL_SPEED:
        // one free terminal-velocity mistake, not two. This is also the clamp --
        // health stops at zero rather than going negative.
        check("S0: two terminal-velocity landings are more than a full bar",
              2 * expected >= Player::MAX_HEALTH,
              std::to_string(expected) + " each of " + std::to_string(Player::MAX_HEALTH));
    }

    // --- the whole spawn drop is free, however far it is ---
    // The case above tested a short first landing. This is the case the rule
    // actually exists for: Run spawns the body a long way up.
    {
        const int TALL_H = fx::trunc(Player::MAX_FALL_SPEED) + 4 * Player::HEIGHT;
        Grid g = make_world(WORLD_W, TALL_H, TALL_H - Player::HEIGHT);
        Player p(10, AIR_Y);
        step(g, p, NOTHING, 4 * TALL_H);

        check("S0: the spawn drop is free at any height",
              p.is_on_ground() && p.health() == Player::MAX_HEALTH,
              "hp=" + std::to_string(p.health()) + "; " + feet_detail(p));
    }

    // --- jumping never hurts ---
    // The static_assert in player.h holds the constants apart; this holds the code
    // path, which is a different claim -- the assert would still pass if the landing
    // check read the speed at the wrong moment and measured a jump's launch instead
    // of its arrival.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);

        PlayerInput jump;
        jump.jump = true;
        for (int i = 0; i < 8; ++i) {
            p.update(g, jump);  // one beat
            step(g, p, NOTHING, 90);  // up, over and back down
            if (!p.is_on_ground()) break;
        }
        check("S0: a standing jump lands for free, every time",
              p.is_on_ground() && p.health() == Player::MAX_HEALTH,
              "hp=" + std::to_string(p.health()) + "; " + feet_detail(p));
    }

    // --- standing in fire burns, at the rate the constants name ---
    // The flame is re-stamped every step because Fire is a gas with a lifetime and
    // would otherwise rise out of the body within a dozen steps. What is being
    // tested is the burn rule, not how long a flame lasts.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);
        check("S0: the body starts a run at full health", p.health() == Player::MAX_HEALTH);

        const int seconds_worth = 60;
        for (int i = 0; i < seconds_worth; ++i) {
            for (int y = p.cell_y(); y < p.cell_y() + Player::HEIGHT; ++y)
                for (int x = p.cell_x(); x < p.cell_x() + Player::WIDTH; ++x)
                    g.set_element(x, y, ElementType::Fire);
            g.update();
            p.update(g, NOTHING);
        }

        // A tick on the step the body first touches heat, then one every
        // BURN_INTERVAL_STEPS after it.
        const int ticks = 1 + (seconds_worth - 1) / Player::BURN_INTERVAL_STEPS;
        check("S0: standing in fire costs health at the burn rate",
              p.health() == Player::MAX_HEALTH - ticks * Player::BURN_DAMAGE,
              "hp=" + std::to_string(p.health()) + " expected " +
                  std::to_string(Player::MAX_HEALTH - ticks * Player::BURN_DAMAGE));
    }

    // --- and steam does not, which is the half a one-sided test would miss ---
    // Steam's spawn temperature sits below the burn threshold on purpose: spawned
    // hotter, a doused fire would be a weapon. player.h asserts the two numbers stay
    // apart; this asserts the rule reads them.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);

        for (int i = 0; i < 120; ++i) {
            for (int y = p.cell_y(); y < p.cell_y() + Player::HEIGHT; ++y)
                for (int x = p.cell_x(); x < p.cell_x() + Player::WIDTH; ++x)
                    g.set_element(x, y, ElementType::Steam);
            g.update();
            p.update(g, NOTHING);
        }
        check("S0: standing in steam costs nothing", p.health() == Player::MAX_HEALTH,
              "hp=" + std::to_string(p.health()) + ", steam spawns at " +
                  std::to_string(material_of(ElementType::Steam).spawn_temperature) +
                  " against a threshold of " + std::to_string(Player::BURN_TEMPERATURE));
    }

    // --- fire kills, and the health stops at zero ---
    // The one scenario that runs the rule all the way to its end. Worth its own
    // block rather than folded into the rate test above: a missing clamp would show
    // up here as a negative number and nowhere else.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        Player p(10, STAND_Y);
        step(g, p, NOTHING, 60);

        // Long enough to take the bar past zero rather than exactly to it.
        const int steps = 2 * Player::BURN_INTERVAL_STEPS *
                          (Player::MAX_HEALTH / Player::BURN_DAMAGE);
        for (int i = 0; i < steps; ++i) {
            for (int y = p.cell_y(); y < p.cell_y() + Player::HEIGHT; ++y)
                for (int x = p.cell_x(); x < p.cell_x() + Player::WIDTH; ++x)
                    g.set_element(x, y, ElementType::Fire);
            g.update();
            p.update(g, NOTHING);
        }
        check("S0: a body left in fire dies, and stops at zero rather than below it",
              p.health() == 0 && !p.is_alive(), "hp=" + std::to_string(p.health()));
    }

    // =====================================================================
    // The body among loose bodies and in water. Everything above runs in a world
    // with neither, which is the promise: walking, jumping and flying are what
    // they were. What follows is what is new, stepped with the pushes the body
    // makes queued the way Run queues them (step_coupled).
    // =====================================================================

    // --- standing on a moving raft, the player goes with it ---
    // A 40x3 wooden raft floating in a pool, pushed along by a steady current
    // (an impulse each step), with the player standing still on it.
    {
        Grid g(320, 100);
        fill(g, 0, 90, 319, 99, ElementType::Wall);
        fill(g, 0, 40, 0, 89, ElementType::Wall);
        fill(g, 319, 40, 319, 89, ElementType::Wall);
        fill(g, 1, 60, 318, 89, ElementType::Water);
        fill(g, 60, 50, 99, 52, ElementType::Wood);
        nudge(g, 80, 49);
        Player p(76, 20);
        step_coupled(g, p, NOTHING, 240);
        check("a player dropped on a floating raft stands on it", p.is_on_ground() && p.is_riding(),
              feet_detail(p) + " raft top row " + std::to_string(top_of(g, ElementType::Wood)));

        const int raft0 = left_of(g, ElementType::Wood), player0 = p.cell_x();
        int grounded = 0;
        const int64_t current = int64_t{2000} * fx::per_step(Grid::BODY_WEIGHT_ACCEL);
        for (int s = 0; s < 120; ++s) {
            const int left = left_of(g, ElementType::Wood);
            g.queue_impulse(left, top_of(g, ElementType::Wood) + 1, current, 0);
            step_coupled(g, p, NOTHING, 1);
            grounded += p.is_on_ground();
        }
        const int raft_moved = left_of(g, ElementType::Wood) - raft0;
        const int player_moved = p.cell_x() - player0;
        check("the raft drifts", raft_moved > 10, "moved " + std::to_string(raft_moved));
        check("and carries the player with it", std::abs(player_moved - raft_moved) <= 3,
              "raft " + std::to_string(raft_moved) + " player " + std::to_string(player_moved));
        check("without the player falling off it or flickering into the air", grounded >= 117,
              std::to_string(grounded) + " of 120 steps on the ground");
        check("and the player is never inside the raft", !p.overlaps_solid(g, p.cell_x(), p.cell_y()));
    }

    // --- the raft carries the player's weight ---
    // The same raft with and without the player on it: loaded, it floats lower
    // -- the mean row of its cells, over a hundred steps of bobbing, since a
    // raft loaded at one end also tips and its top row is the end in the air.
    {
        const auto raft_depth = [](bool loaded) {
            Grid g(200, 100);
            fill(g, 0, 90, 199, 99, ElementType::Wall);
            fill(g, 0, 40, 0, 89, ElementType::Wall);
            fill(g, 199, 40, 199, 89, ElementType::Wall);
            fill(g, 1, 60, 198, 89, ElementType::Water);
            fill(g, 80, 50, 119, 52, ElementType::Wood);
            nudge(g, 100, 49);
            Player p(96, 20);
            int64_t rows = 0, cells = 0;
            for (int s = 0; s < 300; ++s) {
                if (loaded) {
                    step_coupled(g, p, NOTHING, 1);
                } else {
                    g.update();
                }
                if (s < 200) continue;
                for (int y = 0; y < 100; ++y)
                    for (int x = 0; x < 200; ++x)
                        if (g.get_element(x, y).type == ElementType::Wood) {
                            rows += y;
                            ++cells;
                        }
            }
            return cells > 0 ? 100 * rows / cells : 0;  // hundredths of a row
        };
        const int64_t empty = raft_depth(false), loaded = raft_depth(true);
        check("a raft with the player on it floats lower than one without", loaded > empty,
              "mean row x100 empty " + std::to_string(empty) + ", loaded " + std::to_string(loaded));
        check("and still floats", loaded < 100 * 62,
              "mean row x100 " + std::to_string(loaded) + ", surface at row 60");
    }

    // --- a body moving into the player pushes it, rather than burying it ---
    // A crate shoved along the floor into a player standing still, by something
    // pushing it at about a quarter of a walk: pushed hard while it is slower
    // than that, not at all once it is faster.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        fill(g, OBSTACLE_X - 30, FLOOR_Y - 10, OBSTACLE_X - 21, FLOOR_Y - 1, ElementType::Wood);
        Player p(OBSTACLE_X, STAND_Y);
        step_coupled(g, p, NOTHING, 60);
        const int x0 = p.cell_x();
        bool teleported = false, buried = false;
        const int64_t shove = int64_t{1200} * fx::per_step(Grid::BODY_WEIGHT_ACCEL);
        for (int s = 0; s < 120; ++s) {
            int right = 0;
            for (int y = FLOOR_Y - 12; y < FLOOR_Y; ++y)
                for (int x = 0; x < WORLD_W; ++x)
                    if (g.get_element(x, y).type == ElementType::Wood) right = std::max(right, x);
            const int b = g.body_at(right - 9, FLOOR_Y - 5);
            const fx::v going = b >= 0 ? g.body_velocity_at(b, right - 9, FLOOR_Y - 5).vx : 0;
            if (going < Player::MOVE_SPEED / 4) g.queue_impulse(right - 9, FLOOR_Y - 5, shove, 0);
            step_coupled(g, p, NOTHING, 1);
            teleported = teleported || p.teleported();
            buried = buried || p.overlaps_solid(g, p.cell_x(), p.cell_y());
        }
        check("a crate slid into the player pushes it along", p.cell_x() > x0 + 4,
              "moved " + std::to_string(p.cell_x() - x0));
        check("and never buries it, so it is never dug out", !teleported && !buried);
        check("a crate sliding at a few cells a second does not hurt", p.health() == Player::MAX_HEALTH,
              "hp=" + std::to_string(p.health()));
    }

    // --- a falling slab hurts; a light one dropped as far hurts less ---
    {
        const auto struck = [](int w, int h, ElementType t) {
            Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
            Player p(OBSTACLE_X, STAND_Y);
            step_coupled(g, p, NOTHING, 60);
            const int x = p.cell_x() + Player::WIDTH / 2 - w / 2;
            fill(g, x, 2, x + w - 1, 2 + h - 1, t);
            nudge(g, x + w / 2, 1);
            int hits = 0;
            for (int s = 0; s < 120; ++s) {
                step_coupled(g, p, NOTHING, 1);
                hits += p.struck().hit && p.struck().speed > Player::IMPACT_SAFE_SPEED;
            }
            return std::pair<int, int>(Player::MAX_HEALTH - p.health(), hits);
        };
        const auto [stone, stone_hits] = struck(12, 6, ElementType::Wall);
        const auto [plank, plank_hits] = struck(12, 2, ElementType::Wood);
        check("a stone slab dropped on the player hurts it", stone > 0 && stone_hits > 0,
              "damage " + std::to_string(stone));
        check("and does not kill it from a few body heights", stone < Player::MAX_HEALTH,
              "damage " + std::to_string(stone));
        check("a light plank dropped as far hurts less", plank < stone,
              "plank " + std::to_string(plank) + " stone " + std::to_string(stone));
    }

    // --- walking into a crate pushes it; into a stone, does not ---
    {
        const auto pushed = [](int w, int h, ElementType t) {
            Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
            const int x = OBSTACLE_X;
            fill(g, x, FLOOR_Y - h, x + w - 1, FLOOR_Y - 1, t);
            Player p(x - Player::WIDTH - 4, STAND_Y);
            step_coupled(g, p, NOTHING, 60);
            const int start = left_of(g, t);
            step_coupled(g, p, held_right(), 120);
            return left_of(g, t) - start;
        };
        const int crate = pushed(10, 10, ElementType::Wood);
        const int stone = pushed(16, 12, ElementType::Wall);
        check("walking into a wooden crate pushes it", crate > 20, "moved " + std::to_string(crate));
        check("no faster than the player walks",
              crate <= fx::trunc(Player::MOVE_SPEED) * 2, "moved " + std::to_string(crate));
        check("walking into a heavy stone does not move it", stone == 0,
              "moved " + std::to_string(stone));
    }

    // --- a knock throws the body, skids out, and leaves walking as it was ---
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        Player p(OBSTACLE_X, STAND_Y);
        step(g, p, NOTHING, 60);
        const int x0 = p.cell_x();
        p.knock(fx::from_int(150), -fx::from_int(130));
        bool left_ground = false;
        int s = 0;
        for (; s < 240; ++s) {
            step(g, p, PlayerInput{true, false, false}, 1);  // holding back toward where it was hit
            left_ground = left_ground || !p.is_on_ground();
            if (s > 20 && p.push_velocity() == 0) break;
        }
        check("a knock throws the player off its feet and away", left_ground && p.cell_x() > x0 + 20,
              "thrown " + std::to_string(p.cell_x() - x0));
        check("and it skids to a stop", p.push_velocity() == 0, "after " + std::to_string(s));
        const int x1 = p.cell_x();
        step(g, p, held_right(), 60);
        const int expected = fx::trunc(Player::MOVE_SPEED) * 66 / 100;
        check("walking afterwards is walking", p.cell_x() - x1 > expected &&
                                                   p.velocity_x() == Player::MOVE_SPEED,
              "moved " + std::to_string(p.cell_x() - x1));
    }

    // --- water: a fall into a pool is slowed, the body sinks slowly, and swims ---
    // Two bodies deep, so the body is all the way under for most of the way down
    // and up, and there is air above the surface to climb out into.
    {
        Grid g = make_world(WORLD_W, WORLD_H, FLOOR_Y);
        const int surface = FLOOR_Y - 2 * Player::HEIGHT;
        for (int y = surface; y < FLOOR_Y; ++y)
            for (int x = 0; x < WORLD_W; ++x) g.set_element(x, y, ElementType::Water);
        Player p(WORLD_W / 2, AIR_Y);
        fx::v fastest_under = 0;
        int steps_to_floor = 0;
        for (int s = 0; s < 600 && !p.is_on_ground(); ++s) {
            step(g, p, NOTHING, 1);
            if (p.submerged() == fx::ONE) fastest_under = std::max(fastest_under, p.velocity_y());
            steps_to_floor = s;
        }
        check("in water, the body sinks slowly", fastest_under < fx::from_int(60),
              "fastest " + std::to_string(fx::to_float(fastest_under)) + " cells/s");
        check("and does reach the bottom", feet(p) == FLOOR_Y, feet_detail(p) +
                                                               " after " + std::to_string(steps_to_floor));
        check("landing at the bottom of a pool does not hurt", p.health() == Player::MAX_HEALTH,
              "hp=" + std::to_string(p.health()));

        const int bottom = p.cell_y();
        PlayerInput swim;
        swim.jump = true;
        int out = -1;
        for (int s = 0; s < 600 && out < 0; ++s) {
            step(g, p, swim, 1);
            if (feet(p) < surface) out = s;
        }
        check("holding the flap key swims up out of the pool", out > 0,
              "out after " + std::to_string(out) + " steps from row " + std::to_string(bottom));
        check("slowly: it takes the best part of a second or more", out > 40,
              "out after " + std::to_string(out));
    }

    return report();
}
