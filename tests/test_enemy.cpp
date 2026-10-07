// Enemies and the bow.
//
// The claim the feature makes is that an enemy comes apart where it is hit, and
// that what comes off is matter: the pixels an arrow removes are grains of sand in
// the grid, in the body's colours, and they fall. Every scenario here checks one
// half of that against the other -- what the body lost against what the grid
// gained -- because a body that loses pixels without the grid gaining them, or
// the reverse, is the bug that would look fine on screen for a while.
//
// Frame coordinates come from enemy_art.h rather than being written as numbers
// where it matters: the arm is "the columns outside the box", not columns 0-2.

#include "game/boot.h"
#include "game/run.h"
#include "physics/arrow.h"
#include "physics/enemy.h"
#include "test_util.h"
#include <string>

namespace {

// The ghoul, which every scenario below is about until the troll's own.
constexpr const Species& G = species::GHOUL;

constexpr int WORLD_W = 320;
constexpr int WORLD_H = 120;
constexpr int FLOOR_Y = WORLD_H - 10;

Grid make_world() {
    Grid g(WORLD_W, WORLD_H);
    for (int y = FLOOR_Y; y < WORLD_H; ++y)
        for (int x = 0; x < WORLD_W; ++x)
            g.set_element(x, y, ElementType::Wall);
    return g;
}

int count_sand(const Grid& g) {
    int n = 0;
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < g.get_width(); ++x)
            if (g.get_element(x, y).type == ElementType::Sand) ++n;
    return n;
}

// Steps grid and enemy together, with the target parked far away so the enemy
// wanders rather than chases.
void settle(Grid& g, Enemy& e, int steps) {
    for (int i = 0; i < steps; ++i) {
        g.update();
        e.update(g, -10000, -10000, false);
    }
}

// The world cell frame pixel (fx, fy) covers at rest, recomputed here from the
// public position and the anchoring rule the header states, so a disagreement
// between the two is a failure rather than a tautology. Only true of a body in its
// rest pose -- a fresh spawn -- since a pose moves the pixels off it.
int rest_x(const Enemy& e, int fx) {
    const Species& k = e.species();
    const int left = e.cell_x() - k.offset_x();
    return e.facing_left() ? left + (k.frame_w() - 1 - fx) : left + fx;
}
int rest_y(const Enemy& e, int fy) { return e.cell_y() - e.species().offset_y() + fy; }

// Where frame pixel (fx, fy) is in the current pose: where to aim at it.
int world_x(const Enemy& e, int fx, int fy) {
    int wx = 0, wy = 0;
    e.world_of(fx, fy, wx, wy);
    return wx;
}
int world_y(const Enemy& e, int fx, int fy) {
    int wx = 0, wy = 0;
    e.world_of(fx, fy, wx, wy);
    return wy;
}

int count_pixels(const Enemy& e, bool (*pred)(int, int)) {
    int n = 0;
    for (int y = 0; y < G.frame_h(); ++y)
        for (int x = 0; x < G.frame_w(); ++x)
            if (pred(x, y) && e.has_pixel(x, y)) ++n;
    return n;
}

bool is_left_arm(int x, int y) { return enemy_art::is_arm(x, y) && x < enemy_art::BOX_LEFT; }
bool is_right_arm(int x, int y) { return enemy_art::is_arm(x, y) && x >= enemy_art::BOX_RIGHT; }
bool anything(int x, int y) { return enemy_art::is_body(x, y); }

// An enemy standing on the floor, settled, at x.
Enemy standing_enemy(Grid& g, int x) {
    Enemy e;
    e.spawn(x, FLOOR_Y - G.height - 4);
    settle(g, e, 30);
    return e;
}

}  // namespace

