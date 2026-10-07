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

// The world cell frame pixel (fx, fy) covers. Recomputed here from the public
// position and the same anchoring rule the header states, so a disagreement
// between the two is a failure rather than a tautology.
int world_x(const Enemy& e, int fx) {
    const int left = e.cell_x() - Enemy::OFFSET_X;
    return e.facing_left() ? left + (Enemy::FRAME_W - 1 - fx) : left + fx;
}
int world_y(const Enemy& e, int fy) { return e.cell_y() - Enemy::OFFSET_Y + fy; }

int count_pixels(const Enemy& e, bool (*pred)(int, int)) {
    int n = 0;
    for (int y = 0; y < Enemy::FRAME_H; ++y)
        for (int x = 0; x < Enemy::FRAME_W; ++x)
            if (pred(x, y) && e.has_pixel(x, y)) ++n;
    return n;
}

bool is_left_arm(int x, int y) { return enemy_art::is_arm(x, y) && x < enemy_art::BOX_LEFT; }
bool is_right_arm(int x, int y) { return enemy_art::is_arm(x, y) && x >= enemy_art::BOX_RIGHT; }
bool anything(int x, int y) { return enemy_art::is_body(x, y); }

// An enemy standing on the floor, settled, at x.
Enemy standing_enemy(Grid& g, int x) {
    Enemy e;
    e.spawn(x, FLOOR_Y - Enemy::HEIGHT - 4);
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
              e.cell_y() + Enemy::HEIGHT == FLOOR_Y && !e.overlaps_solid(g, e.cell_x(), e.cell_y()),
              "feet=" + std::to_string(e.cell_y() + Enemy::HEIGHT));
        check("it has arms and feet to start with", e.has_arms() && e.has_feet());
        check("nothing has turned to sand yet", count_sand(g) == 0);
    }

    // --- the arm: hit below the shoulder, and the whole forearm comes off ---
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        const int right_before = count_pixels(e, is_right_arm);

        // The elbow: the left arm's middle column, a few rows below the shoulder.
        const int fx = 1, fy = enemy_art::ARM_TOP + 5;
        const int wx = world_x(e, fx), wy = world_y(e, fy);
        check("the arm pixel is where the anchoring rule says", e.pixel_at(wx, wy) >= 0);

        const int lost = e.shatter(g, wx, wy, Quiver::BITE_RADIUS);

        bool hand_gone = true;
        for (int y = fy; y < Enemy::FRAME_H; ++y)
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
        for (int y = 0; y < Enemy::FRAME_H; ++y)
            for (int x = 0; x < Enemy::FRAME_W; ++x)
                if (enemy_art::is_head(x, y)) eye_y = y;
        e.shatter(g, world_x(e, Enemy::FRAME_W / 2), world_y(e, eye_y), Quiver::BITE_RADIUS);
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
        const int knee_y = Enemy::FRAME_H - 4;
        e.shatter(g, world_x(e, 5), world_y(e, knee_y), Quiver::BITE_RADIUS);
        check("one leg shot through still leaves a foot to stand on",
              e.is_alive() && e.has_feet());
        e.shatter(g, world_x(e, 9), world_y(e, knee_y), Quiver::BITE_RADIUS);
        check("both legs shot through and it collapses", !e.is_alive());
    }

    // --- severing never takes the heart's own side ---
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        const int before = e.pixel_count();
        // The outer edge of the right arm, at the shoulder.
        const int lost = e.shatter(g, world_x(e, Enemy::FRAME_W - 1), world_y(e, 13), 1);
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
            for (int x = e.cell_x() - 2; x < e.cell_x() + Enemy::WIDTH + 2; ++x)
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
        const int ex = 40 + 10 * Enemy::WIDTH;
        check("an enemy can be spawned into open air", run.spawn_enemy(ex, FLOOR_Y - Enemy::HEIGHT));
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
        run.spawn_enemy(60 + Enemy::NOTICE_X / 2, FLOOR_Y - Enemy::HEIGHT);
        int hurt_steps = 0;
        for (int i = 0; i < 600; ++i) {
            run.step(Input{});
            if (run.player.damage_this_step() > 0) ++hurt_steps;
        }
        check("an enemy that has seen you walks over and hurts you",
              run.player.health() < Player::MAX_HEALTH,
              "hp=" + std::to_string(run.player.health()));
        check("at the swipe's rate, not every step",
              hurt_steps <= 600 / Enemy::SWIPE_INTERVAL_STEPS + 1,
              "hurt_steps=" + std::to_string(hurt_steps));

        // Disarmed, it cannot.
        Run calm(WORLD_W, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x)
                calm.grid.set_element(x, y, ElementType::Wall);
        calm.player = Player(60, FLOOR_Y - Player::HEIGHT);
        calm.spawn_enemy(60 + Enemy::NOTICE_X / 2, FLOOR_Y - Enemy::HEIGHT);
        Enemy& e = calm.enemies[0];
        for (int y = enemy_art::ARM_TOP; y < Enemy::FRAME_H; ++y) {
            e.shatter(calm.grid, world_x(e, 1), world_y(e, y), 1);
            e.shatter(calm.grid, world_x(e, Enemy::FRAME_W - 2), world_y(e, y), 1);
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
            if (e.cell_y() + Enemy::HEIGHT != FLOOR_Y || e.overlaps_solid(run.grid, e.cell_x(), e.cell_y()))
                standing = false;
            if (std::abs(e.center_x() - run.player.center_x()) < boot::ENEMY_CLEARANCE) clear = false;
        }
        check("a scene gets several enemies", planted.placed >= 3 && planted.placed == run.enemies_alive(),
              "placed=" + std::to_string(planted.placed));
        check("each planted on the floor, not in it", standing);
        check("and none close enough to have noticed the player at the start", clear);

        // A world with no terrain at all -- the `floor` scenes -- stands them on the
        // bottom border, where the player stands.
        Run empty(688, WORLD_H);
        empty.player = Player(344, WORLD_H - Player::HEIGHT);
        const boot::EnemyPlanting on_border = boot::plant_enemies(empty);
        bool on_floor = on_border.placed > 0;
        for (const Enemy& e : empty.enemies)
            if (e.is_alive() && e.cell_y() + Enemy::HEIGHT != WORLD_H) on_floor = false;
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
            run.spawn_enemy(150, FLOOR_Y - Enemy::HEIGHT);
            run.spawn_enemy(230, FLOOR_Y - Enemy::HEIGHT);
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

    return report();
}
