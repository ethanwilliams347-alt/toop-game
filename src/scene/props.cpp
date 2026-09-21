#include "props.h"

#include <fstream>
#include <sstream>

bool prop_sprite_name_ok(const std::string& sprite) {
    if (sprite.empty()) return false;
    if (sprite.find('/') != std::string::npos) return false;
    if (sprite.find('\\') != std::string::npos) return false;
    if (sprite.find("..") != std::string::npos) return false;
    for (char c : sprite) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::vector<PropDef> load_prop_list(const std::string& path, std::string* error) {
    std::vector<PropDef> props;

    std::ifstream in(path);
    // Absent is not malformed. A scene with no props is a scene with no props, and
    // `error` staying untouched is how a caller tells that apart from a file that
    // exists and is wrong.
    if (!in) return props;

    std::string line;
    int line_no = 0;

    auto fail = [&](const std::string& why) {
        if (error) {
            std::ostringstream msg;
            msg << path << ":" << line_no << ": " << why;
            *error = msg.str();
        }
        props.clear();
        return props;
    };

    while (std::getline(in, line)) {
        ++line_no;

        // Comments are stripped before anything else, so a `#` can follow a record
        // on the same line.
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        std::istringstream fields(line);
        std::string sprite;
        if (!(fields >> sprite)) continue;  // blank or comment-only

        if (!prop_sprite_name_ok(sprite))
            return fail("'" + sprite + "' is not a usable sprite name "
                        "(letters, digits, _ and - only; no path separators)");

        float x = 0.0f;
        if (!(fields >> x))
            return fail("'" + sprite + "' has no x coordinate");

        // A trailing field is a typo, or a y coordinate someone expected to be
        // read -- see props.h for why there is no y. Rejected rather than
        // ignored, because an ignored number is one that gets tuned.
        std::string extra;
        if (fields >> extra)
            return fail("unexpected '" + extra + "' after the x coordinate "
                        "(a prop has no authored y; it is planted on the terrain "
                        "under it)");

        props.push_back(PropDef{ sprite, x, line_no });
    }

    return props;
}
