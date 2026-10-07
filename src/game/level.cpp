#include "game/level.h"

#include <cmath>
#include "game/boot.h"
#include "game/scene_activation.h"

namespace level {

bool is_species(const char* name) { return species::find(name) != nullptr; }

Report start(Run& run, const scene_list::SceneDef& def, const Scene& scene,
             const level_list::LevelDef& level, uint64_t seed) {
    Report r;

    // The scale argument only decides Resolved::scale_changed, which is the
    // renderer's business; the world's size does not depend on it.
    const scene_activation::Resolved resolved = scene_activation::resolve(
        def, scene.width, scene.height, boot::GRID_WIDTH, boot::GRID_HEIGHT, def.scale);
    r.world_w = resolved.world_w;
    r.world_h = resolved.world_h;
    r.infinite = resolved.infinite;

    run.reset(seed, r.world_w, r.world_h);

    if (!def.declared_empty() && scene.width > 0) r.scene_cells = load_scene(run.grid, scene, 0, 0);
    r.scene_placed_nothing = !def.declared_empty() && r.scene_cells == 0;

    // --- the player ---
    //
    // An authored column is the body's centre, like a prop's x, so the file does
    // not have to know the box is eight cells wide.
    const int player_left =
        level.has_player ? static_cast<int>(std::lround(level.player_x)) - Player::WIDTH / 2 : -1;
    if (def.spawn == scene_list::Spawn::Floor) {
        boot::stand_player_on_floor(run, player_left);
        r.player_on_floor = true;
    } else {
        if (player_left >= 0) run.player = Player(player_left, run.grid.get_height() / 4);
        const boot::Standing s = boot::stand_player_on_ground(run);
        r.player_standing = s.placed;
        r.player_feet_row = s.surface;
    }

    // --- the objective ---
    run.clear_objective();
    r.objective_wanted = !def.declared_empty();
    r.objective_authored = level.has_objective;
    r.objective_x =
        level.has_objective ? level.objective_x : boot::default_objective_column(r.world_w);
    if (r.objective_wanted) {
        const boot::Objective obj = boot::place_objective(run, r.objective_x);
        r.objective_placed = obj.placed;
        r.objective_y = obj.y;
    }

    // --- the enemies ---
    //
    // A level that lists enemies gets exactly those and no heuristic ones on top:
    // an author who placed one troll in a pit did not also ask for six ghouls
    // spread across the map.
    if (!level.enemies.empty()) {
        r.enemies_authored = true;
        for (const level_list::EnemyDef& e : level.enemies) {
            const Species* kind = species::find(e.species.c_str());
            if (!kind) {  // load_level refuses this; checked again for a hand-built LevelDef
                r.enemy_lines_unplaced.push_back(e.line);
                continue;
            }
            const int left = static_cast<int>(std::lround(e.x)) - kind->width / 2;
            const int y = boot::standing_y(run.grid, left, *kind);
            if (y < 0 || !run.spawn_enemy(left, y, *kind)) {
                r.enemy_lines_unplaced.push_back(e.line);
                continue;
            }
            ++r.enemies_placed;
            if (kind == &species::TROLL) ++r.trolls;
        }
    } else {
        const boot::EnemyPlanting planted = boot::plant_enemies(run);
        r.enemies_placed = planted.placed;
        r.trolls = planted.trolls;
    }
    return r;
}

std::vector<Line> describe(const Report& r, const scene_list::SceneDef& def) {
    std::vector<Line> out;
    auto info = [&](const std::string& t) { out.push_back(Line{false, t}); };
    auto warn = [&](const std::string& t) { out.push_back(Line{true, t}); };

    info("Scene: " + def.name + ", " + std::to_string(r.world_w) + "x" + std::to_string(r.world_h) +
         ", " + std::to_string(r.scene_cells) + " cells placed");
    if (r.scene_placed_nothing) warn("the scene named no material anywhere - the world is empty.");

    if (!r.player_on_floor) {
        if (r.player_standing)
            info("Spawn: standing, feet on row " + std::to_string(r.player_feet_row));
        else
            warn("no ground under the spawn column; the player starts in mid-air and will fall.");
    }

    if (r.objective_wanted) {
        if (r.objective_placed) {
            info("Objective: (" + std::to_string(r.objective_x) + ", " +
                 std::to_string(r.objective_y) + ")" +
                 (r.objective_authored ? "" : " (default column)"));
        } else if (r.objective_x < 0 || r.objective_x >= r.world_w) {
            warn("scene '" + def.name + "' is " + std::to_string(r.world_w) +
                 " cells wide and its objective column is x=" + std::to_string(r.objective_x) +
                 ", so this run has no objective and cannot be won.");
        } else {
            warn("no ground under the objective column x=" + std::to_string(r.objective_x) +
                 "; this run has no objective and cannot be won.");
        }
    }

    info("Enemies: " + std::to_string(r.enemies_placed) + " placed (" + std::to_string(r.trolls) +
         " trolls)" + (r.enemies_authored ? ", from the level file" : ""));
    for (int line : r.enemy_lines_unplaced)
        warn("assets/" + def.level + ":" + std::to_string(line) +
             ": the enemy there has no room to stand and was not placed.");
    return out;
}

} // namespace level
