#include "level_list.h"

#include <fstream>
#include <sstream>

namespace level_list {

LevelDef load_level(const std::string& path, bool (*species_ok)(const char*),
                    std::string* error) {
    LevelDef level;
    std::ifstream in(path);
    if (!in) return level;  // absent: every default applies, as with props

    std::string line;
    int line_no = 0;
    int player_line = 0, objective_line = 0;

    auto fail = [&](const std::string& why) {
        if (error) {
            std::ostringstream msg;
            msg << path << ":" << line_no << ": " << why;
            *error = msg.str();
        }
        return LevelDef{};
    };

    // A column is a number and nothing else. Read with >> into a float, then
    // refused if anything is glued to it ("120px"), which >> would otherwise stop
    // at and leave for the extra-field check to misreport.
    auto column = [](std::istringstream& fields, float& out) -> bool {
        std::string raw;
        if (!(fields >> raw)) return false;
        std::istringstream num(raw);
        float v = 0.0f;
        char trailing = 0;
        if (!(num >> v) || (num >> trailing) || v < 0.0f) return false;
        out = v;
        return true;
    };

    while (std::getline(in, line)) {
        ++line_no;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        std::istringstream fields(line);
        std::string kind;
        if (!(fields >> kind)) continue;  // blank or comment-only

        if (kind == "player") {
            // Twice is two authors who disagree, and taking either one is a
            // level that silently ignores the other.
            if (player_line)
                return fail("a second `player` line; the first is on line " +
                            std::to_string(player_line));
            if (!column(fields, level.player_x))
                return fail("`player` needs one column, a non-negative number");
            level.has_player = true;
            player_line = line_no;
        } else if (kind == "objective") {
            if (objective_line)
                return fail("a second `objective` line; the first is on line " +
                            std::to_string(objective_line));
            float x = 0.0f;
            if (!column(fields, x))
                return fail("`objective` needs one column, a non-negative number");
            level.has_objective = true;
            level.objective_x = static_cast<int>(x);
            objective_line = line_no;
        } else if (kind == "enemy") {
            EnemyDef e;
            e.line = line_no;
            if (!(fields >> e.species))
                return fail("`enemy` needs a species and a column");
            if (!species_ok || !species_ok(e.species.c_str()))
                return fail("'" + e.species + "' is not a species");
            if (!column(fields, e.x))
                return fail("`enemy " + e.species + "` needs a column, a non-negative number");
            level.enemies.push_back(e);
        } else {
            return fail("'" + kind + "' is not a record (player, objective or enemy)");
        }

        std::string extra;
        if (fields >> extra)
            return fail("unexpected '" + extra + "' (nothing in a level has an authored y; "
                        "it stands on the terrain under its column)");
    }
    return level;
}

} // namespace level_list
