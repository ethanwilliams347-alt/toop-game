#include "scene/level_files.h"

#include "scene/bmp.h"

namespace level_files {

Loaded load(const scene_list::SceneDef& def, const std::string& assets_dir,
            bool (*species_ok)(const char*)) {
    Loaded out;
    if (!def.declared_empty()) {
        std::string error, warning;
        out.scene = bmp::load((assets_dir + def.material).c_str(),
                              (assets_dir + def.albedo).c_str(), &error, &warning);
        if (!error.empty()) out.errors.push_back("Failed to load scene: " + error);
        if (!warning.empty()) out.warnings.push_back(warning);
    }
    if (!def.level.empty()) {
        std::string error;
        out.level = level_list::load_level(assets_dir + def.level, species_ok, &error);
        if (!error.empty())
            out.errors.push_back(error + " -- the level falls back to every default");
    }
    return out;
}

} // namespace level_files
