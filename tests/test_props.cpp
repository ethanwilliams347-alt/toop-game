// The prop list format.
//
// What is worth testing here is the refusals, not the happy path. Parsing two
// fields off a line is not where this can go wrong; where it can go wrong is a
// file that is subtly bad and loads anyway -- a palette retune that makes every
// authored pixel resolve to Empty, a props pass that plants trees inside a hill.
// Both render. Neither says anything. So most of what is below is a malformed
// file being rejected, and rejected wholesale rather than a line at a time.

#include "scene/props.h"
#include "test_util.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

// Written to disk rather than parsed from a string, because the loader's
// contract includes "a missing file is not an error" and that is only testable
// against a real filesystem.
const char* TMP = "test_props_tmp.txt";

std::vector<PropDef> parse(const std::string& contents, std::string* error) {
    { std::ofstream out(TMP); out << contents; }
    std::vector<PropDef> result = load_prop_list(TMP, error);
    std::remove(TMP);
    return result;
}

} // namespace

int main() {
    std::printf("=== Prop list format ===\n\n");

    // --- the happy path, briefly ------------------------------------------
    {
        std::string err;
        auto props = parse("tree_a 100\ntree_b 187.5\n", &err);
        check("two records parse", props.size() == 2, "got " + std::to_string(props.size()));
        check("sprite name is read", !props.empty() && props[0].sprite == "tree_a");
        check("x is read as a float", props.size() > 1 && props[1].x == 187.5f);
        check("a clean file reports no error", err.empty(), err);
    }

    // Comments, blanks and trailing annotation. All three appear in the shipped
    // prop list, so a parser that quietly mishandled any of them would empty the
    // real scene.
    {
        std::string err;
        auto props = parse("# header\n\n  \ntree_a 100   # east of the pit\n", &err);
        check("comments and blank lines are skipped", props.size() == 1,
              "got " + std::to_string(props.size()));
        check("a trailing comment does not break the record",
              props.size() == 1 && props[0].x == 100.0f);
        check("line numbers survive comments and blanks",
              props.size() == 1 && props[0].line == 4,
              props.empty() ? "" : "line " + std::to_string(props[0].line));
    }

    // --- refusals, which is what this suite is actually for -----------------
    //
    // Every one of these asserts the list comes back EMPTY, not short. A parser that
    // dropped the bad line and kept the good ones would pass a size check written
    // the obvious way and would be the bug.
    {
        std::string err;
        auto props = parse("tree_a 100\ntree_b\ntree_c 200\n", &err);
        check("a record with no x is an error", !err.empty(), err);
        check("...and it costs the whole list, not just its own line",
              props.empty(), "got " + std::to_string(props.size()));
        check("...and the error names the line", err.find(":2:") != std::string::npos, err);
    }
    {
        std::string err;
        auto props = parse("tree_a 100 380\n", &err);
        check("an authored y is rejected rather than ignored", !err.empty() && props.empty(), err);
    }
    {
        std::string err;
        auto props = parse("tree_a notanumber\n", &err);
        check("a non-numeric x is an error", !err.empty() && props.empty(), err);
    }

    // A prop list is data, and data that names a path can name any path. The loader
    // turns `sprite` into assets/<sprite>.bmp, so the guard has to be on the name
    // rather than on the path it becomes.
    {
        std::string err;
        auto props = parse("../../secrets 100\n", &err);
        check("a traversing sprite name is rejected", !err.empty() && props.empty(), err);
    }
    check("path separators are rejected", !prop_sprite_name_ok("sub/tree"));
    check("backslashes are rejected too", !prop_sprite_name_ok("sub\\tree"));
    check("dot-dot is rejected", !prop_sprite_name_ok("a..b"));
    check("an empty name is rejected", !prop_sprite_name_ok(""));
    check("an ordinary name is accepted", prop_sprite_name_ok("tree_a"));
    check("digits and dashes are accepted", prop_sprite_name_ok("rock-2"));

    // --- absent vs. broken --------------------------------------------------
    //
    // The distinction the whole error contract rests on: a scene with no props and a
    // scene whose props are unreadable must not look the same to a caller, or the
    // loud path is unreachable.
    {
        std::string err = "";
        auto props = load_prop_list("no_such_prop_file_at_all.txt", &err);
        check("a missing file yields no props", props.empty());
        check("...and is NOT reported as an error", err.empty(), err);
    }
    {
        std::string err;
        auto props = parse("", &err);
        check("an empty file yields no props and no error", props.empty() && err.empty(), err);
    }

    // --- the shipped fixture ------------------------------------------------
    //
    // Parsed rather than eyeballed. This is the file main.cpp actually loads, and a
    // typo in it is precisely the silent-blank-scene failure mode above. Reached by
    // two paths because the test binary's working directory differs between a CTest
    // run and a run from the build tree.
    {
        std::string err;
        auto props = load_prop_list("assets/test_props.txt", &err);
        if (props.empty() && err.empty())
            props = load_prop_list("../../assets/test_props.txt", &err);
        check("the shipped fixture parses", err.empty(), err);
        check("...and names nine props", props.size() == 9,
              "got " + std::to_string(props.size()));
        bool names_ok = true;
        for (const PropDef& p : props)
            if (!prop_sprite_name_ok(p.sprite)) names_ok = false;
        check("...all with usable sprite names", names_ok);
    }

    return report();
}
