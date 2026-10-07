#include "run.h"

Run::Run(int width, int height, uint64_t seed)
    : grid(width, height, seed)
    , player(width / 2, height / 4)
{
}

void Run::reset(uint64_t seed, int new_width, int new_height) {
    // Both or neither, and only when it is actually a different size -- a resize to
    // the size already held would throw away the allocation for nothing and would
    // also silently drop vent_radius, which a plain wipe keeps.
    if (new_width > 0 && new_height > 0 &&
        (new_width != grid.get_width() || new_height != grid.get_height())) {
        grid = Grid(new_width, new_height, seed);
    } else {
        grid.reset(seed);
    }
    player = Player(grid.get_width() / 2, grid.get_height() / 4);
    dig_tool = DigTool();
    quiver = Quiver();
    enemies.fill(Enemy{});
    kill_count = 0;
    run_outcome = Outcome::Playing;
    // objective_set, goal_x and goal_y are deliberately not cleared -- see the field
    // comment in run.h.
}

bool Run::spawn_enemy(int x, int y, const Species& kind) {
    for (Enemy& e : enemies) {
        if (e.is_alive()) continue;
        // Asked of the slot as the new species: the test is the box, and the box
        // is the species'.
        e.spawn(x, y, kind);
        if (e.overlaps_solid(grid, x, y)) {
            e = Enemy{};
            return false;
        }
        return true;
    }
    return false;
}

int Run::enemies_alive() const {
    int n = 0;
    for (const Enemy& e : enemies)
        if (e.is_alive()) ++n;
    return n;
}

void Run::set_objective(int x, int y) {
    goal_x = x;
    goal_y = y;
    objective_set = true;
}

void Run::clear_objective() {
    objective_set = false;
    goal_x = 0;
    goal_y = 0;
}

bool Run::step(const Input& input) {
    // A filled circle of radius brush_size, painted before physics runs so a freshly
    // placed cell does not move on the same step it was placed.
    //
    // displace rather than set_element: the brush is the one writer that has to move
    // what it lands on instead of deleting it.
    //
    // Bottom row first, and the order is load-bearing for exactly one brush.
    // Displacement lifts the occupant to the first Empty above it and gives up at
    // anything static, so painting top-down with the Wall brush would build a lid
    // over the rest of the disc a row at a time, and every row under it would find
    // no room and be deleted. Bottom-up, each row escapes before the row above it
    // exists. No other brush shows it, because everything else the brush paints is
    // movable and a climb walks straight through it.
    // The spawn first of all, before the brush and the grid, for the brush's own
    // reason: what is put into the world on a step should not also move on it.
    if (input.spawn_enemy) {
        spawn_enemy(input.cursor_x - species::GHOUL.width / 2,
                    input.cursor_y - species::GHOUL.height / 2);
    }
    // A troll stands on the cursor rather than being centred on it: its box is
    // three bodies tall, and centred on a cursor over the ground it would start
    // half buried and be refused.
    if (input.spawn_troll) {
        spawn_enemy(input.cursor_x - species::TROLL.width / 2,
                    input.cursor_y - species::TROLL.height, species::TROLL);
    }

    if (input.brush_active) {
        for (int dy = input.brush_size; dy >= -input.brush_size; --dy) {
            for (int dx = -input.brush_size; dx <= input.brush_size; ++dx) {
                if (dx * dx + dy * dy <= input.brush_size * input.brush_size) {
                    grid.displace(input.cursor_x + dx, input.cursor_y + dy, input.brush_type);
                }
            }
        }
    }

    grid.update();

    // After the grid, so the player collides against the world as it now is rather
    // than as it was a step ago. Player::update() takes its own PlayerInput, built
    // here rather than widening PlayerInput's job, since the brush is a run-level
    // concern the player has no business knowing about.
    PlayerInput player_input;
    player_input.left = input.left;
    player_input.right = input.right;
    player_input.jump = input.jump;
    player_input.aim_x = input.cursor_x;
    player_input.aim_y = input.cursor_y;
    player_input.dig = input.dig;
    player.update(grid, player_input);

    // Last, so the dig is aimed from where the body actually ended up this step.
    // Called every step whether or not the button is held, because that is what
    // advances the tool's cooldown.
    const bool dug = dig_tool.update(grid, input.dig, player.center_x(), player.center_y(),
                                     input.cursor_x, input.cursor_y);

    // The bow, then the arrows, then the bodies. Arrows before enemies so a shot is
    // tested against the enemy where it stood at the start of the step, which is
    // where the player saw it when they let go; the enemy then moves with whatever
    // it has left.
    //
    // Loosed from a little above the body's centre -- the chest the bow is held
    // at, rather than the belt.
    std::array<bool, MAX_ENEMIES> was_alive{};
    for (int i = 0; i < MAX_ENEMIES; ++i) was_alive[i] = enemies[i].is_alive();

    quiver.update_bow(input.shoot, player.center_x(), player.center_y() - Player::HEIGHT / 5,
                      input.cursor_x, input.cursor_y);
    quiver.update_arrows(grid, enemies.data(), MAX_ENEMIES);

    for (Enemy& e : enemies) {
        if (!e.is_alive()) continue;
        if (e.update(grid, player.cell_x(), player.cell_y(), player.is_alive())) {
            player.take_hit(e.species().damage);
        }
    }

    // Counted after both passes, because both can kill: an arrow through the head,
    // or the last of the legs burning away while it walks through a fire. A slot
    // alive before them and dead after them is one kill, whichever did it.
    for (int i = 0; i < MAX_ENEMIES; ++i) {
        if (was_alive[i] && !enemies[i].is_alive()) ++kill_count;
    }

    // --- has the run ended? ---
    //
    // Asked after everything else has moved, against the world and the body as this
    // step left them -- the same reason the player updates after the grid.
    //
    // Death is checked before the objective, so a body that reaches the marker on
    // the step its last health goes has lost rather than won. That is an arbitrary
    // call between two things that cannot both be true, written down here so it is a
    // decision rather than an accident of ordering: being killed by the thing you
    // were escaping is the more legible reading, and the one a player would
    // describe.
    if (run_outcome == Outcome::Playing) {
        if (!player.is_alive()) {
            run_outcome = Outcome::Lost;
        } else if (objective_set) {
            // Distance from the objective to the nearest point of the body's box,
            // clamped per axis -- the standard box/point distance, in integers,
            // squared so there is no root and therefore no float.
            const int bx0 = player.cell_x(), bx1 = bx0 + Player::WIDTH - 1;
            const int by0 = player.cell_y(), by1 = by0 + Player::HEIGHT - 1;
            const int dx = (goal_x < bx0) ? bx0 - goal_x : (goal_x > bx1 ? goal_x - bx1 : 0);
            const int dy = (goal_y < by0) ? by0 - goal_y : (goal_y > by1 ? goal_y - by1 : 0);
            if (dx * dx + dy * dy <= OBJECTIVE_REACH * OBJECTIVE_REACH) {
                run_outcome = Outcome::Won;
            }
        }
    }

    return dug;
}
