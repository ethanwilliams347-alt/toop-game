#pragma once
#include <string>
#include <vector>

// Which BMP a piece of art actually loads from, as data rather than as a string
// literal in main.cpp.
//
// assets/ is copied next to the exe at build time, so with hardcoded paths
// swapping a sheet meant editing C++ and rebuilding. A BMP dropped into assets/
// and named here is picked up on the next launch; tools/load_sprite.py writes
// the entry and stages the file.
//
// This is a rebinding table, not a loader. It maps a stable key that code refers
// to onto a filename that art owns. Nothing here opens a BMP, knows what SDL is,
// or decides what a frame means.
//
// The frame size below is for catching a mismatch early: a sheet bound to a key
// whose frames are a different size still loads and still draws, it just draws
// the wrong rectangles, silently.
//
// Absent or unlisted is never an error. Every lookup takes the default the code
// would have used anyway, so deleting this file gets you the shipped art rather
// than a black screen.
struct SpriteBinding {
    std::string key;  // what code asks for, e.g. "player_sheet"
    std::string file;  // a bare filename inside assets/, e.g. "my_sheet.bmp"

    // Frame size in cells for a sheet, or 0 for a plain single image. Advisory: the
    // caller checks it, the loader only carries it.
    int frame_w = 0;
    int frame_h = 0;
};

class SpriteManifest {
public:
    // `assets/<file>` for `key`, or `assets/<fallback_file>` if the key is not
    // listed. The fallback is what makes an unlisted key harmless.
    std::string path_for(const std::string& key, const std::string& fallback_file) const;

    // Null when the key is not listed. For callers that want to check a frame size
    // before drawing with it.
    const SpriteBinding* find(const std::string& key) const;

    const std::vector<SpriteBinding>& bindings() const { return bindings_; }

    std::vector<SpriteBinding> bindings_;
};

// The player sheet's key, and the shipped file it falls back to when the
// manifest does not list it -- or is missing, or is malformed and ignored.
//
// Named here rather than written as a literal at the call site, so the fallback
// has a test: boot_test reads this file out of the shipped assets/. The literal
// in main.cpp once named a sheet that had been moved to assets/wip/, which is
// never staged, so deleting sprites.txt -- documented as safe -- quietly turned
// the player into the plain-rectangle fallback.
//
// Props need no entry: a prop's key is its own name and its fallback is
// `<name>.bmp`, which boot_test already checks for every prop the list names.
inline constexpr const char* PLAYER_SHEET_KEY = "player_sheet";
inline constexpr const char* PLAYER_SHEET_FILE = "player_sheet.bmp";

// Parses the manifest. One record per line, `#` starts a comment:
//
//     <key> <file.bmp> [<frame_w> <frame_h>]
//
// `file` is a bare filename in assets/ (text_record::name_ok with the dot
// allowed): no path separators, no `..`. The frame size is two whole numbers or
// absent, and nothing may follow it.
//
// A missing file yields an empty manifest and leaves `error` untouched, which is
// how a caller tells "no manifest" apart from "a manifest that is wrong".
SpriteManifest load_sprite_manifest(const std::string& path, std::string* error);
