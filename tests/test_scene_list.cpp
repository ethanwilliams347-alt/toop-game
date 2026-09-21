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

const std::string GOOD =
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
        const Result r = parse(GOOD + "broken   test_material.bmp\n");
        check("a short row costs the whole list, not just itself", r.rejected(), r.error);
        check("and the message names the line", r.error.find(":4:") != std::string::npos,
              r.error);
    }
    {
        const Result r = parse(GOOD + "extra - - - floor sixth\n");
        check("a sixth field that is not a mode is an error, not something to ignore",
              r.rejected(), r.error);
    }

    // --- the optional columns ----------------------------------------------
    //
    // The thing being tested here is that "optional" did not become "tolerant". The
    // required fields are all-or-nothing and the optional ones are absent-or-exact;
    // a parser that fell back to the default on an unreadable mode or a lone width
    // would produce a world of the wrong size that loads and says nothing.
    {
        const Result r = parse("plain - - - floor\n");
        check("a row written before the mode column existed is fixed",
              r.scenes.size() == 1 && !r.scenes[0].is_infinite(), r.error);
        check("and states no size, rather than inventing one",
              r.scenes.size() == 1 && r.scenes[0].custom_width == 0 &&
              r.scenes[0].custom_height == 0);
    }
    {
        const Result r = parse("far - - - floor infinite\n");
        check("a mode with no size parses", r.scenes.size() == 1 &&
              r.scenes[0].is_infinite() && r.scenes[0].custom_width == 0, r.error);
    }
    {
        const Result r = parse("big test_material.bmp test_albedo.bmp - terrain fixed 3840 1440\n");
        check("a fixed row carries its own world size",
              r.scenes.size() == 1 && !r.scenes[0].is_infinite() &&
              r.scenes[0].custom_width == 3840 && r.scenes[0].custom_height == 1440,
              r.error);
    }
    {
        const Result r = parse("half - - - floor fixed 3840\n");
        check("a width with no height is refused, because half a size is not a size",
              r.rejected(), r.error);
    }
    {
        const Result r = parse("odd - - - floor sideways 100 100\n");
        check("an unknown mode is refused rather than defaulted", r.rejected(), r.error);
    }
    {
        const Result r = parse("zero - - - floor fixed 0 100\n");
        check("a world of no cells is refused", r.rejected(), r.error);
    }
    {
        const Result r = parse("neg - - - floor fixed -8 100\n");
        check("and so is a negative one", r.rejected(), r.error);
    }
    {
        const Result r = parse("wordy - - - floor fixed wide tall\n");
        check("a size that is not a number is refused", r.rejected(), r.error);
    }
    {
        const Result r = parse("toomuch - - - floor fixed 100 100 extra\n");
        check("a ninth field that is not a scale is an error", r.rejected(), r.error);
    }
    {
        const Result r = parse("toomuch2 - - - floor fixed 100 100 8 extra\n");
        check("a tenth field is still an error", r.rejected(), r.error);
    }

    // --- the scale column ---------------------------------------------
    //
    // The same absent-or-exact rule as the two columns above, plus one check that
    // the default the header states really is the camera's: SceneDef spells the
    // number itself rather than including src/game/camera.h, and that duplication is
    // only safe while something pins the two together.
    {
        const Result r = parse("noscale - - - floor fixed 100 100\n");
        check("a row that states no scale gets the engine default",
              r.scenes.size() == 1 && r.scenes[0].scale == Camera::DEFAULT_SCALE, r.error);
    }
    {
        const Result r = parse("scaled - - - floor fixed 344 144 10\n");
        check("a stated scale is read",
              r.scenes.size() == 1 && r.scenes[0].scale == 10, r.error);
    }
    {
        const Result r = parse("zeroscale - - - floor fixed 100 100 0\n");
        check("a scale of zero is refused, not defaulted", r.rejected(), r.error);
    }
    {
        const Result r = parse("bigscale - - - floor fixed 100 100 65\n");
        check("a scale past the cap is refused", r.rejected(), r.error);
    }
    {
        const Result r = parse("wordscale - - - floor fixed 100 100 huge\n");
        check("a scale that is not a number is refused", r.rejected(), r.error);
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
    check("a bare stem is a usable name", scene_list::scene_name_ok("test_material"));
    check("a forward slash is not", !scene_list::scene_name_ok("wip/draft"));
    check("a backslash is not", !scene_list::scene_name_ok("wip" "\\" "draft"));
    check("a parent reference is not", !scene_list::scene_name_ok(".."));
    check("an empty name is not", !scene_list::scene_name_ok(""));
    {
        const Result r = parse("bad ../secret.bmp ../secret.bmp - floor\n");
        check("and a row that tries it is refused", r.rejected(), r.error);
    }
    {
        const Result r = parse("bad wip/draft.bmp wip/draft.bmp - floor\n");
        check("including through a subdirectory", r.rejected(), r.error);
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
