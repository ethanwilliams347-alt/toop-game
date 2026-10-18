// The scene list format.
//
// Most of this file asserts that a malformed list is rejected wholesale. A suite
// written the obvious way -- assert the good rows survived -- passes on the
// exact bug the all-or-nothing rule exists to catch. The rule is
// load_prop_list's; what is tested here is that this loader actually keeps it,
// plus the three refusals that are this format's own: a name given twice, a
// scene that names half a scene, and an empty scene asking for a spawn it has no
// terrain for.

#include "scene/scene_list.h"
#include "game/camera.h"
#include "test_util.h"
#include "util/text_record.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using scene_list::SceneDef;
using scene_list::Spawn;
using scene_list::load_scene_list;

namespace {

const char* TMP = "test_scene_list.tmp.txt";

void write(const std::string& body) {
    std::ofstream out(TMP, std::ios::binary);
    out << body;
}

// Parse `body` and report whether it was rejected, and with what message.
struct Result {
    std::vector<SceneDef> scenes;
    std::string error;
    bool rejected() const { return scenes.empty() && !error.empty(); }
};

Result parse(const std::string& body) {
    write(body);
    Result r;
    r.scenes = load_scene_list(TMP, &r.error);
    return r;
}

constexpr const char* GOOD =
    "# name    material           albedo           props           spawn\n"
    "fixture   test_material.bmp  test_albedo.bmp  test_props.txt  terrain\n"
    "empty     -                  -                -               floor\n";

} // namespace

