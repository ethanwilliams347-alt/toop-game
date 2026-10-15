#include "sprites.h"

#include <fstream>
#include <sstream>
#include "util/text_record.h"

const SpriteBinding* SpriteManifest::find(const std::string& key) const {
    for (const auto& b : bindings_)
        if (b.key == key) return &b;
    return nullptr;
}

std::string SpriteManifest::path_for(const std::string& key,
                                     const std::string& fallback_file) const {
    const SpriteBinding* b = find(key);
    return "assets/" + (b ? b->file : fallback_file);
}

SpriteManifest load_sprite_manifest(const std::string& path, std::string* error) {
    SpriteManifest manifest;

    std::ifstream in(path);
    // Absent is not malformed -- every caller has a fallback, so no manifest means
    // the shipped art, not a failure.
    if (!in) return manifest;

    std::string line;
    int line_no = 0;

    auto fail = [&](const std::string& why) {
        if (error) *error = text_record::at(path, line_no, why);
        manifest.bindings_.clear();
        return manifest;
    };

    while (std::getline(in, line)) {
        ++line_no;

        text_record::strip_comment(line);

        std::istringstream fields(line);
        SpriteBinding b;
        if (!(fields >> b.key)) continue;  // blank or comment-only

        if (!(fields >> b.file))
            return fail("'" + b.key + "' names no file - a record is "
                        "<key> <file.bmp> [<frame_w> <frame_h>]");

        // A whole filename, so the dot is allowed; `..` and separators are not.
        if (!text_record::name_ok(b.file, true))
            return fail("'" + b.file + "' is not a usable file name (letters, digits, "
                        "_ - . only; no path separators - the file must sit in assets/)");

        // The frame size is optional, but half of it is not: a record carrying a
        // width and no height is a typo, and reading it as "not a sheet" would
        // silently discard the check the author was asking for.
        //
        // Read as whole fields, like every other number in assets/. A stream read
        // into the int used to take "14px" as 14 and then blame the "px" for being
        // a missing height, and took "abc" as no frame size at all -- a sheet
        // silently unchecked.
        std::string w_raw, h_raw;
        if (fields >> w_raw) {
            if (!(fields >> h_raw))
                return fail("'" + b.key + "' gives a frame width but no height - "
                            "write both or neither");
            if (!text_record::integer(w_raw, b.frame_w) ||
                !text_record::integer(h_raw, b.frame_h)) {
                std::string why = "'" + b.key + "' has a frame size of '";
                why += w_raw;
                why += ' ';
                why += h_raw;
                why += "', which is not two whole numbers";
                return fail(why);
            }
            if (b.frame_w <= 0 || b.frame_h <= 0)
                return fail("'" + b.key + "' has a non-positive frame size");
        }

        // Anything after the frame size is a typo or a field someone expected to
        // be read; ignored, it would be one that gets tuned for nothing.
        std::string extra;
        if (fields >> extra)
            return fail("unexpected '" + extra + "' after '" + b.key +
                        "' - a record is <key> <file.bmp> [<frame_w> <frame_h>]");

        if (manifest.find(b.key))
            return fail("'" + b.key + "' is bound twice - the second would win "
                        "silently, so it is an error instead");

        manifest.bindings_.push_back(b);
    }

    return manifest;
}
