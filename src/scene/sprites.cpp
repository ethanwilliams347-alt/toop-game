#include "sprites.h"

#include <fstream>
#include <sstream>

bool sprite_file_name_ok(const std::string& file) {
    if (file.empty()) return false;
    if (file.find('/') != std::string::npos) return false;
    if (file.find('\\') != std::string::npos) return false;
    if (file.find("..") != std::string::npos) return false;
    for (char c : file) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

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
        if (error) {
            std::ostringstream msg;
            msg << path << ":" << line_no << ": " << why;
            *error = msg.str();
        }
        manifest.bindings_.clear();
        return manifest;
    };

    while (std::getline(in, line)) {
        ++line_no;

        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        std::istringstream fields(line);
        SpriteBinding b;
        if (!(fields >> b.key)) continue;  // blank or comment-only

        if (!(fields >> b.file))
            return fail("'" + b.key + "' names no file - a record is "
                        "<key> <file.bmp> [<frame_w> <frame_h>]");

        if (!sprite_file_name_ok(b.file))
            return fail("'" + b.file + "' is not a usable file name (letters, digits, "
                        "_ - . only; no path separators - the file must sit in assets/)");

        // The frame size is optional, but half of it is not: a record carrying a
        // width and no height is a typo, and reading it as "not a sheet" would
        // silently discard the check the author was asking for.
        if (fields >> b.frame_w) {
            if (!(fields >> b.frame_h))
                return fail("'" + b.key + "' gives a frame width but no height - "
                            "write both or neither");
            if (b.frame_w <= 0 || b.frame_h <= 0)
                return fail("'" + b.key + "' has a non-positive frame size");
        }

        if (manifest.find(b.key))
            return fail("'" + b.key + "' is bound twice - the second would win "
                        "silently, so it is an error instead");

        manifest.bindings_.push_back(b);
    }

    return manifest;
}
