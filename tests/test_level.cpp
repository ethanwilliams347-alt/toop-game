// Starting a level: the level-file parser, the scene list's named fields, and
// level::start, which is the one function the game, the replay bench and these
// tests all build a world with.
//
// Runs from the source tree (WORKING_DIRECTORY in CMakeLists.txt), because the
// last section starts every shipped scene from assets/.
#include "game/boot.h"
#include "game/input_log.h"
#include "game/level.h"
#include "scene/level_files.h"
#include "scene/level_list.h"
#include "scene/scene_list.h"
#include "test_util.h"
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

const char* TMP = "test_level_tmp.txt";

level_list::LevelDef parse(const std::string& text, std::string* error) {
    {
        std::ofstream out(TMP);
        out << text;
    }
    if (error) error->clear();
    level_list::LevelDef def = level_list::load_level(TMP, level::is_species, error);
    std::remove(TMP);
    return def;
}

void test_parser() {
    std::string err;
    const level_list::LevelDef ok = parse(
        "# a comment\n"
        "player 80\n"
        "\n"
        "enemy ghoul 260.5   # trailing comment\n"
        "enemy troll 560\n"
        "objective 636\n",
        &err);
    check("level: a well-formed file parses", err.empty(), err);
    check("level: ...with its player column", ok.has_player && ok.player_x == 80.0f);
    check("level: ...its objective", ok.has_objective && ok.objective_x == 636);
    check("level: ...and both enemies, in order, with their lines",
          ok.enemies.size() == 2 && ok.enemies[0].species == "ghoul" &&
              ok.enemies[0].x == 260.5f && ok.enemies[0].line == 4 &&
              ok.enemies[1].species == "troll" && ok.enemies[1].line == 5);

    // Every malformed record rejects the whole file and names its line. A level
    // missing the line it could not read is missing the troll it was built around.
    struct Bad { const char* what; const char* text; int line; };
    const Bad bad[] = {
        {"an unknown species", "enemy ghoul 10\nenemy dragon 40\n", 2},
        {"an authored y", "objective 100 40\n", 1},
        {"a second player line", "player 10\nplayer 20\n", 2},
        {"a second objective line", "objective 10\n\nobjective 20\n", 3},
        {"an unknown record", "chest 40\n", 1},
        {"a column with a unit glued on", "player 80px\n", 1},
        {"a negative column", "enemy ghoul -5\n", 1},
        {"an enemy with no column", "enemy troll\n", 1},
    };
    for (const Bad& b : bad) {
        const level_list::LevelDef d = parse(b.text, &err);
        const std::string where = ":" + std::to_string(b.line) + ":";
        check((std::string("level: ") + b.what + " rejects the file at its line").c_str(),
              !d.has_player && !d.has_objective && d.enemies.empty() &&
                  err.find(where) != std::string::npos,
              err);
    }

    err = "untouched";
    const level_list::LevelDef none =
        level_list::load_level("no_such_level_file.txt", level::is_species, &err);
    check("level: a missing file is every default, not an error",
          !none.has_player && !none.has_objective && none.enemies.empty() && err == "untouched");
}

void test_scene_list_level_field() {
    const char* path = "test_level_scenes_tmp.txt";
    auto load = [&](const std::string& text, std::string* error) {
        {
            std::ofstream out(path);
            out << text;
        }
        if (error) error->clear();
        std::vector<scene_list::SceneDef> s = scene_list::load_scene_list(path, error);
        std::remove(path);
        return s;
    };
    std::string err;
    std::vector<scene_list::SceneDef> s =
        load("a m.bmp a.bmp - terrain fixed 10 10 4 level=a_level.txt\n", &err);
    check("scene list: level= after the positional tail", s.size() == 1 && s[0].level ==
          "a_level.txt" && s[0].custom_width == 10 && s[0].scale == 4, err);
    s = load("a m.bmp a.bmp - terrain level=a_level.txt fixed 10 10\n", &err);
    check("scene list: ...or in the middle of it, which reads the same",
          s.size() == 1 && s[0].level == "a_level.txt" && s[0].custom_width == 10, err);
    s = load("a m.bmp a.bmp - terrain\n", &err);
    check("scene list: no level= is no level file", s.size() == 1 && s[0].level.empty(), err);
    s = load("a m.bmp a.bmp - terrain colour=red\n", &err);
    check("scene list: an unknown key= rejects the file", s.empty() && !err.empty(), err);
    s = load("a m.bmp a.bmp - terrain level=x.txt level=y.txt\n", &err);
    check("scene list: level= twice rejects the file", s.empty() && !err.empty(), err);
    s = load("a m.bmp a.bmp - terrain level=../x.txt\n", &err);
    check("scene list: a level= outside assets/ rejects the file", s.empty() && !err.empty(), err);
}

// A flat floor across a small world, built in memory so the level's placements
// are checkable to the cell.
Scene flat_scene(int w, int h, int floor_top) {
    Scene sc;
    sc.width = w;
    sc.height = h;
    sc.materials.assign(static_cast<size_t>(w) * h, ElementType::Empty);
    sc.albedo.assign(static_cast<size_t>(w) * h, 0xFF808080u);
    for (int y = floor_top; y < h; ++y)
        for (int x = 0; x < w; ++x) sc.materials[static_cast<size_t>(y) * w + x] = ElementType::Wall;
    return sc;
}

