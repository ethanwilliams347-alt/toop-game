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
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

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
    // Every Run below is on the heap, behind a `Run&` of the name a stack Run
    // would have had. A Run is about 120 KB -- 24 enemy slots, each carrying a
    // pixel mask sized for the largest frame, the fish's 94x84 at four bits a
    // pixel -- and this function declares a dozen of them in sibling blocks.
    // MSVC does not reliably give sibling blocks the same stack slots, and under
    // ASan never does (each local gets its own slot, wrapped in red zones, so a
    // use after its scope ends is caught), so on Windows' 1 MB default stack
    // they add up and the suite dies of a stack overflow before printing a line.
    // Linux's 8 MB stack hides it. The heap is the fix rather than a bigger
    // stack: a linker flag only moves the line a few more Runs away.

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
        e.shatter(g, world_x(e, G.frame_w() / 2, eye_y), world_y(e, G.frame_w() / 2, eye_y),
                  Quiver::BITE_RADIUS);
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
        const int lost =
            e.shatter(g, world_x(e, G.frame_w() - 1, 13), world_y(e, G.frame_w() - 1, 13), 1);
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

    // --- a burnt pixel scorches its own grain, not the ground it fell on ---
    //
    // A pixel whose cell holds loose powder drops its grain on top of the powder,
    // so the grain is not in the pixel's cell. The scorch used to darken the
    // pixel's cell regardless, which is the terrain sand under the feet -- a little
    // darker every burn tick, until the ground under a burning body was black.
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        // Hot sand where the feet are: a cell keeps its temperature when it is
        // overwritten, so Fire then Sand leaves sand at Fire's temperature.
        const int row = FLOOR_Y - 1;
        std::vector<uint32_t> before;
        for (int x = e.cell_x(); x < e.cell_x() + G.width; ++x) {
            g.set_element(x, row, ElementType::Fire);
            g.set_element(x, row, ElementType::Sand);
            before.push_back(g.get_element(x, row).color);
        }
        const int pixels = e.pixel_count();
        e.update(g, -10000, -10000, false);
        check("a foot standing in hot sand burns", e.pixel_count() < pixels);
        bool ground_untouched = true;
        for (int x = e.cell_x(), i = 0; i < G.width; ++x, ++i) {
            const Element c = g.get_element(x, row);
            if (c.type != ElementType::Sand || c.color != before[static_cast<size_t>(i)])
                ground_untouched = false;
        }
        check("the sand it burns in keeps its colour; the scorch is on the new grain",
              ground_untouched);
    }

    // --- a body shattered inside a deep drift keeps every grain ---
    //
    // A pixel over loose powder puts its grain on top of the powder. The climb
    // used to stop at twice the frame's height, so under a drift deeper than that
    // the grain landed inside the sand and was dropped -- a body that lost pixels
    // without the grid gaining them, which is the bug this suite exists for.
    {
        Grid g = make_world();
        Enemy e;
        e.spawn(100, FLOOR_Y - G.height);
        const int drift_top = FLOOR_Y - 3 * G.frame_h();  // well past the old cap
        for (int y = drift_top; y < FLOOR_Y; ++y)
            for (int x = 80; x < 140; ++x) g.set_element(x, y, ElementType::Sand);
        const int sand_before = count_sand(g);
        const int pixels = e.pixel_count();
        e.shatter(g, e.center_x(), e.center_y(), 4 * G.frame_h());
        check("a body shattered in a deep drift comes apart entirely", !e.is_alive());
        check("...and every pixel is a grain on top of the drift",
              count_sand(g) - sand_before == pixels,
              std::to_string(count_sand(g) - sand_before) + " grains for " +
                  std::to_string(pixels) + " pixels");
    }

    // --- an enemy buried to the waist works itself free ---
    //
    // Deeper than it can step out of, the stuck branch used to hold it still for
    // good -- no gravity, no walking -- until the player dug it out. It now pushes
    // the powder out of its box the way a wading species does, and recovers.
    {
        Grid g = make_world();
        Enemy e = standing_enemy(g, 100);
        for (int y = FLOOR_Y - 12; y < FLOOR_Y; ++y)
            for (int x = e.cell_x() - 3; x < e.cell_x() + G.width + 3; ++x)
                if (g.get_element(x, y).type == ElementType::Empty)
                    g.set_element(x, y, ElementType::Sand);
        check("an enemy with sand to its waist starts out stuck",
              e.overlaps_solid(g, e.cell_x(), e.cell_y()));
        int steps = 0;
        for (; steps < 120 && e.overlaps_solid(g, e.cell_x(), e.cell_y()); ++steps) {
            g.update();
            e.update(g, -10000, -10000, false);
        }
        check("...and works itself free rather than standing frozen",
              e.is_alive() && !e.overlaps_solid(g, e.cell_x(), e.cell_y()),
              "steps=" + std::to_string(steps));
    }

    // --- a pool full of sinking arrows does not stop the bow ---
    //
    // Fluid drag slows an arrow in deep water to a sixth of a cell a step, so a bow
    // held over a lake fills the pool with arrows still in flight. With every slot
    // flying and none stuck, the quiver used to refuse the shot -- a bow that
    // silently stops working. It recycles the longest-flying arrow instead.
    {
        const auto run_owner = std::make_unique<Run>(200, 400);
        Run& run = *run_owner;
        for (int x = 0; x < 40; ++x) run.grid.set_element(x, 60, ElementType::Wall);  // a ledge
        for (int y = 61; y < 400; ++y)
            for (int x = 0; x < 200; ++x) run.grid.set_element(x, y, ElementType::Water);
        run.player = Player(32, 60 - Player::HEIGHT);
        run.player.settle(run.grid);

        Input held;
        held.shoot = true;
        held.cursor_x = 199;  // out over the water, clear of the ledge
        held.cursor_y = 70;
        const int shots = Quiver::CAPACITY + 5;
        int loosed = 0, most_flying = 0;
        for (int i = 0; i < shots * Quiver::DRAW_STEPS; ++i) {
            run.step(held);
            loosed += run.events().count(Event::Kind::ArrowLoosed);
            int flying = 0;
            for (const Arrow& a : run.quiver.arrows())
                if (a.live && !a.stuck) ++flying;
            if (flying > most_flying) most_flying = flying;
        }
        check("the pool of arrows fills with arrows still in flight",
              most_flying == Quiver::CAPACITY, "most flying=" + std::to_string(most_flying));
        check("and the bow keeps firing on its draw regardless", loosed == shots,
              "loosed " + std::to_string(loosed) + " of " + std::to_string(shots));
    }

    // --- the bow and the arrow ---
    {
        const auto run_owner = std::make_unique<Run>(WORLD_W, WORLD_H);
        Run& run = *run_owner;
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
        check("events: loosing an arrow is reported",
              run.events().count(Event::Kind::ArrowLoosed) == 1);

        int live = 0;
        for (const Arrow& a : run.quiver.arrows()) if (a.live) ++live;
        check("holding the bow looses one arrow", live == 1);

        int steps = 0;
        const int before = e.pixel_count();
        Input idle;
        int hit_events = 0, hit_pixels = 0;
        while (steps < 120 && run.enemies[0].pixel_count() == before) {
            run.step(idle);
            ++steps;
            for (const Event& ev : run.events()) {
                if (ev.kind != Event::Kind::ArrowHit) continue;
                ++hit_events;
                hit_pixels += ev.b;
            }
        }
        check("events: the hit is one ArrowHit, with the pixels it took",
              hit_events == 1 && (hit_pixels == before - run.enemies[0].pixel_count() ||
                                  !run.enemies[0].is_alive()),
              "events=" + std::to_string(hit_events) + " px=" + std::to_string(hit_pixels));
        check("the arrow reaches the enemy and takes pixels out of it",
              run.enemies[0].pixel_count() < before, "steps=" + std::to_string(steps));
        check("those pixels are in the grid as sand",
              count_sand(run.grid) == before - run.enemies[0].pixel_count() ||
                  !run.enemies[0].is_alive());

        // A held bow fires on its draw, not on every step.
        const auto r2_owner = std::make_unique<Run>(WORLD_W, WORLD_H);
        Run& r2 = *r2_owner;
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
        const auto run_owner = std::make_unique<Run>(WORLD_W, WORLD_H);
        Run& run = *run_owner;
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
        check("at the swipe's rate, not every step", hurt_steps <= 600 / G.attack_interval + 1,
              "hurt_steps=" + std::to_string(hurt_steps));

        // Disarmed, it cannot.
        const auto calm_owner = std::make_unique<Run>(WORLD_W, WORLD_H);
        Run& calm = *calm_owner;
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
        const auto run_owner = std::make_unique<Run>(WORLD_W, WORLD_H);
        Run& run = *run_owner;
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
        const auto run_owner = std::make_unique<Run>(688, WORLD_H);
        Run& run = *run_owner;
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
        const auto empty_owner = std::make_unique<Run>(688, WORLD_H);
        Run& empty = *empty_owner;
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
            const auto run_owner = std::make_unique<Run>(WORLD_W, WORLD_H, 7);
            Run& run = *run_owner;
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
        // Both sides are the same call on purpose: the claim is that calling it twice
        // gives the same answer.
        check("two runs from the same inputs end in the same world",
              play() == play());  // NOLINT(misc-redundant-expression)

        // The same volley, reading the events: every kill the run counts is one
        // EnemyKilled, naming the slot that died.
        const auto run_owner = std::make_unique<Run>(WORLD_W, WORLD_H, 7);
        Run& run = *run_owner;
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x) run.grid.set_element(x, y, ElementType::Wall);
        run.player = Player(40, FLOOR_Y - Player::HEIGHT);
        run.spawn_enemy(150, FLOOR_Y - G.height);
        run.spawn_enemy(230, FLOOR_Y - G.height);
        int killed = 0;
        bool slots_dead = true;
        for (int i = 0; i < 400; ++i) {
            Input in;
            in.shoot = (i % 3) == 0;
            in.cursor_x = 150 + (i % 90);
            in.cursor_y = FLOOR_Y - 15 + (i % 7);
            run.step(in);
            for (const Event& ev : run.events()) {
                if (ev.kind != Event::Kind::EnemyKilled) continue;
                ++killed;
                slots_dead &= !run.enemies[static_cast<size_t>(ev.a)].is_alive();
            }
        }
        check("events: one EnemyKilled per kill, naming a dead slot",
              run.kills() > 0 && killed == run.kills() && slots_dead,
              "kills=" + std::to_string(run.kills()) + " events=" + std::to_string(killed));
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
        const int lost =
            e.shatter(g, world_x(e, TA.w / 2, 42), world_y(e, TA.w / 2, 42), Quiver::BITE_RADIUS);
        check("an arrow in the belly takes exactly its bite", lost == 13 && e.is_alive(),
              "lost=" + std::to_string(lost));
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
        const int first = e.shatter(g, world_x(e, arm_x - 2, arm_y), world_y(e, arm_x - 2, arm_y),
                                    Quiver::BITE_RADIUS);
        bool hand_left = false;
        for (int y = 52; y < TA.h; ++y)
            for (int x = 0; x < TA.box_left; ++x)
                if (e.has_pixel(x, y)) hand_left = true;
        check("one arrow into an arm that thick is a bite, and the hand stays on",
              first <= 13 && hand_left, "first=" + std::to_string(first));
        // Two more across the same height, through the rest of the arm.
        const int rest = e.shatter(g, world_x(e, arm_x + 1, arm_y), world_y(e, arm_x + 1, arm_y),
                                   Quiver::BITE_RADIUS) +
                         e.shatter(g, world_x(e, arm_x + 3, arm_y), world_y(e, arm_x + 3, arm_y),
                                   Quiver::BITE_RADIUS);
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
        e.shatter(g, world_x(e, left_eye + 1, eye_y), world_y(e, left_eye + 1, eye_y),
                  Quiver::BITE_RADIUS);
        e.shatter(g, world_x(e, right_eye - 1, eye_y), world_y(e, right_eye - 1, eye_y),
                  Quiver::BITE_RADIUS);
        check("one into each eye does", !e.is_alive());
        check("and all of it comes down as sand", count_sand(g) == troll_art::PIXEL_COUNT,
              "sand=" + std::to_string(count_sand(g)));
    }

    // --- the slam: winds up, lands, breaks the ground ---
    // Returns the owner, as fish_arena does: returning the Run would copy it out
    // of the heap into the caller's frame, which is the stack Run this file
    // exists to avoid.
    auto troll_arena = [](int player_x) {
        auto run = std::make_unique<Run>(WORLD_W, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x) run->grid.set_element(x, y, ElementType::Wall);
        run->player = Player(player_x, FLOOR_Y - Player::HEIGHT);
        run->spawn_enemy(100, FLOOR_Y - species::TROLL.height, species::TROLL);
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
        const auto run_owner = troll_arena(100 + T.width + 4);
        Run& run = *run_owner;
        const int walls = count_type(run.grid, ElementType::Wall);
        int wound_up_at = -1, hit_at = -1, x_at_windup = 0;
        int windup_event_at = -1, slam_event_at = -1, hurt_events = 0, hurt_amount = 0,
            hurt_source = -2;
        bool moved_while_winding = false;
        for (int i = 0; i < 300 && hit_at < 0; ++i) {
            run.step(Input{});
            for (const Event& ev : run.events()) {
                if (ev.kind == Event::Kind::SlamWindup) windup_event_at = i;
                if (ev.kind == Event::Kind::Slam) slam_event_at = i;
                if (ev.kind == Event::Kind::PlayerHurt) {
                    ++hurt_events;
                    hurt_amount = ev.a;
                    hurt_source = ev.b;
                }
            }
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
        check("for the troll's damage", run.player.health() == Player::MAX_HEALTH - T.damage,
              "hp=" + std::to_string(run.player.health()));
        check("events: the wind-up and the blow are each reported on their step",
              windup_event_at == wound_up_at && slam_event_at == hit_at,
              "windup ev=" + std::to_string(windup_event_at) +
                  " slam ev=" + std::to_string(slam_event_at));
        check("events: ...and the hit as one PlayerHurt naming the troll and its damage",
              hurt_events == 1 && hurt_amount == T.damage &&
                  hurt_source == species::index_of(species::TROLL));
        check("events: nothing is dropped", run.events().dropped() == 0);
        const int broken = walls - count_type(run.grid, ElementType::Wall);
        check("and the ground where it lands breaks into sand",
              broken > 20 && count_type(run.grid, ElementType::Sand) == broken,
              "broken=" + std::to_string(broken) +
                  " sand=" + std::to_string(count_type(run.grid, ElementType::Sand)));
    }
    {
        // The same, but the player walks out from under it once it starts.
        const auto run_owner = troll_arena(100 + T.width + 4);
        Run& run = *run_owner;
        bool started = false, landed = false;
        int hurt = 0, slams = 0, hurt_events = 0;
        for (int i = 0; i < 300 && !landed; ++i) {
            Input in;
            in.right = started;
            const bool winding = run.enemies[0].windup_left() > 0;
            run.step(in);
            slams += run.events().count(Event::Kind::Slam);
            hurt_events += run.events().count(Event::Kind::PlayerHurt);
            if (run.enemies[0].windup_left() > 0) started = true;
            if (winding && run.enemies[0].windup_left() == 0) landed = true;
            if (run.player.damage_this_step() > 0) ++hurt;
        }
        check("a slam you walk out from under misses", landed && hurt == 0,
              "landed=" + std::to_string(landed) + " hurt=" + std::to_string(hurt));
        check("events: a missed slam is still a Slam, with no PlayerHurt",
              slams == 1 && hurt_events == 0);
    }
    {
        // Both arms shot off: it has nothing to slam with.
        const auto run_owner = troll_arena(100 + T.width + 4);
        Run& run = *run_owner;
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
        const auto run_owner = troll_arena(10);
        Run& run = *run_owner;
        run.enemies[0] = Enemy{};
        Input in;
        in.command = Command::spawn(species::TROLL);
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
    // of that cell -- two cells (squared distance 8) for a species with
    // tentacles, whose cells filled in at the outside of a bend are drawn up to
    // that far from their pixel (tentacle.h). Returns false on the first
    // violation.
    auto pose_is_sound = [](const Enemy& e, std::string& why) {
        const Species& k = e.species();
        const int slack = k.tentacles ? 8 : 2;
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
                if (dx * dx + dy * dy > slack) {
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
        const auto run_owner = std::make_unique<Run>(WORLD_W, WORLD_H);
        Run& run = *run_owner;
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x) run.grid.set_element(x, y, ElementType::Wall);
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
        const auto run_owner = troll_arena(100 + T.width + 4);
        Run& run = *run_owner;
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
                forward_at_impact =
                    e.facing_left() ? cx < front - T.reach / 2 : cx > front + T.reach / 2;
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

    // --- snapped poses ---------------------------------------------------
    //
    // The other animation style: the same pose, its slow clocks held at key
    // points. It has to keep every promise the continuous one makes above, and
    // it has to actually be a handful of held frames, which is the whole point.

    // Which pixel is in every cell of the pose's bounds, hashed. In posed frame
    // space, so the body walking along does not change it and only the pose does.
    auto pose_signature = [](const Enemy& e) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&h](int v) { h = (h ^ static_cast<uint32_t>(v)) * 1099511628211ull; };
        const rig::Box b = e.pose_bounds();
        mix(b.x0);
        mix(b.y0);
        mix(b.x1);
        mix(b.y1);
        for (int py = b.y0; py < b.y1; ++py)
            for (int px = b.x0; px < b.x1; ++px) mix(e.posed_pixel(px, py));
        return h;
    };
    {
        // Snapped from the start; the command flips it, for bodies already there
        // and bodies spawned after, and a reset puts it back -- a recording starts
        // from a reset.
        const auto run_owner = std::make_unique<Run>(WORLD_W, WORLD_H);
        Run& run = *run_owner;
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x) run.grid.set_element(x, y, ElementType::Wall);
        run.spawn_enemy(60, FLOOR_Y - G.height);
        check("a fresh run snaps its enemies' poses",
              run.snapped_poses() && run.enemies[0].snapped_poses());
        Input in;
        in.command = Command{Command::Kind::TogglePoseSnap, 0};
        run.step(in);
        run.spawn_enemy(160, FLOOR_Y - G.height);
        check("the toggle unsnaps the run, the body already in it, and one spawned after",
              !run.snapped_poses() && !run.enemies[0].snapped_poses() &&
                  !run.enemies[1].snapped_poses());
        run.step(in);
        check("and toggles back", run.snapped_poses() && run.enemies[0].snapped_poses());
        run.step(in);
        run.reset(Grid::DEFAULT_SEED);
        check("a reset puts a continuous run back to snapped", run.snapped_poses());
    }
    {
        // A ghoul wandering: snapped, it shows a few dozen poses at most -- walk
        // keys times breath keys -- where turning continuously it shows hundreds.
        size_t distinct[2] = {0, 0};
        for (int mode = 0; mode < 2; ++mode) {
            Grid g = make_world();
            Enemy e;
            e.spawn(100, FLOOR_Y - G.height - 4);
            e.set_snapped_poses(mode == 1);
            std::set<uint64_t> seen;
            for (int i = 0; i < 600; ++i) {
                g.update();
                e.update(g, -10000, -10000, false);
                seen.insert(pose_signature(e));
            }
            distinct[mode] = seen.size();
        }
        const std::string counts =
            "continuous=" + std::to_string(distinct[0]) + " snapped=" + std::to_string(distinct[1]);
        check("a snapped ghoul's wander is a few dozen held poses", distinct[1] <= 40, counts);
        check("a small share of the continuous style's", distinct[1] * 4 < distinct[0], counts);
    }
    {
        // Snapped, every pose a wander, a chase, a swipe and a slam reach is still
        // sound, the ghoul's feet still swing, and the troll's club still goes up
        // over its head and lands out in front. Snapped is a fresh Run's default,
        // so these runs take no toggle.
        bool sound = true;
        std::string why;
        int ahead = 0, behind = 0;
        {
            const auto run_owner = std::make_unique<Run>(WORLD_W, WORLD_H);
            Run& run = *run_owner;
            for (int y = FLOOR_Y; y < WORLD_H; ++y)
                for (int x = 0; x < WORLD_W; ++x) run.grid.set_element(x, y, ElementType::Wall);
            run.player = Player(240, FLOOR_Y - Player::HEIGHT);
            run.spawn_enemy(60, FLOOR_Y - G.height);
            const int foot_x = 9, foot_y = G.frame_h() - 1;
            for (int i = 0; i < 400 && sound; ++i) {
                run.step(Input{});
                const Enemy& e = run.enemies[0];
                if (!e.is_alive()) break;
                sound = pose_is_sound(e, why);
                const int fwd = e.facing_left() ? rest_x(e, foot_x) - world_x(e, foot_x, foot_y)
                                                : world_x(e, foot_x, foot_y) - rest_x(e, foot_x);
                if (fwd > 0) ++ahead;
                if (fwd < 0) ++behind;
            }
        }
        check("snapped, every ghoul pose draws what is hit, within its pad", sound, why);
        check("snapped, a walking ghoul's foot still swings ahead and behind",
              ahead > 10 && behind > 10,
              "ahead=" + std::to_string(ahead) + " behind=" + std::to_string(behind));

        bool troll_sound = true, overhead = false, forward_at_impact = false;
        {
            const auto run_owner = std::make_unique<Run>(WORLD_W, WORLD_H);
            Run& run = *run_owner;
            for (int y = FLOOR_Y; y < WORLD_H; ++y)
                for (int x = 0; x < WORLD_W; ++x) run.grid.set_element(x, y, ElementType::Wall);
            run.player = Player(100 + T.width + 4, FLOOR_Y - Player::HEIGHT);
            run.spawn_enemy(100, FLOOR_Y - T.height, T);
            const int club_x = TA.w - 3, club_y = TA.h - 2;
            for (int i = 0; i < 200 && troll_sound; ++i) {
                run.step(Input{});
                const Enemy& e = run.enemies[0];
                troll_sound = pose_is_sound(e, why);
                if (e.windup_left() > 0 && world_y(e, club_x, club_y) < rest_y(e, 4))
                    overhead = true;
                if (e.attack_recovery() == 0) {
                    const int cx = world_x(e, club_x, club_y);
                    const int front = e.facing_left() ? e.cell_x() : e.cell_x() + T.width;
                    forward_at_impact =
                        e.facing_left() ? cx < front - T.reach / 2 : cx > front + T.reach / 2;
                }
            }
        }
        check("snapped, every troll pose draws what is hit, within its pad", troll_sound, why);
        check("snapped, the troll still raises its club over its head", overhead);
        check("and still lands it out in front", forward_at_impact);
    }

    // ================= the fish =================
    //
    // The body rules with the arm named by letter rather than by place: the lure
    // arm grows out of the top of the head, so everything the troll's arms get
    // from the box's columns, this one gets from fish_art's letters.
    const Species& F = species::FISH;
    const body_art::Art& FA = *F.art;
    // The lowest pixel of the arm: a fingertip of the dangling hand.
    int hand_x = 0, hand_y = -1;
    for (int y = 0; y < FA.h; ++y)
        for (int x = 0; x < FA.w; ++x)
            if (FA.is_arm(x, y) && y > hand_y) {
                hand_x = x;
                hand_y = y;
            }
    // On the heap, for the reason at the top of main().
    auto fish_arena = [&](int player_x) {
        auto run = std::make_unique<Run>(WORLD_W, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x) run->grid.set_element(x, y, ElementType::Wall);
        run->player = Player(player_x, FLOOR_Y - Player::HEIGHT);
        run->spawn_enemy(100, FLOOR_Y - F.height, F);
        return run;
    };

    // --- a fresh fish ---
    {
        Grid g = make_world();
        Enemy e;
        e.spawn(100, FLOOR_Y - F.height - 4, F);
        settle(g, e, 30);
        check("a spawned fish is alive and whole",
              e.is_alive() && e.pixel_count() == fish_art::PIXEL_COUNT,
              "pixels=" + std::to_string(e.pixel_count()));
        check("it stands on the floor", e.cell_y() + F.height == FLOOR_Y,
              "feet=" + std::to_string(e.cell_y() + F.height));
        check("it has its arm and its feet", e.has_arms() && e.has_feet());

        // Every arm letter is the front arm, wherever it sits, and nothing is the
        // rear arm; everything inside the box from the hips down is leg.
        bool arm_is_front = true, legs_are_legs = true;
        int arm_pixels = 0;
        for (int y = 0; y < FA.h; ++y)
            for (int x = 0; x < FA.w; ++x) {
                if (!FA.is_body(x, y)) continue;
                const rig::Part p = rig::part_of(FA, F.rig, x, y);
                if (FA.is_arm(x, y)) ++arm_pixels;
                if (FA.is_arm(x, y) != (p == rig::FrontArm) || p == rig::RearArm)
                    arm_is_front = false;
                const bool in_legs = y >= F.rig.hip_row && x >= FA.box_left && x < FA.box_right();
                if (in_legs != (p == rig::RearLeg || p == rig::FrontLeg)) legs_are_legs = false;
            }
        check("the lettered arm is exactly the front arm", arm_is_front && arm_pixels > 100,
              "arm=" + std::to_string(arm_pixels));
        check("and the legs are what the box holds below the hips", legs_are_legs);
        check("the arm's bounds hold every arm pixel",
              FA.arm_box.x0 <= hand_x && hand_x < FA.arm_box.x1 && FA.arm_box.y1 == hand_y + 1);
    }

    // --- the lure arm comes off at the shoulder ---
    {
        Grid g = make_world();
        Enemy e;
        e.spawn(100, FLOOR_Y - F.height, F);
        const int before = e.pixel_count();
        // Two arrows' bites where the arm leaves the head: the root is wider than
        // one bite, as the troll's arms are.
        const int sx = fish_art::SHOULDER_X, sy = fish_art::SHOULDER_Y;
        e.shatter(g, world_x(e, sx - 2, sy), world_y(e, sx - 2, sy), Quiver::BITE_RADIUS);
        e.shatter(g, world_x(e, sx + 2, sy), world_y(e, sx + 2, sy), Quiver::BITE_RADIUS);
        check("two shots through the shoulder take the whole arm off",
              e.is_alive() && !e.has_arms() && !e.has_pixel(hand_x, hand_y),
              "arms=" + std::to_string(e.has_arms()));
        check("as sand, grain for pixel", count_sand(g) == before - e.pixel_count(),
              "sand=" + std::to_string(count_sand(g)) +
                  " lost=" + std::to_string(before - e.pixel_count()));
    }

    // --- one arrow in the eye is not enough; two are ---
    {
        Grid g = make_world();
        Enemy e;
        e.spawn(100, FLOOR_Y - F.height, F);
        int ex = -1, ey = -1;  // the eye's top-left 'O'
        for (int y = 0; y < FA.h && ex < 0; ++y)
            for (int x = 0; x < FA.w && ex < 0; ++x)
                if (FA.at(x, y) == 'O') {
                    ex = x;
                    ey = y;
                }
        e.shatter(g, world_x(e, ex, ey), world_y(e, ex, ey), Quiver::BITE_RADIUS);
        check("an arrow through the pupil leaves the fish standing", e.is_alive());
        e.shatter(g, world_x(e, ex + 2, ey + 2), world_y(e, ex + 2, ey + 2), Quiver::BITE_RADIUS);
        check("a second through what is left of the eye kills it", !e.is_alive());
        check("and all of it comes down as sand", count_sand(g) == fish_art::PIXEL_COUNT,
              "sand=" + std::to_string(count_sand(g)));
    }

    // --- the cast: back over the head, down in front, nothing broken ---
    {
        const auto run_owner = fish_arena(100 + F.width + 4);
        Run& run = *run_owner;
        const int walls = count_type(run.grid, ElementType::Wall);
        bool sound = true, overhead = false, forward_at_impact = false, moved = false;
        int wound_up_at = -1, hit_at = -1, x_at_windup = 0;
        std::string why;
        for (int i = 0; i < 300 && sound && hit_at < 0; ++i) {
            run.step(Input{});
            const Enemy& e = run.enemies[0];
            sound = pose_is_sound(e, why);
            if (wound_up_at < 0 && e.windup_left() > 0) {
                wound_up_at = i;
                x_at_windup = e.cell_x();
            }
            if (e.windup_left() > 0 && e.cell_x() != x_at_windup) moved = true;
            // Over the head: the hand above the shoulder it hangs from.
            if (e.windup_left() > 0 && world_y(e, hand_x, hand_y) < rest_y(e, fish_art::SHOULDER_Y))
                overhead = true;
            if (run.player.damage_this_step() > 0) {
                hit_at = i;
                const int hx = world_x(e, hand_x, hand_y);
                const int front = e.facing_left() ? e.cell_x() : e.cell_x() + F.width;
                forward_at_impact = e.facing_left() ? hx < front : hx > front;
            }
        }
        check("a fish with you in reach winds up a cast and stands still for it",
              wound_up_at >= 0 && !moved);
        check("the hand goes back over its head", overhead);
        check("the cast lands after the wind-up", hit_at - wound_up_at == F.windup_steps,
              "wound=" + std::to_string(wound_up_at) + " hit=" + std::to_string(hit_at));
        check("with the hand out in front of the body", forward_at_impact);
        check("for the fish's damage", run.player.health() == Player::MAX_HEALTH - F.damage,
              "hp=" + std::to_string(run.player.health()));
        check("and the ground it lands on is left whole",
              count_type(run.grid, ElementType::Wall) == walls &&
                  count_type(run.grid, ElementType::Sand) == 0);
        check("every fish pose through a cast draws what is hit, within its pad", sound, why);
    }

    // --- the walk ---
    {
        // Wandering, so it walks the whole time; then the same snapped.
        for (const bool snapped : {false, true}) {
            const auto run_owner = fish_arena(WORLD_W * 4);
            Run& run = *run_owner;
            run.enemies[0].set_snapped_poses(snapped);
            // The front foot's toe, the bottom row's last pixel inside the box.
            int toe = -1;
            for (int x = FA.box_right() - 1; x >= FA.box_left && toe < 0; --x)
                if (FA.is_foot(x, FA.h - 2)) toe = x;
            int ahead = 0, behind = 0;
            bool sound = true;
            std::string why;
            for (int i = 0; i < 300 && sound; ++i) {
                run.step(Input{});
                const Enemy& e = run.enemies[0];
                sound = pose_is_sound(e, why);
                const int rest = rest_x(e, toe);
                const int now = world_x(e, toe, FA.h - 2);
                const int fwd = e.facing_left() ? rest - now : now - rest;
                if (fwd > 0) ++ahead;
                if (fwd < 0) ++behind;
            }
            const std::string mode = snapped ? " (snapped)" : "";
            check(("a walking fish's foot swings ahead and behind" + mode).c_str(),
                  ahead > 10 && behind > 10,
                  "ahead=" + std::to_string(ahead) + " behind=" + std::to_string(behind));
            check(("every walking fish pose is sound" + mode).c_str(), sound, why);
        }
    }

    // ================= the angler =================
    //
    // The fish on tentacles: its legs and lure are chains of links (tentacle.h)
    // rather than parts turned whole. Everything the body rules promise has to
    // survive that -- a fresh body maps every pixel to itself, what is drawn is
    // what is hit, a cut drops what is past it -- and so do the two things the
    // chains add: a foot on the ground stays put, and the lure strikes where the
    // blow lands.
    const Species& AN = species::ANGLER;
    const body_art::Art& AA = *AN.art;
    const tentacle::Set& TS = *AN.tentacles;
    const tentacle::Chain& rear_chain = TS.chain[0];
    const tentacle::Chain& lure_chain = TS.chain[2];
    int bulb_x = -1, bulb_y = -1;  // the bulb's glowing core
    for (int y = 0; y < AA.h && bulb_x < 0; ++y)
        for (int x = 0; x < AA.w && bulb_x < 0; ++x)
            if (AA.at(x, y) == 'v') {
                bulb_x = x;
                bulb_y = y;
            }
    const rig::Joint rear_tip = rear_chain.rest[rear_chain.links()];
    const rig::Joint front_tip = TS.chain[1].rest[TS.chain[1].links()];
    auto angler_arena = [&](int player_x) {
        auto run = std::make_unique<Run>(WORLD_W, WORLD_H);
        for (int y = FLOOR_Y; y < WORLD_H; ++y)
            for (int x = 0; x < WORLD_W; ++x) run->grid.set_element(x, y, ElementType::Wall);
        run->player = Player(player_x, FLOOR_Y - Player::HEIGHT);
        run->spawn_enemy(100, FLOOR_Y - AN.height, AN);
        return run;
    };

    // --- the chain arithmetic ---
    {
        // Reach keeps every link its rest length and puts a reachable tip on its
        // target; the rest chain turned onto itself is the identity, to the bit.
        const int n = rear_chain.links();
        const tentacle::Lengths len = tentacle::rest_lengths(rear_chain);
        tentacle::Points p = tentacle::rest_points(rear_chain);
        const rig::Point root = p[0];
        const rig::Point target{p[n].x + fx::from_int(9), p[n].y - fx::from_int(4)};
        // Run to convergence: in the game three rounds a step is enough because
        // the next step carries on from where this one stopped.
        tentacle::reach(p, len, n, root, target, 12);
        fx::v worst = 0;
        for (int i = 0; i < n; ++i)
            worst = std::max(
                worst,
                std::abs(tentacle::length(p[i + 1].x - p[i].x, p[i + 1].y - p[i].y) - len[i]));
        const fx::v miss = tentacle::length(p[n].x - target.x, p[n].y - target.y);
        check("a solved chain keeps every link its length", worst <= 4,
              "worst=" + std::to_string(worst));
        check("and puts a reachable tip on its target, within a tenth of a cell",
              miss < fx::ONE / 10, "miss=" + std::to_string(miss));
        check("p[0] stays on the root", p[0].x == root.x && p[0].y == root.y);
        const tentacle::Points rest = tentacle::rest_points(lure_chain);
        bool identity = true;
        for (int i = 0; i < lure_chain.links(); ++i) {
            const rig::Xform t = tentacle::link_turn(lure_chain, i, rest[i], rest[i + 1]);
            identity = identity && t.c == fx::ONE && t.s == 0 && t.tx == 0 && t.ty == 0;
        }
        check("a link at rest turns by nothing at all", identity);
        // The lure hooks back under itself, which is what broke a link_of that
        // cut the chain by straight lines: the bulb belongs to the last link.
        check("the bulb belongs to the lure's last link",
              tentacle::link_of(lure_chain, bulb_x, bulb_y) == lure_chain.links() - 1,
              "link=" + std::to_string(tentacle::link_of(lure_chain, bulb_x, bulb_y)));
    }

    // --- second-order followers ---
    {
        // A target that jumps from 0 to 10 cells, followed for 200 steps:
        // where the follower got to, how far past 10 it went, and how far
        // below 0 it went first.
        struct Track {
            fx::v end, peak, dip;
        };
        auto track = [](const tentacle::Response& k) {
            tentacle::Follow f;
            Track t{0, 0, 0};
            for (int i = 0; i < 200; ++i) {
                tentacle::follow(f, fx::from_int(10), k);
                t.peak = std::max(t.peak, f.pos);
                t.dip = std::min(t.dip, f.pos);
            }
            t.end = f.pos;
            return t;
        };
        const Track loose = track(tentacle::response(20, fx::from_ratio(1, 3), 0));
        const Track firm = track(tentacle::response(20, fx::ONE, 0));
        const Track wary = track(tentacle::response(20, fx::ONE, -fx::ONE));
        const Track wild = track(tentacle::response(1, fx::ONE / 10, fx::from_int(4)));
        check("a follower settles on its target",
              std::abs(loose.end - fx::from_int(10)) < fx::ONE / 8 &&
                  std::abs(firm.end - fx::from_int(10)) < fx::ONE / 8,
              "loose=" + std::to_string(loose.end) + " firm=" + std::to_string(firm.end));
        check("underdamped, it swings past the target first", loose.peak > fx::from_int(11),
              "peak=" + std::to_string(loose.peak));
        check("critically damped, it does not", firm.peak <= fx::from_int(10) + fx::ONE / 16,
              "peak=" + std::to_string(firm.peak));
        check("with a negative response, it dips the wrong way first (anticipation)",
              wary.dip < -fx::ONE / 2 && firm.dip == 0, "dip=" + std::to_string(wary.dip));
        check("the stability clamp keeps even a wild tuning bounded",
              wild.peak < fx::from_int(100) && wild.dip > -fx::from_int(100),
              "peak=" + std::to_string(wild.peak) + " dip=" + std::to_string(wild.dip));
    }

    // --- a fresh angler ---
    {
        Grid g = make_world();
        Enemy::Chains chains;
        Enemy e;
        e.spawn(100, FLOOR_Y - AN.height, AN, &chains);
        // At rest every pixel is drawn exactly where the art has it, including
        // every tentacle pixel through its own link.
        int misplaced = 0;
        std::string first;
        for (int y = 0; y < AA.h; ++y)
            for (int x = 0; x < AA.w; ++x) {
                if (!AA.is_body(x, y) || e.posed_pixel(x, y) == y * AA.w + x) continue;
                if (misplaced++ == 0) first = std::to_string(x) + "," + std::to_string(y);
            }
        check("a fresh angler draws every pixel where its art has it", misplaced == 0,
              "misplaced=" + std::to_string(misplaced) + " first=" + first);
        // Every tentacle pixel is within its chain's radius of its own link: the
        // bound place_links draws round each posed link to look for them.
        bool within = true;
        std::string far;
        for (int k = 0; k < TS.count && within; ++k) {
            const tentacle::Chain& c = TS.chain[k];
            for (int y = 0; y < AA.h; ++y)
                for (int x = 0; x < AA.w; ++x) {
                    if (!AA.is_body(x, y) || rig::part_of(AA, AN.rig, x, y) != c.part) continue;
                    const int i = tentacle::link_of(c, x, y);
                    const rig::Joint a = c.rest[i], b = c.rest[i + 1];
                    const double ux = b.x - a.x, uy = b.y - a.y;
                    const double t = std::clamp(
                        ((x - a.x) * ux + (y - a.y) * uy) / (ux * ux + uy * uy), 0.0, 1.0);
                    const double dx = x - a.x - t * ux, dy = y - a.y - t * uy;
                    if (dx * dx + dy * dy > double(c.radius) * c.radius) {
                        within = false;
                        far = std::to_string(x) + "," + std::to_string(y);
                    }
                }
        }
        check("every tentacle pixel is within its chain's radius of its link", within, far);
        settle(g, e, 30);
        check("a spawned angler is alive and whole",
              e.is_alive() && e.pixel_count() == angler_art::PIXEL_COUNT,
              "pixels=" + std::to_string(e.pixel_count()));
        check("it stands on the floor", e.cell_y() + AN.height == FLOOR_Y,
              "feet=" + std::to_string(e.cell_y() + AN.height));
        check("it has its lure and its feet", e.has_arms() && e.has_feet());
    }

    // --- the walk: feet plant, alternate, and never skate ---
    {
        const auto run_owner = angler_arena(WORLD_W * 4);
        Run& run = *run_owner;
        // In the continuous style, which this was written against. Snapped -- a
        // fresh Run's default -- a planted foot slips a couple of cells now and
        // then as the body's held pose changes under it, which is a known gap
        // (see "snapped poses" in enemy.h) rather than what this checks.
        {
            Input in;
            in.command = Command{Command::Kind::TogglePoseSnap, 0};
            run.step(in);
        }
        bool sound = true;
        std::string why;
        int skates = 0, rear_up = 0, front_up = 0, both_up = 0;
        int prev[2][2] = {{0, 0}, {0, 0}};
        const int start_x = run.enemies[0].cell_x();
        const rig::Joint tips[2] = {rear_tip, front_tip};
        for (int i = 0; i < 300 && sound; ++i) {
            run.step(Input{});
            const Enemy& e = run.enemies[0];
            sound = pose_is_sound(e, why);
            bool up[2];
            for (int k = 0; k < 2; ++k) {
                const int x = world_x(e, tips[k].x, tips[k].y),
                          y = world_y(e, tips[k].x, tips[k].y);
                up[k] = y < FLOOR_Y - 3;
                // A foot standing on the floor two steps running has not moved
                // under the body; a cell of rounding is allowed for, since a foot
                // is held still at the body's sub-cell position and drawn at its
                // whole one. On the floor means on it: a foot a cell up is at the
                // start or end of a swing, and moving.
                if (i > 0 && y == FLOOR_Y - 1 && prev[k][1] == FLOOR_Y - 1 &&
                    std::abs(x - prev[k][0]) > 1)
                    ++skates;
                prev[k][0] = x;
                prev[k][1] = y;
            }
            if (up[0] && !up[1]) ++rear_up;
            if (up[1] && !up[0]) ++front_up;
            if (up[0] && up[1]) ++both_up;
        }
        const int walked = std::abs(run.enemies[0].cell_x() - start_x);
        check("a wandering angler walks", walked > 60, "walked=" + std::to_string(walked));
        check("each tentacle lifts its foot in turn", rear_up > 10 && front_up > 10,
              "rear=" + std::to_string(rear_up) + " front=" + std::to_string(front_up));
        check("never both at once", both_up == 0, "both=" + std::to_string(both_up));
        check("and a planted foot does not skate", skates == 0, "skates=" + std::to_string(skates));
        check("every walking angler pose draws what is hit, within its pad", sound, why);
    }

    // --- uneven ground: feet find footholds, still one at a time ---
    {
        // A step five cells high across the angler's path. Feet are placed on
        // whatever ground is in reach of their home, so on the way over the
        // step each foot is put down on the step's top or the floor, never in
        // the wall, and the turn-taking holds whatever the ground does.
        //
        // Not "never both feet off the ground": standing on the step's corner,
        // the box five cells up, the rear foot cannot reach the floor and hangs.
        const auto run_owner = angler_arena(WORLD_W * 4);
        Run& run = *run_owner;
        const int step_x0 = 160, step_x1 = 210, step_top = FLOOR_Y - 5;
        for (int x = step_x0; x < step_x1; ++x)
            for (int y = step_top; y < FLOOR_Y; ++y) run.grid.set_element(x, y, ElementType::Wall);
        {
            Input in;
            in.command = Command{Command::Kind::TogglePoseSnap, 0};
            run.step(in);
        }
        const rig::Joint tips[2] = {rear_tip, front_tip};
        int both_up = 0, buried = 0, on_top = 0;
        // Two body pixels a row apart in height at rest -- the first and last
        // of the body's on a row through the middle of the fish -- so the body's
        // tilt is the difference in their heights: negative is nose up.
        const int mid_row = AN.rig.hip_row - 20;
        int tail_x = -1, nose_x = -1;
        for (int x = 0; x < AA.w; ++x) {
            if (!AA.is_body(x, mid_row) || rig::part_of(AA, AN.rig, x, mid_row) != rig::Body)
                continue;
            if (tail_x < 0) tail_x = x;
            nose_x = x;
        }
        int nose_up = 0, flat_tilt = 0, flat_seen = 0;
        bool sound = true;
        std::string why;
        for (int i = 0; i < 600 && sound; ++i) {
            run.step(Input{});
            const Enemy& e = run.enemies[0];
            sound = pose_is_sound(e, why);
            for (int k = 0; k < 2; ++k) {
                const int x = world_x(e, tips[k].x, tips[k].y),
                          y = world_y(e, tips[k].x, tips[k].y);
                // The cell above the tip solid: a foot put down inside the step
                // rather than on it, by more than a cell of rounding.
                if (is_solid(run.grid.get_element(x, y - 1).type)) ++buried;
                // Standing on the step: the step's top within a cell or two.
                if (x >= step_x0 && x < step_x1 && y < step_top && y >= step_top - 3) ++on_top;
            }
            const int tilt = world_y(e, nose_x, mid_row) - world_y(e, tail_x, mid_row);
            // Climbing on, the front foot is up on the step and the rear one
            // still on the floor: the body leans back, nose up.
            if (tilt <= -3) ++nose_up;
            // Back on even ground, past the step's far end by more than a
            // stride -- a rear foot can still be up on the step at ten cells
            // past it, and the body rightly leans down to it -- it levels.
            if (e.cell_x() > step_x1 + 20) {
                ++flat_seen;
                if (std::abs(tilt) > 2) ++flat_tilt;
            }
            // Counted from the body, not from where feet are drawn: a foot
            // dangling over a drop it cannot reach down to is not a step.
            if (e.feet_swinging() > 1) ++both_up;
        }
        check("over a step, an angler never has two feet in the middle of a step", both_up == 0,
              "both=" + std::to_string(both_up));
        check("and keeps stepping", run.enemies[0].footfall_count() > 20,
              "footfalls=" + std::to_string(run.enemies[0].footfall_count()));
        check("never put down inside the step", buried == 0, "buried=" + std::to_string(buried));
        check("climbing on, its body leans back with its feet, nose up", nose_up > 3,
              "nose_up=" + std::to_string(nose_up));
        check("and levels again on even ground", flat_seen > 20 && flat_tilt == 0,
              "seen=" + std::to_string(flat_seen) + " tilted=" + std::to_string(flat_tilt));
        check("and stand on its top on the way over", on_top > 20,
              "on_top=" + std::to_string(on_top));
        check("every angler pose over a step draws what is hit", sound, why);
    }

    // --- the strike: reared back over the head, whipped down in front ---
    {
        const auto run_owner = angler_arena(100 + AN.width + 4);
        Run& run = *run_owner;
        bool sound = true, overhead = false, forward_at_impact = false;
        int wound_up_at = -1, hit_at = -1;
        std::string why;
        for (int i = 0; i < 300 && sound && hit_at < 0; ++i) {
            run.step(Input{});
            const Enemy& e = run.enemies[0];
            sound = pose_is_sound(e, why);
            if (wound_up_at < 0 && e.windup_left() > 0) wound_up_at = i;
            if (e.windup_left() > 0 &&
                world_y(e, bulb_x, bulb_y) < rest_y(e, angler_art::SHOULDER_Y) - 20)
                overhead = true;
            if (run.player.damage_this_step() > 0) {
                hit_at = i;
                // The bulb is what hit: drawn on or beside the player.
                const int bx = world_x(e, bulb_x, bulb_y), by = world_y(e, bulb_x, bulb_y);
                const int px = run.player.cell_x(), py = run.player.cell_y();
                const int near = TS.motion.bulb_radius + 3;
                forward_at_impact = bx >= px - near && bx < px + Player::WIDTH + near &&
                                    by >= py - near && by < py + Player::HEIGHT + near;
            }
        }
        check("an angler with you in reach winds up a strike", wound_up_at >= 0);
        check("the bulb rears back high over its head", overhead);
        // The bulb hits what it touches, so the blow lands on the step of the
        // whip that reaches the player -- not before the whip, and not after
        // its follow-through.
        const int whip_from = AN.windup_steps - AN.rig.strike_steps;
        check("the strike lands in the whip, after the wind-up",
              hit_at - wound_up_at > whip_from &&
                  hit_at - wound_up_at <= AN.windup_steps + TS.motion.lure_live,
              "wound=" + std::to_string(wound_up_at) + " hit=" + std::to_string(hit_at));
        check("with the bulb where the player is", forward_at_impact);
        check("for the angler's damage", run.player.health() == Player::MAX_HEALTH - AN.damage,
              "hp=" + std::to_string(run.player.health()));
        check("every angler pose through a strike draws what is hit, within its pad", sound, why);
    }

    // --- the bulb's sweep: what it passed through, not only where it stopped ---
    {
        // A ball three cells round crossing a box two cells wide, starting
        // eight cells left of it and ending eight right: neither end is
        // anywhere near the box, and the sweep still finds it. One passing a
        // cell above its reach does not.
        const fx::v r = fx::from_int(3);
        const rig::Point a{fx::from_int(0), fx::from_int(10)},
            b{fx::from_int(20), fx::from_int(10)};
        check("a bulb that jumps clean over a thin target in one step still hits it",
              tentacle::sweep_touches(a, b, r, 9, 5, 11, 15));
        check("a bulb whose path passes clear of it does not",
              !tentacle::sweep_touches(a, b, r, 9, 14, 11, 20));
        check("and a bulb that only ends near it, short of its reach, does not",
              !tentacle::sweep_touches(a, {fx::from_int(5), fx::from_int(10)}, r, 9, 5, 11, 15));
    }

    // --- dodged: stepped out from under the wind-up, nothing lands ---
    {
        // In reach when it starts winding up; then gone, far behind it, before
        // the whip. The bulb hits only what it touches, so the strike lands on
        // the empty spot it was aimed at. A box test made where the player was
        // at the start would have hit them anyway; one made where they are at
        // the end would hit a player standing anywhere in reach, however far
        // the bulb came down from them.
        const auto run_owner = angler_arena(100 + AN.width + 4);
        Run& run = *run_owner;
        bool moved = false;
        for (int i = 0; i < AN.windup_steps * 3; ++i) {
            run.step(Input{});
            const Enemy& e = run.enemies[0];
            if (!moved && e.windup_left() > 0 && e.windup_left() < AN.windup_steps / 2) {
                run.player = Player(100 + AN.width + 40, FLOOR_Y - Player::HEIGHT);
                moved = true;
            }
            if (moved && e.windup_left() == 0) break;
        }
        for (int i = 0; i < 4; ++i) run.step(Input{});
        check("a player who steps out from under the wind-up is not hit",
              moved && run.player.health() == Player::MAX_HEALTH,
              "moved=" + std::to_string(moved) + " hp=" + std::to_string(run.player.health()));
    }

    // --- stung: an arrow in the bulb calls the strike off ---
    {
        const auto run_owner = angler_arena(100 + AN.width + 4);
        Run& run = *run_owner;
        bool stung = false;
        int recovery = 0;
        for (int i = 0; i < 400 && !stung; ++i) {
            run.step(Input{});
            Enemy& e = run.enemies[0];
            // At the top of the wind-up, where the bulb is held still.
            if (e.windup_left() == AN.rig.strike_steps + 1) {
                e.shatter(run.grid, world_x(e, bulb_x, bulb_y), world_y(e, bulb_x, bulb_y),
                          Quiver::BITE_RADIUS);
                stung = true;
                recovery = e.attack_recovery();
                check("an arrow in the held bulb calls the wound-up strike off",
                      e.windup_left() == 0, "windup=" + std::to_string(e.windup_left()));
            }
        }
        // Long enough for the strike it would have made and the start of the
        // next decision: nothing lands while it recovers.
        for (int i = 0; i < AN.attack_interval + AN.rig.strike_steps; ++i) run.step(Input{});
        check("and it lands nothing while it recovers from the sting",
              stung && run.player.health() == Player::MAX_HEALTH,
              "hp=" + std::to_string(run.player.health()));
        // attack_recovery counts steps since the blow; two intervals left to
        // wait reads as one interval before it.
        check("and it has twice its usual recovery to sit out", recovery == -AN.attack_interval,
              "recovery=" + std::to_string(recovery));
    }

    // --- the turn: chased from behind, it comes round, and the chains with it ---
    {
        const auto run_owner = angler_arena(100 - 40);
        Run& run = *run_owner;
        bool sound = true;
        std::string why;
        for (int i = 0; i < 200 && sound; ++i) {
            run.step(Input{});
            sound = pose_is_sound(run.enemies[0], why);
        }
        check("an angler turned round to chase still draws what is hit", sound, why);
        check("and has turned", run.enemies[0].facing_left());
    }

    // --- chain slots ---
    {
        // The chains live in Run's own small pool (Enemy::Chains), so a Run
        // holds only so many anglers at once; past that it refuses one as it
        // refuses a twenty-fifth enemy, and still has room for the rest.
        const auto run_owner = angler_arena(WORLD_W * 4);
        Run& run = *run_owner;
        for (int i = 1; i < Run::MAX_CHAINED; ++i) run.spawn_enemy(100, FLOOR_Y - AN.height, AN);
        bool distinct = true;
        for (int i = 0; i < Run::MAX_CHAINED; ++i)
            for (int j = 0; j < i; ++j)
                distinct = distinct && run.enemies[i].chain_slot() &&
                           run.enemies[i].chain_slot() != run.enemies[j].chain_slot();
        check("every angler in a run has chains of its own",
              run.enemies_alive() == Run::MAX_CHAINED && distinct,
              "alive=" + std::to_string(run.enemies_alive()));
        check("one more than the chain pool holds is refused",
              !run.spawn_enemy(100, FLOOR_Y - AN.height, AN));
        check("but an enemy without tentacles still spawns",
              run.spawn_enemy(40, FLOOR_Y - G.height, G) &&
                  run.enemies[Run::MAX_CHAINED].chain_slot() == nullptr);
        // Killed outright, an angler gives its chains back for the next one.
        Enemy& first = run.enemies[0];
        const Enemy::Chains* freed = first.chain_slot();
        first.shatter(run.grid, first.center_x(), first.center_y(), 200);
        const bool died = !first.is_alive();
        const bool respawned = run.spawn_enemy(100, FLOOR_Y - AN.height, AN);
        check("a dead angler's chains go to the next one spawned",
              died && respawned && run.enemies[0].is_alive() &&
                  run.enemies[0].chain_slot() == freed);
        Enemy bare;
        bare.spawn(100, FLOOR_Y - AN.height, AN);
        check("an angler spawned without chains is left dead", !bare.is_alive());
    }
    {
        // And no angler moves another's tentacles: the first of two bodies
        // walking in one run is posed as one walking alone, step for step.
        const auto alone_owner = angler_arena(WORLD_W * 4);
        const auto pair_owner = angler_arena(WORLD_W * 4);
        Run& alone = *alone_owner;
        Run& pair = *pair_owner;
        pair.spawn_enemy(230, FLOOR_Y - AN.height, AN);
        int differ = -1;
        for (int i = 0; i < 300 && differ < 0; ++i) {
            for (Run* r : {&alone, &pair}) {
                r->grid.update();
                for (Enemy& e : r->enemies)
                    if (e.is_alive()) e.update(r->grid, -10000, -10000, false);
            }
            const Enemy& a = alone.enemies[0];
            const Enemy& b = pair.enemies[0];
            const rig::Box box = a.pose_bounds();
            bool same = a.cell_x() == b.cell_x() && a.cell_y() == b.cell_y();
            for (int y = box.y0; y < box.y1 && same; ++y)
                for (int x = box.x0; x < box.x1 && same; ++x)
                    same = a.posed_pixel(x, y) == b.posed_pixel(x, y);
            if (!same) differ = i;
        }
        check("an angler beside another is posed as it would be alone", differ < 0,
              "first differs at step " + std::to_string(differ));
    }

    // --- cuts ---
    //
    // Two shots through a leg at its middle joint, either side of the chain:
    // everything below is cut off from the heart.
    const auto cut_leg = [&](Enemy& e, Grid& g, const tentacle::Chain& c) {
        const rig::Joint mid = c.rest[c.links() / 2];
        e.shatter(g, world_x(e, mid.x - 1, mid.y), world_y(e, mid.x - 1, mid.y),
                  Quiver::BITE_RADIUS);
        e.shatter(g, world_x(e, mid.x + 2, mid.y), world_y(e, mid.x + 2, mid.y),
                  Quiver::BITE_RADIUS);
    };
    const tentacle::Chain& front_chain = TS.chain[1];
    {
        Grid g = make_world();
        Enemy::Chains chains;
        Enemy e;
        e.spawn(100, FLOOR_Y - AN.height, AN, &chains);
        const int before = e.pixel_count();
        cut_leg(e, g, rear_chain);
        // What is cut off is not sand yet: it lies there as a piece, still
        // drawn and still hit, and the body stands on what it has left.
        check("shot through, a tentacle's piece past the hole comes off whole",
              e.is_alive() && e.has_pixel(rear_tip.x, rear_tip.y) && e.has_feet());
        const int hole = before - e.pixel_count();
        check("only the hole is sand so far", count_sand(g) == hole,
              "sand=" + std::to_string(count_sand(g)) + " hole=" + std::to_string(hole));
        // It wriggles where it lies -- its tip drawn in a different place
        // from one step to the next -- and is a pose as sound as any.
        int moved = 0, last_x = world_x(e, rear_tip.x, rear_tip.y),
            last_y = world_y(e, rear_tip.x, rear_tip.y);
        bool sound = true;
        std::string why;
        for (int i = 0; i < TS.motion.piece_steps / 2 && sound; ++i) {
            settle(g, e, 1);
            sound = pose_is_sound(e, why);
            const int x = world_x(e, rear_tip.x, rear_tip.y),
                      y = world_y(e, rear_tip.x, rear_tip.y);
            if (x != last_x || y != last_y) ++moved;
            last_x = x;
            last_y = y;
        }
        check("the piece wriggles", moved > 5, "moved=" + std::to_string(moved));
        check("every pose with a piece off draws what is hit", sound, why);
        settle(g, e, TS.motion.piece_steps);
        check("then crumbles", e.is_alive() && !e.has_pixel(rear_tip.x, rear_tip.y));
        check("as sand, grain for pixel", count_sand(g) == before - e.pixel_count(),
              "sand=" + std::to_string(count_sand(g)) +
                  " lost=" + std::to_string(before - e.pixel_count()));
    }
    {
        // And the lure, through its stalk just above the head: three shots
        // across it, since the stalk there is wider than a bite -- at rest,
        // where the stalk stands upright. Everything past the cut comes off,
        // bulb and all, and the stub left on the head has nothing to strike
        // with: the bulb is the blow.
        Grid g = make_world();
        Enemy::Chains chains;
        Enemy e;
        e.spawn(100, FLOOR_Y - AN.height, AN, &chains);
        const int before_lure = e.pixel_count();
        const int sand_before = count_sand(g);
        const rig::Joint stalk = lure_chain.rest[2];
        const rig::Joint crown = lure_chain.rest[lure_chain.links() / 2];
        for (const int dx : {-3, 0, 3})
            e.shatter(g, world_x(e, stalk.x + dx, stalk.y), world_y(e, stalk.x + dx, stalk.y),
                      Quiver::BITE_RADIUS);
        check("shots across its stalk take the lure off past the cut, and the strike with it",
              e.is_alive() && !e.has_arms());
        settle(g, e, TS.motion.piece_steps + 1);
        check("bulb and all", !e.has_pixel(crown.x, crown.y) && !e.has_pixel(bulb_x, bulb_y));
        check("as sand, grain for pixel",
              count_sand(g) - sand_before == before_lure - e.pixel_count(),
              "sand=" + std::to_string(count_sand(g) - sand_before) +
                  " lost=" + std::to_string(before_lure - e.pixel_count()));
    }

    // --- one leg: it hops ---
    {
        const auto run_owner = angler_arena(WORLD_W * 4);
        Run& run = *run_owner;
        for (int i = 0; i < 30; ++i) run.step(Input{});
        cut_leg(run.enemies[0], run.grid, rear_chain);
        const int start_x = run.enemies[0].cell_x();
        const int stand_y = run.enemies[0].cell_y();
        int hops = 0, ground_moves = 0;
        bool was_up = false, sound = true;
        std::string why;
        for (int i = 0; i < 300 && sound; ++i) {
            const Enemy& before = run.enemies[0];
            const int x0 = before.cell_x();
            const bool grounded = before.on_ground();
            run.step(Input{});
            const Enemy& e = run.enemies[0];
            sound = pose_is_sound(e, why);
            const bool up = e.cell_y() < stand_y - 2;
            if (up && !was_up) ++hops;
            was_up = up;
            // On the ground the whole step, and moved: walking, not hopping.
            if (grounded && e.on_ground() && e.cell_x() != x0) ++ground_moves;
        }
        const Enemy& e = run.enemies[0];
        check("on one leg an angler still gets about",
              e.is_alive() && e.has_feet() && std::abs(e.cell_x() - start_x) > 30,
              "moved=" + std::to_string(e.cell_x() - start_x));
        check("in hops", hops >= 5, "hops=" + std::to_string(hops));
        check("and never walks along the ground", ground_moves == 0,
              "ground_moves=" + std::to_string(ground_moves));
        check("every hopping pose draws what is hit", sound, why);
    }

    // --- no legs: dragged along on the lure, which no longer strikes ---
    {
        const auto run_owner = angler_arena(100 + AN.width + 4);
        Run& run = *run_owner;
        for (int i = 0; i < 2; ++i) run.step(Input{});
        cut_leg(run.enemies[0], run.grid, rear_chain);
        cut_leg(run.enemies[0], run.grid, front_chain);
        const Enemy& e = run.enemies[0];
        check("with both legs off it lives on, on its lure",
              e.is_alive() && e.has_feet() && !e.has_arms());
        // A body pixel over the waist, which the lean turns about: how far it
        // sits below where it would stand.
        const int mid_row = AN.rig.hip_row - 20;
        const int body_x = AN.rig.waist.x;
        const int start_x = e.cell_x();
        bool sound = true;
        std::string why;
        for (int i = 0; i < 300 && sound; ++i) {
            run.step(Input{});
            sound = pose_is_sound(run.enemies[0], why);
        }
        const int slump = world_y(e, body_x, mid_row) - rest_y(e, mid_row);
        check("slumped down onto the ground", slump >= TS.motion.legless_sink - 3,
              "slump=" + std::to_string(slump));
        check("it drags itself after you", e.cell_x() != start_x,
              "moved=" + std::to_string(e.cell_x() - start_x));
        check("and lands nothing, standing in reach all along",
              run.player.health() == Player::MAX_HEALTH,
              "hp=" + std::to_string(run.player.health()));
        check("every crawling pose draws what is hit", sound, why);
    }

    return report();
}
