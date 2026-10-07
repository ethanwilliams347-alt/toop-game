#include "scene_list.h"

#include <fstream>
#include <sstream>
#include <vector>

namespace scene_list {

bool scene_name_ok(const std::string& name) {
    if (name.empty()) return false;
    if (name.find('/') != std::string::npos) return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find("..") != std::string::npos) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// The fallback main.cpp uses when assets/scenes.txt is missing or malformed, so
// it is the guarantee that the game boots at all. It names no files, which is
// what makes it unable to fail to find one -- a fallback that depends on assets
// shipping is not a fallback.
//
// The body lands on the world's bottom border. That is not Spawn::Floor being
// used as a recovery path -- the refusal beside that enumerator forbids exactly
// that -- because this scene is meant to be empty rather than having failed a
// terrain scan.
//
// The cost, stated rather than hidden: the fallback world has no terrain in it.
// That is the honest reading of a missing file, and preferable to booting into a
// scene whose assets may not exist.
std::vector<SceneDef> default_scene_list() {
    SceneDef empty;
    empty.name = "empty";
    empty.material = "";
    empty.albedo = "";
    empty.props = "";
    empty.spawn = Spawn::Floor;
    empty.line = 0;  // 0 rather than 1: this record came from no line at all
    return { empty };
}

std::vector<SceneDef> load_scene_list(const std::string& path, std::string* error) {
    std::vector<SceneDef> scenes;

    std::ifstream in(path);
    // Absent is not malformed, exactly as in load_prop_list. The caller falls back
    // to default_scene_list(), so deleting this file cannot stop the game booting --
    // it only removes the choice.
    if (!in) return scenes;

    std::string line;
    int line_no = 0;

    auto fail = [&](const std::string& why) {
        if (error) {
            std::ostringstream msg;
            msg << path << ":" << line_no << ": " << why;
            *error = msg.str();
        }
        scenes.clear();
        return scenes;
    };

    // A filename field: `-` means none, anything else must be a safe name plus an
    // extension. That is why this is not scene_name_ok directly -- a prop sprite is
    // a stem and these are whole filenames, so the dot is allowed here and nowhere
    // else.
    auto file_field = [&](const std::string& raw, std::string& out) -> bool {
        if (raw == "-") { out.clear(); return true; }
        const size_t dot = raw.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= raw.size()) return false;
        if (!scene_name_ok(raw.substr(0, dot))) return false;
        if (!scene_name_ok(raw.substr(dot + 1))) return false;
        out = raw;
        return true;
    };