scene_list::SceneDef terrain_def() {
    scene_list::SceneDef def;
    def.name = "flat";
    def.material = "flat_material.bmp";  // never read: start() takes the Scene
    def.albedo = "flat_albedo.bmp";
    def.level = "flat_level.txt";
    return def;
}

void test_start() {
    const Scene sc = flat_scene(400, 200, 180);
    const scene_list::SceneDef def = terrain_def();

    {
        Run run(10, 10, 1);
        const level::Report r = level::start(run, def, sc, level_list::LevelDef{}, 7);
        check("start: the world takes the art's size", r.world_w == 400 && r.world_h == 200 &&
              run.grid.get_width() == 400);
        check("start: every cell of the art is stamped", r.scene_cells == 400 * 20);
        check("start: the player stands on the floor", r.player_standing && r.player_feet_row == 180 &&
              run.player.cell_y() + Player::HEIGHT == 180);
        check("start: with no level file the objective goes to the default column",
              r.objective_placed && !r.objective_authored &&
                  run.objective_x() == boot::default_objective_column(400));
        check("start: ...and enemies are planted by the heuristic",
              !r.enemies_authored && r.enemies_placed == run.enemies_alive() && r.enemies_placed > 0);
    }

    level_list::LevelDef lv;
    lv.has_player = true;
    lv.player_x = 50.0f;
    lv.has_objective = true;
    lv.objective_x = 300;
    lv.enemies.push_back(level_list::EnemyDef{"troll", 200.0f, 3});
    lv.enemies.push_back(level_list::EnemyDef{"ghoul", 120.0f, 4});
    lv.enemies.push_back(level_list::EnemyDef{"ghoul", 1000.0f, 5});  // off the world

    Run run(10, 10, 1);
    const level::Report r = level::start(run, def, sc, lv, 7);
    check("start: an authored player column is the body's centre",
          run.player.center_x() == 50 && r.player_standing);
    check("start: an authored objective is planted at its column",
          r.objective_authored && r.objective_placed && run.objective_x() == 300 &&
              run.objective_y() == 180 - Player::HEIGHT / 2);
    check("start: authored enemies are exactly the ones listed",
          r.enemies_authored && r.enemies_placed == 2 && run.enemies_alive() == 2 && r.trolls == 1);
    check("start: ...each standing centred on its column",
          [&] {
              bool troll = false, ghoul = false;
              for (const Enemy& e : run.enemies) {
                  if (!e.is_alive()) continue;
                  const bool on_floor = e.cell_y() + e.species().height == 180;
                  if (&e.species() == &species::TROLL) troll = on_floor && e.center_x() == 200;
                  if (&e.species() == &species::GHOUL) ghoul = on_floor && e.center_x() == 120;
              }
              return troll && ghoul;
          }());
    check("start: one that cannot stand is reported by its line",
          r.enemy_lines_unplaced.size() == 1 && r.enemy_lines_unplaced[0] == 5);
    bool warned = false;
    for (const level::Line& l : level::describe(r, def))
        warned |= l.warning && l.text.find("flat_level.txt:5") != std::string::npos;
    check("start: ...and describe() names the file and line", warned);

    // Restarting is the same call, and must be the same world -- that is the
    // whole claim a recording's start fingerprint relies on.
    Run again(10, 10, 1);
    level::start(again, def, sc, lv, 7);
    check("start: the same inputs build the same world",
          input_log::fingerprint(again) == input_log::fingerprint(run) &&
              again.player.cell_x() == run.player.cell_x() &&
              again.enemies_alive() == run.enemies_alive());
    level::start(again, def, sc, lv, 7);
    check("start: ...including into a run that already held a level",
          input_log::fingerprint(again) == input_log::fingerprint(run) &&
              again.enemies_alive() == run.enemies_alive());
}

// Every scene the game ships, started exactly as the game starts it. The bug this
// pins: one hard-coded objective column wider than every authored scene, so no
// scene could be won and the only sign was a stderr line at launch.
void test_shipped_scenes() {
    std::string error;
    const std::vector<scene_list::SceneDef> scenes =
        scene_list::load_scene_list("assets/scenes.txt", &error);
    check("shipped: the scene list loads", !scenes.empty(), error);
    for (const scene_list::SceneDef& def : scenes) {
        const level_files::Loaded loaded = level_files::load(def, "assets/", level::is_species);
        check(("shipped: '" + def.name + "' loads without errors").c_str(),
              loaded.errors.empty(), loaded.errors.empty() ? "" : loaded.errors.front());
        Run run(10, 10, 1);
        const level::Report r = level::start(run, def, loaded.scene, loaded.level, 1);
        if (def.declared_empty()) continue;
        check(("shipped: '" + def.name + "' has an objective, so it can be won").c_str(),
              r.objective_placed && run.has_objective());
        check(("shipped: '" + def.name + "' stands the player on ground").c_str(),
              r.player_standing);
        check(("shipped: '" + def.name + "' has enemies").c_str(), r.enemies_placed > 0);
        check(("shipped: '" + def.name + "' places every enemy its level lists").c_str(),
              r.enemy_lines_unplaced.empty());
    }
}

}  // namespace

int main() {
    test_parser();
    test_scene_list_level_field();
    test_start();
    test_shipped_scenes();
    return report();
}
