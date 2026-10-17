#include "scene_list.h"

#include <fstream>
#include <sstream>
#include <string_view>
#include <vector>
#include "util/text_record.h"

namespace scene_list {

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
        if (error) *error = text_record::at(path, line_no, why);
        scenes.clear();
        return scenes;
    };

    // A filename field: `-` means none, anything else must be a safe stem, one dot
    // and an extension. Stricter than text_record::name_ok's dotted form, which
    // takes any number of dots: a scene field names one file, and "a.b.bmp" or a
    // name with no extension is more likely a typo than a file.
    //
    // It may sit in one folder, `<dir>/<file>`, where the folder is a bare name
    // too. That is how a backdrop scene keeps its maps and level beside its layers
    // in assets/<set>/ (the naming rule in assets/scenes.txt). One level and a
    // plain name, so there is still no `..`, no absolute path and no backslash:
    // the field can reach a folder under assets/ and nothing outside it.
    auto file_field = [&](const std::string& raw, std::string& out) -> bool {
        if (raw == "-") { out.clear(); return true; }
        const size_t slash = raw.find('/');
        if (slash != std::string::npos &&
            (raw.find('/', slash + 1) != std::string::npos ||
             !text_record::name_ok(std::string_view(raw).substr(0, slash))))
            return false;
        const size_t stem = slash == std::string::npos ? 0 : slash + 1;
        const size_t dot = raw.rfind('.');
        if (dot == std::string::npos || dot < stem) return false;
        const std::string_view name = raw;
        if (!text_record::name_ok(name.substr(stem, dot - stem))) return false;
        if (!text_record::name_ok(name.substr(dot + 1))) return false;
        out = raw;
        return true;
    };

    while (std::getline(in, line)) {
        ++line_no;

        text_record::strip_comment(line);

        std::istringstream fields(line);
        SceneDef def;
        def.line = line_no;

        if (!(fields >> def.name)) continue;  // blank or comment-only

        if (!text_record::name_ok(def.name))
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
        // Everything after the five required fields is `key=value`, in any order,
        // each key at most once. Named rather than positional because every one of
        // them is optional: a positional tail had to be read in order, so a row that
        // wanted a scale spelled out a mode and a size it did not care about, and
        // `... unclamped 10` could not be told from a width of 10.
        //
        // Optional, and still read. A field the loader ignores is one an author
        // eventually tunes for nothing; an absent field is a different thing, and
        // the two are kept apart by refusing every spelling that is neither.
        bool seen_mode = false, seen_size = false, seen_scale = false;
        auto first_time = [](bool& seen) {
            const bool first = !seen;
            seen = true;
            return first;
        };
        std::string token;
        while (fields >> token) {
            const size_t eq = token.find('=');
            if (eq == std::string::npos) {
                // The positional tail this format had until the fields were named.
                // Refused with the spelling that replaced it, rather than with a
                // bare "unexpected field" that leaves the author to work out why a
                // row that used to load no longer does.
                if (token == "fixed" || token == "unclamped" || token == "infinite" ||
                    (token[0] >= '0' && token[0] <= '9'))
                    return fail("'" + token +
                                "' is a positional field; mode, size and scale are named "
                                "now, as in mode=unclamped size=1920x1080 scale=10");
                return fail("unexpected field '" + token +
                            "' (after the five required fields, every field is key=value)");
            }
            const std::string key = token.substr(0, eq);
            const std::string value = token.substr(eq + 1);
            if (key == "level") {
                if (!def.level.empty())
                    return fail("scene '" + def.name + "' names `level=` twice");
                if (!file_field(value, def.level) || def.level.empty())
                    return fail("'" + value + "' is not a usable level file name");
            } else if (key == "backdrop") {
                // A directory under assets/ holding backdrop.txt and its layers
                // (render/backdrop_set.h). A bare name, so a scene cannot point
                // outside assets/.
                if (!def.backdrop.empty())
                    return fail("scene '" + def.name + "' names `backdrop=` twice");
                if (!text_record::name_ok(value))
                    return fail("'" + value +
                                "' is not a usable backdrop directory "
                                "(a bare name under assets/)");
                def.backdrop = value;
            } else if (key == "mode") {
                if (!first_time(seen_mode))
                    return fail("scene '" + def.name + "' names `mode=` twice");
                if (value == "fixed")
                    def.mode = SceneMode::Fixed;
                else if (value == "unclamped")
                    def.mode = SceneMode::Unclamped;
                else if (value == "infinite")
                    return fail("`infinite` is now `unclamped`: the mode only lets the camera "
                                "scroll past the world's side edges, and the world stays finite");
                else
                    return fail("'" + value + "' is not a scene mode (fixed or unclamped)");
            } else if (key == "size") {
                // One field for both sides, so a size is a pair or it is absent.
                // Half a size is not a size, and accepting one gives a world as wide
                // as the author said and as tall as the engine guessed.
                //
                // The six-digit cap is the one the hand-written reader this replaced
                // had. Nothing downstream bounds a world, so a stray extra digit or
                // two is refused here rather than allocated.
                if (!first_time(seen_size))
                    return fail("scene '" + def.name + "' names `size=` twice");
                const std::string_view v = value;
                const size_t x = v.find('x');
                int w = 0, h = 0;
                if (x == std::string_view::npos || !text_record::whole(v.substr(0, x), w) ||
                    !text_record::whole(v.substr(x + 1), h) || w <= 0 || h <= 0 || w > 999999 ||
                    h > 999999)
                    return fail("size='" + value +
                                "' is not a usable world size (WIDTHxHEIGHT in cells, like "
                                "1920x1080)");
                def.custom_width = w;
                def.custom_height = h;
            } else if (key == "scale") {
                // The cap is 64 rather than unbounded because this multiplies into
                // every screen coordinate in the renderer, and a typo of 1000 is a
                // window-sized cell rather than a diagnosable error.
                if (!first_time(seen_scale))
                    return fail("scene '" + def.name + "' names `scale=` twice");
                int scale = 0;
                if (!text_record::whole(value, scale) || scale < 1 || scale > 64)
                    return fail("scale='" + value +
                                "' is not a usable screen scale (1 to 64 pixels per cell)");
                def.scale = scale;
            } else {
                return fail("'" + key +
                            "=' is not a scene field (mode, size, scale, level, backdrop)");
            }
        }

        // A file in a folder is only ever in the scene's own backdrop folder.
        // Not any folder: assets/wip/ holds drafts and is deliberately not staged
        // next to the exe (CMakeLists.txt), so a row reaching into it would load
        // from the source tree and fail in the build.
        for (const std::string* file : {&def.material, &def.albedo, &def.props, &def.level}) {
            const size_t slash = file->find('/');
            if (slash == std::string::npos) continue;
            if (def.backdrop.empty() || std::string_view(*file).substr(0, slash) != def.backdrop)
                return fail("'" + *file +
                            "' is in a folder, and a scene's files may only sit in its "
                            "own backdrop= folder" +
                            (def.backdrop.empty() ? std::string(" (this scene names none)")
                                                  : " ('" + def.backdrop + "/')"));
        }

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