    while (std::getline(in, line)) {
        ++line_no;

        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        std::istringstream fields(line);
        SceneDef def;
        def.line = line_no;

        if (!(fields >> def.name)) continue;  // blank or comment-only

        if (!scene_name_ok(def.name))
            return fail("'" + def.name + "' is not a usable scene name "
                        "(letters, digits, _ and - only; no path separators)");

        for (const SceneDef& seen : scenes)
            if (seen.name == def.name)
                return fail("scene '" + def.name + "' is already defined on line " +
                            std::to_string(seen.line));

        std::string material, albedo, props, spawn;
        if (!(fields >> material >> albedo >> props >> spawn))
            return fail("scene '" + def.name +
                        "' needs five fields: name material albedo props spawn "
                        "(use - for none)");

        if (!file_field(material, def.material))
            return fail("'" + material + "' is not a usable file name");
        if (!file_field(albedo, def.albedo))
            return fail("'" + albedo + "' is not a usable file name");
        if (!file_field(props, def.props))
            return fail("'" + props + "' is not a usable file name");

        // A material map without an albedo map, or the reverse, is refused rather
        // than half-loaded. bmp::load takes both and a scene is the pair; naming
        // one of them is an author who meant something the loader cannot do, and
        // the failure without this check is a blank world.
        if (def.material.empty() != def.albedo.empty())
            return fail("scene '" + def.name +
                        "' names one of material/albedo and not the other; "
                        "a scene is the pair, or neither");

        if (spawn == "terrain") def.spawn = Spawn::Terrain;
        else if (spawn == "floor") def.spawn = Spawn::Floor;
        else return fail("'" + spawn + "' is not a spawn rule (terrain or floor)");

        // An empty scene may not ask for a terrain spawn: there is no terrain to
        // scan, and the body would be left in mid-air part-way down the world.
        if (def.declared_empty() && def.spawn != Spawn::Floor)
            return fail("scene '" + def.name +
                        "' names no material map, so it has no terrain to stand on; "
                        "its spawn must be floor");

        // --- named fields ----------------------------------------------------
        //
        // `key=value`, anywhere after the five required fields. Named rather than
        // positional because the positional tail is already three optional fields
        // deep, and a fourth would make every row that wants it spell out a mode,
        // a size and a scale it does not care about. Pulled out first so the
        // positional reading below sees exactly what it always did.
        {
            std::string rest, token;
            std::vector<std::string> positional;
            while (fields >> token) {
                const size_t eq = token.find('=');
                if (eq == std::string::npos) {
                    positional.push_back(token);
                    continue;
                }
                const std::string key = token.substr(0, eq);
                const std::string value = token.substr(eq + 1);
                if (key == "level") {
                    if (!def.level.empty())
                        return fail("scene '" + def.name + "' names `level=` twice");
                    if (!file_field(value, def.level) || def.level.empty())
                        return fail("'" + value + "' is not a usable level file name");
                } else if (key == "backdrop") {
                    // A directory under assets/ holding backdrop.txt and its
                    // layers (render/backdrop_set.h). A bare name, so a scene
                    // cannot point outside assets/.
                    if (!def.backdrop.empty())
                        return fail("scene '" + def.name + "' names `backdrop=` twice");
                    if (value.empty() || !scene_name_ok(value))
                        return fail("'" + value +
                                    "' is not a usable backdrop directory "
                                    "(a bare name under assets/)");
                    def.backdrop = value;
                } else {
                    return fail("'" + key + "=' is not a scene field (level, backdrop)");
                }
            }
            for (const std::string& p : positional) rest += p + " ";
            fields = std::istringstream(rest);
        }

        // --- the optional trailing fields ------------------------------------
        //
        // Optional, and still read. A field the loader ignores is one an author
        // eventually tunes for nothing; an absent field is a different thing, and
        // the two are kept apart by refusing every spelling that is neither. A
        // sixth token that is not a mode is an error, and a seventh that arrives
        // without an eighth is an error, rather than either being skipped back to
        // the default.
        std::string mode;
        if (fields >> mode) {
            if (mode == "fixed") def.mode = SceneMode::Fixed;
            else if (mode == "infinite") def.mode = SceneMode::Infinite;
            else return fail("'" + mode + "' is not a scene mode (fixed or infinite)");

            // The size is a pair or it is absent. Half a size is not a size, and
            // accepting one gives a world as wide as the author said and as tall
            // as the engine guessed.
            std::string w_raw, h_raw;
            if (fields >> w_raw) {
                if (!(fields >> h_raw))
                    return fail("scene '" + def.name +
                                "' gives a width with no height; state both or neither");

                // Accumulated by hand rather than through std::stoi, because the build
                // compiles with exceptions off and stoi's failure mode is a throw. Six
                // digits is the cap, which cannot overflow an int.
                auto positive_int = [](const std::string& raw, int& out) -> bool {
                    if (raw.empty() || raw.size() > 6) return false;
                    int v = 0;
                    for (char c : raw) {
                        if (c < '0' || c > '9') return false;
                        v = v * 10 + (c - '0');
                    }
                    if (v <= 0) return false;
                    out = v;
                    return true;
                };

                if (!positive_int(w_raw, def.custom_width))
                    return fail("'" + w_raw + "' is not a usable world width");
                if (!positive_int(h_raw, def.custom_height))
                    return fail("'" + h_raw + "' is not a usable world height");

                // Nested inside the size on purpose. These fields are positional, so a
                // row reading `... infinite 10` cannot be told from one meaning a width
                // of 10; the only unambiguous place a scale can sit is after a size that
                // is already stated. A row wanting a scale therefore states its size
                // too, which every row that would want one already does.
                //
                // The cap is 64 rather than unbounded because this multiplies into every
                // screen coordinate in the renderer, and a typo of 1000 is a
                // window-sized cell rather than a diagnosable error.
                std::string scale_raw;
                if (fields >> scale_raw) {
                    if (!positive_int(scale_raw, def.scale) || def.scale > 64)
                        return fail("'" + scale_raw +
                                    "' is not a usable screen scale (1 to 64 pixels per cell)");
                }
            }
        }

        std::string extra;
        if (fields >> extra)
            return fail("unexpected extra field '" + extra + "'");

        scenes.push_back(def);
    }

    // A file that exists and lists nothing is an author who meant something.
    // Returning an empty list here would silently fall the caller back to the
    // built-in default, which is a different world than the one the file was edited
    // to produce.
    if (scenes.empty()) {
        line_no = 0;
        return fail("the file exists but lists no scenes");
    }

    return scenes;
}

} // namespace scene_list