int main() {
    // --- a fresh body ---
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        check("a spawned enemy is alive and whole",
              e.is_alive() && e.pixel_count() == enemy_art::PIXEL_COUNT,
              "pixels=" + std::to_string(e.pixel_count()));
        check("it lands on the floor rather than in it",
              e.cell_y() + G.height == FLOOR_Y && !e.overlaps_solid(g, e.cell_x(), e.cell_y()),
              "feet=" + std::to_string(e.cell_y() + G.height));
        check("it has arms and feet to start with", e.has_arms() && e.has_feet());

        // At rest, the pose is the art: every pixel exactly where the anchoring
        // rule puts it, found there by an arrow, and nothing found in a gap.
        Enemy fresh;
        fresh.spawn(100, 20);
        bool rest_is_art = true;
        for (int y = 0; y < G.frame_h(); ++y)
            for (int x = 0; x < G.frame_w(); ++x) {
                const int want = enemy_art::is_body(x, y) ? y * G.frame_w() + x : -1;
                if (fresh.pixel_at(rest_x(fresh, x), rest_y(fresh, y)) != want) rest_is_art = false;
                if (want >= 0 && (world_x(fresh, x, y) != rest_x(fresh, x) ||
                                  world_y(fresh, x, y) != rest_y(fresh, y)))
                    rest_is_art = false;
            }
        check("a fresh spawn stands in the art's own pose", rest_is_art);
        check("nothing has turned to sand yet", count_sand(g) == 0);
    }

    // --- the arm: hit below the shoulder, and the whole forearm comes off ---
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        const int right_before = count_pixels(e, is_right_arm);

        // The elbow: the left arm's middle column, a few rows below the shoulder.
        const int fx = 1, fy = enemy_art::ARM_TOP + 5;
        const int wx = world_x(e, fx, fy), wy = world_y(e, fx, fy);
        check("the arm pixel is where the pose puts it", e.pixel_at(wx, wy) >= 0);

        const int lost = e.shatter(g, wx, wy, Quiver::BITE_RADIUS);

        bool hand_gone = true;
        for (int y = fy; y < G.frame_h(); ++y)
            for (int x = 0; x < enemy_art::BOX_LEFT; ++x)
                if (e.has_pixel(x, y)) hand_gone = false;
        check("everything below the cut on that arm is gone", hand_gone);
        check("but the shoulder above it stays on", count_pixels(e, is_left_arm) > 0);
        check("the other arm is untouched", count_pixels(e, is_right_arm) == right_before);
        check("losing an arm is not fatal", e.is_alive());
        check("the arm went into the grid as sand, grain for pixel", count_sand(g) == lost,
              "lost=" + std::to_string(lost) + " sand=" + std::to_string(count_sand(g)));
        check("more came off than the bite alone, because the forearm was severed",
              lost > 13, "lost=" + std::to_string(lost));

        // The grains are the arm's colours, not the Sand row's.
        bool bone_found = false;
        for (int y = 0; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x) {
                const Element el = g.get_element(x, y);
                if (el.type == ElementType::Sand && el.color == enemy_art::color_at(0, 19))
                    bone_found = true;
            }
        check("the claw's grain keeps the claw's colour", bone_found);

        // And it falls like sand.
        const int sand = count_sand(g);
        settle(g, e, 240);
        bool resting = true;
        for (int y = 0; y < WORLD_H - 1; ++y)
            for (int x = 0; x < WORLD_W; ++x)
                if (g.get_element(x, y).type == ElementType::Sand &&
                    g.get_element(x, y + 1).type == ElementType::Empty)
                    resting = false;
        check("the grains fall and come to rest", resting);
        check("and none of them were lost on the way down", count_sand(g) == sand);
    }

    // --- the head: one arrow's bite spans both eyes ---
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        int eye_y = 0;
        for (int y = 0; y < G.frame_h(); ++y)
            for (int x = 0; x < G.frame_w(); ++x)
                if (enemy_art::is_head(x, y)) eye_y = y;
        e.shatter(g, world_x(e, G.frame_w() / 2, eye_y), world_y(e, G.frame_w() / 2, eye_y), Quiver::BITE_RADIUS);
        check("a shot between the eyes kills it", !e.is_alive());
        check("and the whole body comes down as sand", count_sand(g) == enemy_art::PIXEL_COUNT,
              "sand=" + std::to_string(count_sand(g)) + " of " +
                  std::to_string(enemy_art::PIXEL_COUNT));
        check("a dead body has no pixels left to hit", e.pixel_count() == 0);
    }

    // --- the legs: one is a limp, two is the end ---
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        const int knee_y = G.frame_h() - 4;
        e.shatter(g, world_x(e, 5, knee_y), world_y(e, 5, knee_y), Quiver::BITE_RADIUS);
        check("one leg shot through still leaves a foot to stand on",
              e.is_alive() && e.has_feet());
        e.shatter(g, world_x(e, 9, knee_y), world_y(e, 9, knee_y), Quiver::BITE_RADIUS);
        check("both legs shot through and it collapses", !e.is_alive());
    }

    // --- severing never takes the heart's own side ---
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        const int before = e.pixel_count();
        // The outer edge of the right arm, at the shoulder.
        const int lost = e.shatter(g, world_x(e, G.frame_w() - 1, 13), world_y(e, G.frame_w() - 1, 13), 1);
        check("a graze takes a graze", lost > 0 && lost < 10 && e.pixel_count() == before - lost,
              "lost=" + std::to_string(lost));
    }

    // --- fire eats it from the feet up ---
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        // A fire across the floor under it, and nowhere else, kept lit.
        int head_lost_while_feet_burned = 0;
        int steps = 0;
        for (; steps < 600 && e.is_alive(); ++steps) {
            for (int x = e.cell_x() - 2; x < e.cell_x() + G.width + 2; ++x)
                for (int y = FLOOR_Y - 3; y < FLOOR_Y; ++y)
                    if (g.get_element(x, y).type == ElementType::Empty)
                        g.set_element(x, y, ElementType::Fire);
            g.update();
            e.update(g, -10000, -10000, false);
            if (e.is_alive() && count_pixels(e, enemy_art::is_head) < 2)
                ++head_lost_while_feet_burned;
        }
        check("standing in fire it burns away", !e.is_alive(),
              "steps=" + std::to_string(steps));
        check("over a while, rather than the step it touches the flame",
              steps > 4 * Enemy::BURN_INTERVAL_STEPS, "steps=" + std::to_string(steps));
        check("and the head is still on while it does", head_lost_while_feet_burned == 0);
    }

    // --- the bow and the arrow ---
    {
        Run run(WORLD_W, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x)
                run.grid.set_element(x, y, ElementType::Wall);
        run.player = Player(40, FLOOR_Y - Player::HEIGHT);

        // An enemy to the right, at the player's height, a few body widths off.
        const int ex = 40 + 10 * G.width;
        check("an enemy can be spawned into open air", run.spawn_enemy(ex, FLOOR_Y - G.height));
        check("but not into a wall", !run.spawn_enemy(ex, FLOOR_Y));
        for (int i = 0; i < 5; ++i) run.step(Input{});

        const Enemy& e = run.enemies[0];
        Input shoot;
        shoot.shoot = true;
        shoot.cursor_x = e.center_x();
        shoot.cursor_y = e.center_y();
        run.step(shoot);

        int live = 0;
        for (const Arrow& a : run.quiver.arrows()) if (a.live) ++live;
        check("holding the bow looses one arrow", live == 1);

        int steps = 0;
        const int before = e.pixel_count();
        Input idle;
        while (steps < 120 && run.enemies[0].pixel_count() == before) {
            run.step(idle);
            ++steps;
        }
        check("the arrow reaches the enemy and takes pixels out of it",
              run.enemies[0].pixel_count() < before, "steps=" + std::to_string(steps));
        check("those pixels are in the grid as sand",
              count_sand(run.grid) == before - run.enemies[0].pixel_count() ||
                  !run.enemies[0].is_alive());

        // A held bow fires on its draw, not on every step.
        Run r2(WORLD_W, WORLD_H);
        Input held;
        held.shoot = true;
        held.cursor_x = WORLD_W - 1;
        held.cursor_y = 0;
        for (int i = 0; i < 3 * Quiver::DRAW_STEPS; ++i) r2.step(held);
        int fired = 0;
        for (const Arrow& a : r2.quiver.arrows()) if (a.live) ++fired;
        check("a held bow fires once per draw", fired == 3, "fired=" + std::to_string(fired));

        // An arrow into a wall stays there, then is cleared.
        bool stuck = false;
        for (int i = 0; i < 200 && !stuck; ++i) {
            r2.step(Input{});
            for (const Arrow& a : r2.quiver.arrows()) if (a.live && a.stuck) stuck = true;
        }
        check("an arrow that hits terrain sticks in it", stuck);
        for (int i = 0; i < Quiver::STUCK_STEPS + 10; ++i) r2.step(Input{});
        int left = 0;
        for (const Arrow& a : r2.quiver.arrows()) if (a.live) ++left;
        check("and is cleared away after a while", left == 0, "left=" + std::to_string(left));
    }

    // --- it notices you, closes in, and swipes ---
    {
        Run run(WORLD_W, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x)
                run.grid.set_element(x, y, ElementType::Wall);
        run.player = Player(60, FLOOR_Y - Player::HEIGHT);
        run.spawn_enemy(60 + G.notice_x / 2, FLOOR_Y - G.height);
        int hurt_steps = 0;
        for (int i = 0; i < 600; ++i) {
            run.step(Input{});
            if (run.player.damage_this_step() > 0) ++hurt_steps;
        }
        check("an enemy that has seen you walks over and hurts you",
              run.player.health() < Player::MAX_HEALTH,
              "hp=" + std::to_string(run.player.health()));
        check("at the swipe's rate, not every step",
              hurt_steps <= 600 / G.attack_interval + 1,
              "hurt_steps=" + std::to_string(hurt_steps));

        // Disarmed, it cannot.
        Run calm(WORLD_W, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x)
                calm.grid.set_element(x, y, ElementType::Wall);
        calm.player = Player(60, FLOOR_Y - Player::HEIGHT);
        calm.spawn_enemy(60 + G.notice_x / 2, FLOOR_Y - G.height);
        Enemy& e = calm.enemies[0];
        for (int y = enemy_art::ARM_TOP; y < G.frame_h(); ++y) {
            e.shatter(calm.grid, world_x(e, 1, y), world_y(e, 1, y), 1);
            e.shatter(calm.grid, world_x(e, G.frame_w() - 2, y), world_y(e, G.frame_w() - 2, y), 1);
        }
        check("an enemy with both arms off is still alive", e.is_alive() && !e.has_arms());
        for (int i = 0; i < 600; ++i) calm.step(Input{});
        check("but has nothing to swipe with",
              calm.player.health() == Player::MAX_HEALTH,
              "hp=" + std::to_string(calm.player.health()));
    }

    // --- a reset is a fresh run ---
    {
        Run run(WORLD_W, WORLD_H);
        run.spawn_enemy(100, 10);
        Input shoot;
        shoot.shoot = true;
        shoot.cursor_x = 200;
        run.step(shoot);
        run.reset(Grid::DEFAULT_SEED);
        int live_arrows = 0;
        for (const Arrow& a : run.quiver.arrows()) if (a.live) ++live_arrows;
        check("reset clears the enemies, the arrows and the kill count",
              run.enemies_alive() == 0 && live_arrows == 0 && run.kills() == 0);
    }

    // --- planting ---
    {
        // A flat floor at the width of the shipped 10x scenes, with the player
        // standing in the middle as the scene loader leaves it.
        Run run(688, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < 688; ++x)
                run.grid.set_element(x, y, ElementType::Wall);
        run.player = Player(344, FLOOR_Y - Player::HEIGHT);
        const boot::EnemyPlanting planted = boot::plant_enemies(run);
        bool standing = true, clear = true;
        for (const Enemy& e : run.enemies) {
            if (!e.is_alive()) continue;
            if (e.cell_y() + e.species().height != FLOOR_Y ||
                e.overlaps_solid(run.grid, e.cell_x(), e.cell_y()))
                standing = false;
            if (std::abs(e.center_x() - run.player.center_x()) < boot::ENEMY_CLEARANCE) clear = false;
        }
        check("a scene gets several enemies", planted.placed >= 3 && planted.placed == run.enemies_alive(),
              "placed=" + std::to_string(planted.placed));
        check("each planted on the floor, not in it", standing);
        check("and none close enough to have noticed the player at the start", clear);
        check("a scene with room for one gets exactly one troll", planted.trolls == 1,
              "trolls=" + std::to_string(planted.trolls));
        int troll_dist = 0;
        for (const Enemy& e : run.enemies)
            if (e.is_alive() && &e.species() == &species::TROLL)
                troll_dist = std::abs(e.center_x() - run.player.center_x());
        check("and it is outside its own, longer, notice range",
              troll_dist >= boot::clearance_for(species::TROLL),
              "dist=" + std::to_string(troll_dist));

        // A world with no terrain at all -- the `floor` scenes -- stands them on the
        // bottom border, where the player stands.
        Run empty(688, WORLD_H);
        empty.player = Player(344, WORLD_H - Player::HEIGHT);
        const boot::EnemyPlanting on_border = boot::plant_enemies(empty);
        bool on_floor = on_border.placed > 0;
        for (const Enemy& e : empty.enemies)
            if (e.is_alive() && e.cell_y() + e.species().height != WORLD_H) on_floor = false;
        check("an empty scene plants them on the world's floor", on_floor,
              "placed=" + std::to_string(on_border.placed));
    }

    // --- determinism: the same inputs make the same sand ---
    {
        auto play = []() {
            Run run(WORLD_W, WORLD_H, 7);
            for (int y = FLOOR_Y; y < WORLD_H; ++y)
                for (int x = 0; x < WORLD_W; ++x)
                    run.grid.set_element(x, y, ElementType::Wall);
            run.player = Player(40, FLOOR_Y - Player::HEIGHT);
            run.spawn_enemy(150, FLOOR_Y - G.height);
            run.spawn_enemy(230, FLOOR_Y - G.height);
            run.spawn_enemy(260, FLOOR_Y - species::TROLL.height, species::TROLL);
            for (int i = 0; i < 400; ++i) {
                Input in;
                in.shoot = (i % 3) == 0;
                in.cursor_x = 150 + (i % 90);
                in.cursor_y = FLOOR_Y - 15 + (i % 7);
                run.step(in);
            }
            uint64_t h = 1469598103934665603ull;
            for (int y = 0; y < WORLD_H; ++y)
                for (int x = 0; x < WORLD_W; ++x) {
                    const Element el = run.grid.get_element(x, y);
                    h = (h ^ static_cast<uint64_t>(el.type)) * 1099511628211ull;
                    h = (h ^ el.color) * 1099511628211ull;
                }
            return h ^ static_cast<uint64_t>(run.kills());
        };
        check("two runs from the same inputs end in the same world", play() == play());
    }

    // ================= the troll =================
    //
    // The same body rules at a size where "local" means something: a bite is a
    // hole, not a limb, and a limb is something you take apart.
    const Species& T = species::TROLL;
    const body_art::Art& TA = *T.art;

    // --- a fresh troll ---
    {
        Grid g = make_world();
        Enemy e;
        e.spawn(100, FLOOR_Y - T.height - 4, T);
        settle(g, e, 30);
        check("a spawned troll is alive and whole",
              e.is_alive() && e.pixel_count() == troll_art::PIXEL_COUNT,
              "pixels=" + std::to_string(e.pixel_count()));
        check("it stands on the floor", e.cell_y() + T.height == FLOOR_Y,
              "feet=" + std::to_string(e.cell_y() + T.height));
        check("it has arms and feet", e.has_arms() && e.has_feet());
        check("and it is far bigger than a ghoul",
              troll_art::PIXEL_COUNT > 5 * enemy_art::PIXEL_COUNT,
              "troll=" + std::to_string(troll_art::PIXEL_COUNT));
    }

    // --- a hit is a hole, not a wound ---
    {
        Grid g = make_world();
        Enemy e;
        e.spawn(100, FLOOR_Y - T.height, T);
        // The middle of the belly: torso all round, nothing to sever.
        const int lost = e.shatter(g, world_x(e, TA.w / 2, 42), world_y(e, TA.w / 2, 42), Quiver::BITE_RADIUS);
        check("an arrow in the belly takes exactly its bite",
              lost == 13 && e.is_alive(), "lost=" + std::to_string(lost));
        check("as sand, grain for pixel", count_sand(g) == lost);
        check("and leaves a hole you can see through",
              e.pixel_at(world_x(e, TA.w / 2, 42), world_y(e, TA.w / 2, 42)) < 0 &&
                  e.pixel_at(world_x(e, TA.w / 2, 45), world_y(e, TA.w / 2, 45)) >= 0);
    }

    // --- an arm is chipped, then comes off ---
    {
        Grid g = make_world();
        Enemy e;
        e.spawn(100, FLOOR_Y - T.height, T);
        // The left forearm, the arm's own centre column.
        const int arm_x = 8, arm_y = 38;
        check("the forearm is there to hit", e.has_pixel(arm_x, arm_y));
        const int first = e.shatter(g, world_x(e, arm_x - 2, arm_y), world_y(e, arm_x - 2, arm_y), Quiver::BITE_RADIUS);
        bool hand_left = false;
        for (int y = 52; y < TA.h; ++y)
            for (int x = 0; x < TA.box_left; ++x)
                if (e.has_pixel(x, y)) hand_left = true;
        check("one arrow into an arm that thick is a bite, and the hand stays on",
              first <= 13 && hand_left, "first=" + std::to_string(first));
        // Two more across the same height, through the rest of the arm.
        const int rest = e.shatter(g, world_x(e, arm_x + 1, arm_y), world_y(e, arm_x + 1, arm_y), Quiver::BITE_RADIUS) +
                         e.shatter(g, world_x(e, arm_x + 3, arm_y), world_y(e, arm_x + 3, arm_y), Quiver::BITE_RADIUS);
        hand_left = false;
        for (int y = 52; y < TA.h; ++y)
            for (int x = 0; x < TA.box_left; ++x)
                if (e.has_pixel(x, y)) hand_left = true;
        check("cut through, and everything below falls away -- far more than two bites",
              !hand_left && rest > 100, "rest=" + std::to_string(rest));
        check("still standing on one arm", e.is_alive() && e.has_arms());
        check("and the whole forearm is sand in the grid",
              count_sand(g) == troll_art::PIXEL_COUNT - e.pixel_count());
    }

    // --- it wades out of a drift instead of standing buried in it ---
    {
        // What its own arm does when it comes off: a pile well over a step
        // deep, around and inside its feet.
        Grid g = make_world();
        Enemy e;
        e.spawn(100, FLOOR_Y - T.height, T);
        for (int y = FLOOR_Y - 15; y < FLOOR_Y; ++y)
            for (int x = 90; x < 100 + T.width + 10; ++x) g.set_element(x, y, ElementType::Sand);
        const int sand = count_sand(g);
        for (int i = 0; i < 60; ++i) {
            g.update();
            e.update(g, -10000, -10000, false);
        }
        int inside = 0;
        for (int y = e.cell_y(); y < e.cell_y() + T.height; ++y)
            for (int x = e.cell_x(); x < e.cell_x() + T.width; ++x)
                if (g.get_element(x, y).type == ElementType::Sand) ++inside;
        check("a troll in a drift still stands on the floor beneath it",
              e.cell_y() + T.height == FLOOR_Y, "feet=" + std::to_string(e.cell_y() + T.height));
        // Not zero: the drift keeps slumping back against the legs between
        // steps, and the body walks into fresh sand as it goes. A quarter of what
        // buried it is the line between wading and standing in it.
        check("having shoved most of the sand out of its way", inside < 15 * T.width / 4,
              "inside=" + std::to_string(inside));
        check("without losing a grain of it", count_sand(g) == sand);
        check("and it has walked on through", e.cell_x() != 100);
    }

    // --- the head takes one shot per eye ---
    {
        Grid g = make_world();
        Enemy e;
        e.spawn(100, FLOOR_Y - T.height, T);
        int eye_y = 0, left_eye = TA.w, right_eye = 0;
        for (int y = 0; y < TA.h; ++y)
            for (int x = 0; x < TA.w; ++x)
                if (TA.is_head(x, y)) {
                    eye_y = y;
                    if (x < left_eye) left_eye = x;
                    if (x > right_eye) right_eye = x;
                }
        const int between = (left_eye + right_eye) / 2;
        e.shatter(g, world_x(e, between, eye_y), world_y(e, between, eye_y), Quiver::BITE_RADIUS);
        check("a shot between the eyes does not kill a troll", e.is_alive());
        e.shatter(g, world_x(e, left_eye + 1, eye_y), world_y(e, left_eye + 1, eye_y), Quiver::BITE_RADIUS);
        e.shatter(g, world_x(e, right_eye - 1, eye_y), world_y(e, right_eye - 1, eye_y), Quiver::BITE_RADIUS);
        check("one into each eye does", !e.is_alive());
        check("and all of it comes down as sand", count_sand(g) == troll_art::PIXEL_COUNT,
              "sand=" + std::to_string(count_sand(g)));
    }

    // --- the slam: winds up, lands, breaks the ground ---
    auto troll_arena = [](int player_x) {
        Run run(WORLD_W, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x)
                run.grid.set_element(x, y, ElementType::Wall);
        run.player = Player(player_x, FLOOR_Y - Player::HEIGHT);
        run.spawn_enemy(100, FLOOR_Y - species::TROLL.height, species::TROLL);
        return run;
    };
    auto count_type = [](const Grid& g, ElementType t) {
        int n = 0;
        for (int y = 0; y < g.get_height(); ++y)
            for (int x = 0; x < g.get_width(); ++x)
                if (g.get_element(x, y).type == t) ++n;
        return n;
    };
    {
        // The player just in front of it, inside the club's reach.
        Run run = troll_arena(100 + T.width + 4);
        const int walls = count_type(run.grid, ElementType::Wall);
        int wound_up_at = -1, hit_at = -1, x_at_windup = 0;
        bool moved_while_winding = false;
        for (int i = 0; i < 300 && hit_at < 0; ++i) {
            run.step(Input{});
            const Enemy& e = run.enemies[0];
            if (wound_up_at < 0 && e.windup_left() > 0) {
                wound_up_at = i;
                x_at_windup = e.cell_x();
            }
            if (wound_up_at >= 0 && e.windup_left() > 0 && e.cell_x() != x_at_windup)
                moved_while_winding = true;
            if (run.player.damage_this_step() > 0) hit_at = i;
        }
        check("a troll with you in reach winds up a slam", wound_up_at >= 0);
        check("and stands still while it does", !moved_while_winding);
        check("the slam lands after the wind-up, not on contact",
              hit_at - wound_up_at == T.windup_steps,
              "wound=" + std::to_string(wound_up_at) + " hit=" + std::to_string(hit_at));
        check("for the troll's damage",
              run.player.health() == Player::MAX_HEALTH - T.damage,
              "hp=" + std::to_string(run.player.health()));
        const int broken = walls - count_type(run.grid, ElementType::Wall);
        check("and the ground where it lands breaks into sand",
              broken > 20 && count_type(run.grid, ElementType::Sand) == broken,
              "broken=" + std::to_string(broken) +
                  " sand=" + std::to_string(count_type(run.grid, ElementType::Sand)));
    }
    {
        // The same, but the player walks out from under it once it starts.
        Run run = troll_arena(100 + T.width + 4);
        bool started = false, landed = false;
        int hurt = 0;
        for (int i = 0; i < 300 && !landed; ++i) {
            Input in;
            in.right = started;
            const bool winding = run.enemies[0].windup_left() > 0;
            run.step(in);
            if (run.enemies[0].windup_left() > 0) started = true;
            if (winding && run.enemies[0].windup_left() == 0) landed = true;
            if (run.player.damage_this_step() > 0) ++hurt;
        }
        check("a slam you walk out from under misses", landed && hurt == 0,
              "landed=" + std::to_string(landed) + " hurt=" + std::to_string(hurt));
    }
    {
        // Both arms shot off: it has nothing to slam with.
        Run run = troll_arena(100 + T.width + 4);
        Enemy& e = run.enemies[0];
        for (int y = TA.arm_top; y < TA.h; y += 2)
            for (int x = 0; x < TA.w; x += 2)
                if (TA.is_arm(x, y)) e.shatter(run.grid, world_x(e, x, y), world_y(e, x, y), 1);
        check("a troll with both arms off is still alive", e.is_alive() && !e.has_arms());
        for (int i = 0; i < 600; ++i) run.step(Input{});
        check("but cannot slam", run.player.health() == Player::MAX_HEALTH,
              "hp=" + std::to_string(run.player.health()));
    }
    {
        // T at the cursor: the troll stands on the cursor rather than centring on
        // it, so pointing at the ground spawns one standing there.
        Run run = troll_arena(10);
        run.enemies[0] = Enemy{};
        Input in;
        in.spawn_troll = true;
        in.cursor_x = 200;
        in.cursor_y = FLOOR_Y;
        run.step(in);
        check("the spawn key puts a troll on the ground under the cursor",
              run.enemies_alive() == 1 && &run.enemies[0].species() == &species::TROLL &&
                  run.enemies[0].cell_y() + T.height == FLOOR_Y);
    }

    // --- the animation ---------------------------------------------------
    //
    // The pose moves where pixels are drawn and hit and nothing else. What has to
    // hold: it actually moves (walk, slam), what is drawn in a cell is what an
    // arrow finds there and the grain a pixel becomes lands within a cell of
    // where it was drawn, and no pose reaches past the renderer's slot.

    // Every surviving pixel's place in posed frame space, from its world place,
    // inside the species' pad; and every cell of the pose's bounds, if it names a
    // pixel, names one whose forward place is within a cell (squared distance 2)
    // of that cell. Returns false on the first violation.
    auto pose_is_sound = [](const Enemy& e, std::string& why) {
        const Species& k = e.species();
        const body_art::Art& a = *k.art;
        const int pad = k.rig.pad;
        const int left = e.cell_x() - k.offset_x();
        const int top = e.cell_y() - k.offset_y();
        for (int y = 0; y < a.h; ++y)
            for (int x = 0; x < a.w; ++x) {
                if (!e.has_pixel(x, y)) continue;
                int wx = 0, wy = 0;
                e.world_of(x, y, wx, wy);
                int px = wx - left;
                if (e.facing_left()) px = a.w - 1 - px;
                const int py = wy - top;
                if (px < -pad || px >= a.w + pad || py < -pad || py >= a.h + pad) {
                    why = "pixel " + std::to_string(x) + "," + std::to_string(y) + " posed at " +
                          std::to_string(px) + "," + std::to_string(py) + " is outside the pad";
                    return false;
                }
            }
        const rig::Box b = e.pose_bounds();
        for (int py = b.y0; py < b.y1; ++py)
            for (int px = b.x0; px < b.x1; ++px) {
                const int i = e.posed_pixel(px, py);
                if (i < 0) continue;
                const int wx = e.facing_left() ? left + a.w - 1 - px : left + px;
                const int wy = top + py;
                if (e.pixel_at(wx, wy) != i) {
                    why = "pixel_at and posed_pixel disagree at " + std::to_string(px) + "," +
                          std::to_string(py);
                    return false;
                }
                int fx = 0, fy = 0;
                e.world_of(i % a.w, i / a.w, fx, fy);
                const int dx = fx - wx, dy = fy - wy;
                if (dx * dx + dy * dy > 2) {
                    why = "pixel " + std::to_string(i) + " drawn at " + std::to_string(px) + "," +
                          std::to_string(py) + " but its grain would land " + std::to_string(dx) +
                          "," + std::to_string(dy) + " away";
                    return false;
                }
            }
        return true;
    };
    {
        // A ghoul wandering, then chasing and swiping, then hit: every pose sound,
        // and the feet swing both ahead of and behind where they stand at rest.
        Run run(WORLD_W, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x)
                run.grid.set_element(x, y, ElementType::Wall);
        run.player = Player(240, FLOOR_Y - Player::HEIGHT);
        run.spawn_enemy(60, FLOOR_Y - G.height);
        const int foot_x = 9, foot_y = G.frame_h() - 1;  // the front foot's toe
        int ahead = 0, behind = 0, swipes = 0;
        bool sound = true, raised = false;
        std::string why;
        for (int i = 0; i < 400 && sound; ++i) {
            run.step(Input{});
            const Enemy& e = run.enemies[0];
            if (!e.is_alive()) break;
            sound = pose_is_sound(e, why);
            const int rest = rest_x(e, foot_x);
            const int now = world_x(e, foot_x, foot_y);
            const int fwd = e.facing_left() ? rest - now : now - rest;
            if (fwd > 0) ++ahead;
            if (fwd < 0) ++behind;
            if (e.attack_recovery() == 0) {
                ++swipes;
                // The claw starts the stroke up by the head, not hanging at the hip.
                if (world_y(e, G.frame_w() - 1, 19) < rest_y(e, 10)) raised = true;
            }
        }
        check("a walking ghoul's foot swings ahead of where it stands at rest", ahead > 10,
              "ahead=" + std::to_string(ahead));
        check("and behind it", behind > 10, "behind=" + std::to_string(behind));
        check("a ghoul's swipe starts from over its head", swipes > 0 && raised,
              "swipes=" + std::to_string(swipes));
        check("every ghoul pose draws what is hit, within its pad", sound, why);
    }
    {
        // A troll's slam: the club goes up over its head in the wind-up and is
        // forward of the body when the blow lands, and every pose is sound.
        Run run = troll_arena(100 + T.width + 4);
        bool sound = true, overhead = false, forward_at_impact = false;
        std::string why;
        // The club's head: the bottom of the leading arm.
        const int club_x = TA.w - 3, club_y = TA.h - 2;
        for (int i = 0; i < 200 && sound; ++i) {
            run.step(Input{});
            const Enemy& e = run.enemies[0];
            sound = pose_is_sound(e, why);
            const int top_of_head = rest_y(e, 4);
            if (e.windup_left() > 0 && world_y(e, club_x, club_y) < top_of_head) overhead = true;
            if (e.attack_recovery() == 0) {
                const int cx = world_x(e, club_x, club_y);
                const int front = e.facing_left() ? e.cell_x() : e.cell_x() + T.width;
                forward_at_impact = e.facing_left() ? cx < front - T.reach / 2
                                                    : cx > front + T.reach / 2;
            }
        }
        check("the troll raises its club over its head to slam", overhead);
        check("and the club is well out in front of it when the blow lands", forward_at_impact);
        check("every troll pose draws what is hit, within its pad", sound, why);
    }
    {
        // A pose moves the pixels, never the damage: an arm shot off mid-stride
        // stays off through the rest of the walk.
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        const int fx = 1, fy = enemy_art::ARM_TOP + 4;
        e.shatter(g, world_x(e, fx, fy), world_y(e, fx, fy), Quiver::BITE_RADIUS);
        const int left_arm = count_pixels(e, is_left_arm);
        settle(g, e, 120);
        check("an arm shot off mid-stride does not grow back as the body walks on",
              count_pixels(e, is_left_arm) == left_arm && !e.has_pixel(fx, fy));
    }

    return report();
}
