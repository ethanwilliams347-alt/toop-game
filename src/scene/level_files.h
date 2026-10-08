#pragma once
#include <string>
#include <vector>
#include "scene/level_list.h"
#include "scene/scene.h"
#include "scene/scene_list.h"

// Reads everything a scene row names that the simulation needs -- the material
// and albedo maps and the level file -- from `assets_dir`.
//
// One function for the game and the replay bench, so the two cannot load a scene
// differently. What it returns goes straight to level::start(). Props and the
// backdrop are not here: they are render-only and loaded by the shell.
namespace level_files {

struct Loaded {
    Scene scene;
    level_list::LevelDef level;
    // A failure leaves the part that failed empty and says so here: an unreadable
    // map is an empty world, an unreadable level file is every default. The game
    // prints these and plays on, degraded and loud, as with every other file.
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

Loaded load(const scene_list::SceneDef& def, const std::string& assets_dir,
            bool (*species_ok)(const char*));

} // namespace level_files