int main() {
    // --- the happy path -----------------------------------------------------
    {
        const Result r = parse(GOOD);
        check("a well-formed list parses", r.scenes.size() == 2 && r.error.empty(),
              std::to_string(r.scenes.size()) + " scenes, error '" + r.error + "'");
        if (r.scenes.size() == 2) {
            check("the fixture row keeps its three files",
                  r.scenes[0].name == "fixture" &&
                  r.scenes[0].material == "test_material.bmp" &&
                  r.scenes[0].albedo == "test_albedo.bmp" &&
                  r.scenes[0].props == "test_props.txt");
            check("and its spawn rule", r.scenes[0].spawn == Spawn::Terrain);
            check("the empty row names no files",
                  r.scenes[1].material.empty() && r.scenes[1].albedo.empty() &&
                  r.scenes[1].props.empty());
            check("and is declared empty rather than merely resolving to nothing",
                  r.scenes[1].declared_empty() && !r.scenes[0].declared_empty());
            check("and spawns on the floor", r.scenes[1].spawn == Spawn::Floor);
            check("line numbers are 1-based and point at the record",
                  r.scenes[0].line == 2 && r.scenes[1].line == 3,
                  std::to_string(r.scenes[0].line) + ", " + std::to_string(r.scenes[1].line));
        }
    }

    // Comments, blank lines and trailing annotations.
    {
        const Result r = parse("\n\n# only a comment\n"
                               "empty - - - floor   # the backdrop on its own\n\n");
        check("comments and blank lines are not records",
              r.scenes.size() == 1 && r.error.empty(),
              std::to_string(r.scenes.size()));
    }

    // --- absent is not malformed -------------------------------------------
    {
        std::string error = "untouched";
        const std::vector<SceneDef> s = load_scene_list("no_such_scene_list.txt", &error);
        check("a missing file yields an empty list and no error",
              s.empty() && error == "untouched", error);
    }

    // --- and a file that exists and says nothing IS malformed ---------------
    //
    // The two cases above and below look identical from the return value alone,
    // which is why the loader separates them: falling back to the built-in default
    // because a file was deleted is right, and falling back because the file was
    // edited down to comments is a different world than the author meant.
    {
        const Result r = parse("# everything here is a comment\n\n");
        check("a file that exists but lists nothing is rejected", r.rejected(), r.error);
    }

    // --- rejection is wholesale --------------------------------------------
    {
        const Result r = parse(std::string(GOOD) + "broken   test_material.bmp\n");
        check("a short row costs the whole list, not just itself", r.rejected(), r.error);
        check("and the message names the line", r.error.find(":4:") != std::string::npos,
              r.error);
    }
    {
        const Result r = parse(std::string(GOOD) + "extra - - - floor sixth\n");
        check("a sixth field that is not key=value is an error, not something to ignore",
              r.rejected(), r.error);
    }

    // --- the named fields --------------------------------------------------
    //
    // The thing being tested here is that "optional" did not become "tolerant". The
    // required fields are all-or-nothing and the named ones are absent-or-exact;
    // a parser that fell back to the default on an unreadable mode or a lone width
    // would produce a world of the wrong size that loads and says nothing.
    {
        const Result r = parse("plain - - - floor\n");
        check("a row that names no mode is fixed",
              r.scenes.size() == 1 && !r.scenes[0].is_unclamped(), r.error);
        check("and states no size, rather than inventing one",
              r.scenes.size() == 1 && r.scenes[0].custom_width == 0 &&
              r.scenes[0].custom_height == 0);
    }
    {
        const Result r = parse("far - - - floor mode=unclamped\n");
        check("a mode with no size parses",
              r.scenes.size() == 1 && r.scenes[0].is_unclamped() && r.scenes[0].custom_width == 0,
              r.error);
    }
    {
        const Result r = parse("far - - - floor mode=fixed\n");
        check("mode=fixed is the default stated",
              r.scenes.size() == 1 && !r.scenes[0].is_unclamped(), r.error);
    }
    {
        // The old spelling is refused rather than accepted as a synonym, with a
        // message that says what it became: the mode never made a world infinite.
        const Result r = parse("far - - - floor mode=infinite\n");
        check("the old `infinite` spelling is refused, naming its replacement",
              r.rejected() && r.error.find("unclamped") != std::string::npos, r.error);
    }
    {
        const Result r =
            parse("big test_material.bmp test_albedo.bmp - terrain mode=fixed size=3840x1440\n");
        check("a fixed row carries its own world size",
              r.scenes.size() == 1 && !r.scenes[0].is_unclamped() &&
                  r.scenes[0].custom_width == 3840 && r.scenes[0].custom_height == 1440,
              r.error);
    }
    {
        const Result r = parse("any - - - floor scale=10 size=344x144 mode=unclamped\n");
        check("named fields read the same in any order",
              r.scenes.size() == 1 && r.scenes[0].is_unclamped() &&
                  r.scenes[0].custom_width == 344 && r.scenes[0].custom_height == 144 &&
                  r.scenes[0].scale == 10,
              r.error);
    }
    {
        const Result r = parse("scaleonly - - - floor scale=10\n");
        check("a scale needs no size now that it is named",
              r.scenes.size() == 1 && r.scenes[0].scale == 10 && r.scenes[0].custom_width == 0,
              r.error);
    }
    {
        // Each one is half a size, a size of no cells, or a size that is not a
        // number. Every one is refused, never read as part of a size.
        const char* bad[] = {"3840",      "3840x",    "x1440",   "0x100",   "100x0",      "-8x100",
                             "widextall", "10x10x10", "1e3x100", "+10x100", "1000000x100"};
        for (const char* size : bad) {
            const Result r = parse(std::string("bad - - - floor size=") + size + "\n");
            check((std::string("size= refuses '") + size + "'").c_str(),
                  r.rejected() && r.error.find("not a usable world size") != std::string::npos,
                  r.error);
        }
    }
    {
        const Result r = parse("odd - - - floor mode=sideways\n");
        check("an unknown mode is refused rather than defaulted", r.rejected(), r.error);
    }
    {
        const Result r = parse("twice - - - floor mode=fixed mode=unclamped\n");
        check("a named field given twice is refused", r.rejected(), r.error);
    }
    {
        const Result r = parse("twice - - - floor size=10x10 size=20x20\n");
        check("...size= too", r.rejected(), r.error);
    }
    {
        const Result r = parse("twice - - - floor scale=4 scale=4\n");
        check("...and scale=, even agreeing with itself", r.rejected(), r.error);
    }
    {
        const Result r = parse("colour - - - floor tint=red\n");
        check("an unknown key= is refused",
              r.rejected() && r.error.find("tint=") != std::string::npos, r.error);
    }

    // --- the positional tail, which this format no longer has ---------------
    //
    // A row written before the fields were named still has its mode, size and
    // scale in it, and would mean something to whoever wrote it. It costs the whole
    // file, like any other malformed row, and the message gives the new spelling so
    // fixing it does not need the header.
    {
        const Result r = parse(std::string(GOOD) + "old - - - floor unclamped 1920 1080\n");
        check("an old positional mode rejects the whole file", r.rejected(), r.error);
        check("...at its line", r.error.find(":4:") != std::string::npos, r.error);
        check("...naming the field and its named spelling",
              r.error.find("'unclamped' is a positional field") != std::string::npos &&
                  r.error.find("mode=unclamped size=1920x1080 scale=10") != std::string::npos,
              r.error);
    }
    {
        const Result r = parse("old test_material.bmp test_albedo.bmp - terrain fixed 344 144 10 "
                               "backdrop=bg_valley\n");
        check("so does a fixed one with a size, a scale and a named field after them",
              r.rejected() && r.error.find("positional") != std::string::npos, r.error);
    }
    {
        const Result r = parse("old - - - floor mode=fixed 344 144\n");
        check("...and a bare size after a named mode",
              r.rejected() && r.error.find("'344' is a positional field") != std::string::npos,
              r.error);
    }

    // --- the scale ----------------------------------------------------
    //
    // The same absent-or-exact rule as the fields above, plus one check that the
    // default the header states really is the camera's: SceneDef spells the number
    // itself rather than including src/game/camera.h, and that duplication is only
    // safe while something pins the two together.
    {
        const Result r = parse("noscale - - - floor size=100x100\n");
        check("a row that states no scale gets the engine default",
              r.scenes.size() == 1 && r.scenes[0].scale == Camera::DEFAULT_SCALE, r.error);
    }
    {
        const Result r = parse("scaled - - - floor size=344x144 scale=10\n");
        check("a stated scale is read",
              r.scenes.size() == 1 && r.scenes[0].scale == 10, r.error);
    }
    {
        const Result r = parse("zeroscale - - - floor scale=0\n");
        check("a scale of zero is refused, not defaulted", r.rejected(), r.error);
    }
    {
        const Result r = parse("bigscale - - - floor scale=65\n");
        check("a scale past the cap is refused", r.rejected(), r.error);
    }
    {
        const Result r = parse("wordscale - - - floor scale=huge\n");
        check("a scale that is not a number is refused", r.rejected(), r.error);
    }
    {
        const Result r = parse("unitscale - - - floor scale=10px\n");
        check("a scale with a unit on it is refused, not read as 10", r.rejected(), r.error);
    }
    {
        const Result r = parse("fixture - - - floor\nfixture - - - floor\n");
        check("a name given twice is refused", r.rejected(), r.error);
        check("and the message names the first definition",
              r.error.find("line 1") != std::string::npos, r.error);
    }
    {
        const Result r = parse("half test_material.bmp - - terrain\n");
        check("a scene that names material without albedo is refused", r.rejected(), r.error);
    }
    {
        const Result r = parse("void - - - terrain\n");
        check("an empty scene may not ask for a terrain spawn", r.rejected(), r.error);
    }
    {
        const Result r = parse("nowhere - - - hovering\n");
        check("an unknown spawn rule is refused", r.rejected(), r.error);
    }

    // --- data that names a path may not name any path ------------------------
    //
    // The prop list learned this one; a scene list reaches the filesystem the same
    // way and gets the same refusal.
    //
    // text_record::name_ok is the one check behind every name in assets/ -- scene
    // names, prop sprites, the sprite manifest's files, backdrop layers -- so its
    // two forms are pinned here, once.
    check("a bare stem is a usable name", text_record::name_ok("test_material"));
    check("a forward slash is not", !text_record::name_ok("wip/draft"));
    check("a backslash is not", !text_record::name_ok("wip"
                                                      "\\"
                                                      "draft"));
    check("a parent reference is not", !text_record::name_ok(".."));
    check("an empty name is not", !text_record::name_ok(""));
    check("a dot is not part of a stem", !text_record::name_ok("a.bmp"));
    check("but is part of a filename", text_record::name_ok("a.bmp", true));
    check("a parent reference is still not, with dots allowed",
          !text_record::name_ok("..", true) && !text_record::name_ok("a..bmp", true));
    check("nor a separator", !text_record::name_ok("wip/a.bmp", true));
    {
        const Result r = parse("bad ../secret.bmp ../secret.bmp - floor\n");
        check("and a row that tries it is refused", r.rejected(), r.error);
    }
    {
        const Result r = parse("bad wip/draft.bmp wip/draft.bmp - floor\n");
        check("including through a subdirectory", r.rejected(), r.error);
    }
    {
        const Result r = parse("bad wip/draft.bmp wip/draft.bmp - floor backdrop=bg_x\n");
        check("...even in a scene with a backdrop, when it is not that backdrop's folder",
              r.rejected() && r.error.find("own backdrop= folder") != std::string::npos,
              r.error);
    }
    {
        // The one folder a scene's files may sit in: the naming rule in
        // assets/scenes.txt keeps a backdrop scene's maps and level with its layers.
        const Result r = parse("bg_x bg_x/bg_x_material.bmp bg_x/bg_x_albedo.bmp - terrain "
                               "backdrop=bg_x level=bg_x/bg_x_level.txt\n");
        check("a scene's files may sit in its own backdrop folder",
              r.scenes.size() == 1 && r.scenes[0].material == "bg_x/bg_x_material.bmp" &&
                  r.scenes[0].albedo == "bg_x/bg_x_albedo.bmp" &&
                  r.scenes[0].level == "bg_x/bg_x_level.txt",
              r.error);
    }
    {
        const char* bad[] = {"bg_x/../a.bmp", "bg_x/sub/a.bmp", "/a.bmp", "bg_x/.bmp",
                             "bg_x/", "bg_x\\a.bmp"};
        for (const char* file : bad) {
            const Result r = parse(std::string("bg_x ") + file + " - - terrain backdrop=bg_x\n");
            check((std::string("...and only as one plain name: '") + file + "' is refused").c_str(),
                  r.rejected(), r.error);
        }
    }
    // --- mood= ---------------------------------------------------------------
    {
        const Result r = parse("empty - - - floor\n");
        check("mood: a row without mood= has none (the legacy mood)",
              r.scenes.size() == 1 && r.scenes[0].mood.empty(), r.error);
    }
    {
        const Result r = parse("bg_x - - - floor backdrop=bg_x mood=bg_x/bg_x_noir_mood.txt\n");
        check("mood: a mood in the scene's own backdrop folder is kept",
              r.scenes.size() == 1 && r.scenes[0].mood == "bg_x/bg_x_noir_mood.txt", r.error);
    }
    {
        const Result r = parse("plain - - - floor mood=plain_mood.txt\n");
        check("mood: a mood at the assets/ root is kept",
              r.scenes.size() == 1 && r.scenes[0].mood == "plain_mood.txt", r.error);
    }
    {
        const Result r = parse("bg_x - - - floor backdrop=bg_x mood=bg_y/bg_y_mood.txt\n");
        check("mood: a mood in another set's folder is refused",
              r.rejected() && r.error.find("own backdrop= folder") != std::string::npos, r.error);
    }
    {
        const Result r = parse("bad - - - floor mood=bg_x/bg_x_mood.txt\n");
        check("mood: a mood in a folder, in a scene with no backdrop, is refused", r.rejected(),
              r.error);
    }
    {
        const Result r = parse("bad - - - floor mood=a_mood.txt mood=b_mood.txt\n");
        check("mood: mood= twice is refused",
              r.rejected() && r.error.find("twice") != std::string::npos, r.error);
    }
    {
        const char* bad[] = {"night.txt",           "_mood.txt",      "bg_x/_mood.txt",
                             "a_mood.bmp",          "a_mood",         "bg_x/../a_mood.txt",
                             "bg_x/sub/a_mood.txt", "a_mood.txt.bak", ""};
        for (const char* file : bad) {
            const Result r =
                parse(std::string("bg_x - - - floor backdrop=bg_x mood=") + file + "\n");
            check((std::string("mood: '") + file + "' is refused").c_str(), r.rejected(), r.error);
        }
    }
    {
        const Result r = parse("good - - - floor\nbad - - - floor mood=night.txt\n");
        check("mood: a bad mood= on one row loses the whole list",
              r.rejected() && r.error.find(":2:") != std::string::npos, r.error);
    }

    {
        const Result r = parse("bad noextension noextension - floor\n");
        check("a filename with no extension is refused", r.rejected(), r.error);
    }

    // --- the fallback -------------------------------------------------------
    //
    // This exists so that "the file is missing" and "the file lists exactly the
    // shipped location" are provably the same world rather than two things that look
    // the same.
    {
        const std::vector<SceneDef> d = scene_list::default_scene_list();
        check("the built-in default is the location the game shipped with",
              d.size() == 1 && d[0].name == "empty" &&
              d[0].material.empty() &&
              d[0].albedo.empty() &&
              d[0].props.empty() &&
              d[0].spawn == Spawn::Floor);

        const Result r = parse("empty - - - floor\n");
        check("and an explicit row for it parses to the same record",
              r.scenes.size() == 1 && d.size() == 1 &&
              r.scenes[0].name == d[0].name &&
              r.scenes[0].material == d[0].material &&
              r.scenes[0].albedo == d[0].albedo &&
              r.scenes[0].props == d[0].props &&
              r.scenes[0].spawn == d[0].spawn);
    }

    // --- the shipped file ----------------------------------------------------
    //
    // Parsed rather than described, for scene_test's reason: a format whose only
    // test is a synthetic fixture is one the shipped file can drift out of.
    {
        std::string error;
        const std::vector<SceneDef> s = load_scene_list("assets/scenes.txt", &error);
        check("assets/scenes.txt parses", !s.empty() && error.empty(),
              std::to_string(s.size()) + " scenes, error '" + error + "'");
        bool has_empty = false;
        for (const SceneDef& d : s) if (d.declared_empty()) has_empty = true;
        check("and it ships the empty scene", has_empty);
    }

    std::remove(TMP);
    return report();
}
