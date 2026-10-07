#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "game/run.h"
#include "scene/level_list.h"
#include "scene/scene.h"
#include "scene/scene_list.h"

// Starting a level: everything between "this scene was chosen" and the first
// step, in one SDL-free call.
//
// Why one call. The sequence -- size the world, reset the run, stamp the
// terrain, stand the player, place the objective, put the enemies down -- is what
// defines the world a session recording starts in. It used to exist only as a
// chain of lambdas inside main(), and every headless tool that needed the same
// world rebuilt its own version of it. They drifted: the replay bench stamped a
// fixture no scene used any more and never planted an enemy, so a session played
// in the game could not be replayed at all, and had it been, it would have
// replayed without the enemies the player was fighting. With one function, the
// game, the bench and the tests cannot disagree about what a level is.
//
// What it leaves out, on purpose: props (render-only, sized by their textures,
// planted by main.cpp after this returns) and the backdrop (render-only). Neither
// can change a single simulated cell, so neither is part of the world a replay
// has to rebuild.
//
// In ENGINE_SOURCES: it only reads the scene and level records, which are plain
// structs, and does not call into the scene loaders. Loading them is
// load_assets() below's job, which lives with the loaders.
namespace level {

// What start() did, for the caller to print and a test to check. Plain data
// rather than printing in here, because the bench and the game want the same
// facts in different places.
struct Report {
    int world_w = 0;
    int world_h = 0;
    bool infinite = false;

    int scene_cells = 0;
    // The scene named a material map, and it placed nothing: a legend that matched
    // no colour, which is a defect, as against a scene declared empty.
    bool scene_placed_nothing = false;

    // Player. On the floor for a floor scene; otherwise on the terrain under its
    // column, or left in mid-air (and warned about) when there is none.
    bool player_on_floor = false;
    bool player_standing = false;
    int player_feet_row = 0;

    // Objective. Not wanted in a scene declared empty, which has nothing to plant
    // it on and is not meant to be won.
    bool objective_wanted = false;
    bool objective_authored = false;
    bool objective_placed = false;
    int objective_x = 0;
    int objective_y = 0;

    // Enemies. Authored when the level file lists any, planted by the spacing
    // heuristic otherwise.
    bool enemies_authored = false;
    int enemies_placed = 0;
    int trolls = 0;
    // Level-file lines whose enemy could not be stood up: no ground under the
    // column, something solid in the way, or the pool full.
    std::vector<int> enemy_lines_unplaced;
};

// Builds the level into `run`. The world's size comes from the scene row or its
// art (scene_activation::resolve); `seed` is the world seed the run is reset to.
Report start(Run& run, const scene_list::SceneDef& def, const Scene& scene,
             const level_list::LevelDef& level, uint64_t seed);

// One line of what start() did, ready to print. `warning` lines go to stderr in
// the game; the rest are the same "Scene: ..., Spawn: ..." lines it always
// printed.
struct Line {
    bool warning = false;
    std::string text;
};
std::vector<Line> describe(const Report& report, const scene_list::SceneDef& def);

// The species check level_list::load_level wants, as a plain function pointer.
bool is_species(const char* name);

} // namespace level
