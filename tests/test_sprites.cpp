// The sprite manifest -- which BMP each key loads from.
//
// Same emphasis as the prop list next door: the happy path is two fields off a
// line and is not where this breaks. What is worth pinning is the behaviour
// around a bad manifest, because this file sits between "the art I dropped in
// assets/" and "the art on screen", and every way it can fail quietly is a
// session spent wondering why a drawing did not appear.
//
// Two properties are load-bearing and neither is obvious from reading the
// parser:
//
//   - an unlisted key falls back rather than failing, which is what lets the
//     manifest be deleted, truncated or partially written and still leave a
//     runnable game;
//   - a malformed manifest is rejected wholesale, not line by line. A
//     half-applied rebinding table is a scene where some art moved and some did
//     not, which is harder to diagnose than the art you already had.

#include "scene/sprites.h"
#include "test_util.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace {

// On disk rather than from a string: "a missing file is not an error" is part of
// the contract and is only testable against a real filesystem.
const char* TMP = "test_sprites_tmp.txt";

SpriteManifest parse(const std::string& contents, std::string* error) {
    { std::ofstream out(TMP); out << contents; }
    SpriteManifest m = load_sprite_manifest(TMP, error);
    std::remove(TMP);
    return m;
}

} // namespace

int main() {
    std::printf("=== Sprite manifest ===\n\n");

    // --- the happy path, briefly ------------------------------------------
    {
        std::string err;
        SpriteManifest m = parse("player_sheet my_sheet.bmp 14 26\n"
                                 "backdrop_sky sky.bmp\n", &err);
        check("two records parse", m.bindings().size() == 2,
              "got " + std::to_string(m.bindings().size()));
        check("a clean file reports no error", err.empty(), err);
        check("the key resolves to its file",
              m.path_for("player_sheet", "shipped.bmp") == "assets/my_sheet.bmp");
        check("the frame size is carried",
              m.find("player_sheet") && m.find("player_sheet")->frame_w == 14 &&
              m.find("player_sheet")->frame_h == 26);
        check("a record with no frame size is not a sheet",
              m.find("backdrop_sky") && m.find("backdrop_sky")->frame_w == 0);
    }

    // --- comments and blank lines -----------------------------------------
    {
        std::string err;
        SpriteManifest m = parse("# a header comment\n"
                                 "\n"
                                 "tree_a oak.bmp   # trailing comment\n", &err);
        check("comments and blanks are skipped", m.bindings().size() == 1,
              "got " + std::to_string(m.bindings().size()));
        check("a trailing comment does not become part of the file name",
              m.path_for("tree_a", "tree_a.bmp") == "assets/oak.bmp");
    }

    // --- the fallback, which is the whole reason this is safe to delete ----
    {
        std::string err;
        SpriteManifest m = parse("tree_a oak.bmp\n", &err);
        check("an unlisted key falls back to the shipped file",
              m.path_for("player_sheet", "player_sheet_fly.bmp") ==
                  "assets/player_sheet_fly.bmp");
        check("an unlisted key is not found", m.find("player_sheet") == nullptr);
    }
    {
        std::string err;
        SpriteManifest m = load_sprite_manifest("no_such_manifest_at_all.txt", &err);
        check("a missing manifest is empty, not an error", m.bindings().empty());
        check("a missing manifest leaves `error` untouched", err.empty(), err);
    }

    // --- refusals ----------------------------------------------------------
    //
    // Each of these is a file that would otherwise load and do something almost
    // right, which is the failure mode this suite exists for.
    {
        std::string err;
        SpriteManifest m = parse("player_sheet\n", &err);
        check("a key with no file is rejected", !err.empty());
        check("a rejected file yields nothing at all", m.bindings().empty());
    }
    {
        std::string err;
        parse("player_sheet ../../secrets.bmp\n", &err);
        check("a path escaping assets/ is rejected", !err.empty());
    }
    {
        std::string err;
        parse("player_sheet sheets/mine.bmp\n", &err);
        check("a path separator is rejected", !err.empty());
    }
    {
        // Half a frame size is a typo, and reading it as "not a sheet" would
        // silently drop the check the author was asking for.
        std::string err;
        parse("player_sheet mine.bmp 14\n", &err);
        check("a frame width with no height is rejected", !err.empty());
    }
    {
        std::string err;
        parse("player_sheet mine.bmp 0 26\n", &err);
        check("a zero frame size is rejected", !err.empty());
    }
    {
        // Last-wins would be a manifest where an earlier line is dead and nothing
        // says so -- the same silence as a prop planted inside a hill.
        std::string err;
        parse("player_sheet a.bmp\nplayer_sheet b.bmp\n", &err);
        check("a key bound twice is rejected", !err.empty());
    }
    {
        std::string err;
        SpriteManifest m = parse("tree_a oak.bmp\nplayer_sheet\n", &err);
        check("one bad line discards the whole manifest, not just that line",
              m.bindings().empty() && m.find("tree_a") == nullptr);
    }

    return report();
}
