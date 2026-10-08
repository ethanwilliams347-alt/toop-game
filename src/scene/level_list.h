#pragma once
#include <string>
#include <vector>

// What a level puts into its world once the terrain is stamped: where the player
// starts, where the objective is, and which enemies stand where.
//
// Its own file rather than more columns on the scene list, for the props file's
// reason: a scene row is one record, and a level is a handful of them. The scene
// list names this file with `level=<file>`.
//
// Before this file existed, all three were code. The objective was one column
// for every scene (boot::OBJECTIVE_X = 1700, wider than every authored scene, so
// no scene could be won), and enemies were planted by a spacing heuristic that no
// one could aim. A level that is going to be designed -- a troll at the bottom of
// a pit the player has to bring down on it, ghouls on a bridge that burns -- needs
// its contents written down.
//
// Like props, nothing has an authored y. Every record is a column, and the thing
// is stood on the terrain actually under it when the level starts
// (game/level.cpp). A y in the file would be a number the loader ignores once
// someone digs the ground away, and a number the loader ignores is one an author
// eventually spends an afternoon tuning.
//
// Format, one record per line, `#` to end-of-line is a comment:
//
//     player     120         # the body's centre column; at most once
//     objective  600         # the objective's column; at most once
//     enemy      ghoul  300  # species name, then its centre column
//     enemy      troll  520
//
// Every record is optional. A file with no `player` line spawns the player in the
// middle of the world, as before; one with no `objective` line puts it at the far
// end (boot::default_objective_column); one with no `enemy` line plants enemies
// by the spacing heuristic (boot::plant_enemies), so a scene with no level file
// plays as it always has. A file that lists enemies gets exactly those.
//
// All-or-nothing, like every parser here: on the first malformed record the
// whole level comes back empty and `error` says which line. A level that dropped
// the line it could not read would be missing the troll it was designed around,
// and would say nothing.
namespace level_list {

struct EnemyDef {
    std::string species;  // a species::find name; checked by the loader
    float x = 0.0f;       // world cell, the body's centre column
    int line = 0;         // 1-based source line, for diagnostics
};

struct LevelDef {
    bool has_player = false;
    float player_x = 0.0f;  // world cell, the body's centre column
    bool has_objective = false;
    int objective_x = 0;    // world cell
    std::vector<EnemyDef> enemies;
};

// Parses a level file. On any malformed record the level comes back empty and
// `error` (if non-null) names the line. A missing file is not an error and
// yields an empty level -- the defaults above -- for the same reason a missing
// prop list is not.
//
// `species_ok` decides whether a name is a species. It is a parameter rather
// than a lookup here because src/scene/ is linked by targets that do not link the
// simulation; the caller passes species::find.
LevelDef load_level(const std::string& path, bool (*species_ok)(const char*),
                    std::string* error = nullptr);

} // namespace level_list
