#include "props.h"

#include <fstream>
#include <sstream>
#include "util/text_record.h"

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
        if (error) *error = text_record::at(path, line_no, why);
        props.clear();
        return props;
    };

    while (std::getline(in, line)) {
        ++line_no;

        text_record::strip_comment(line);

        std::istringstream fields(line);
        std::string sprite;
        if (!(fields >> sprite)) continue;  // blank or comment-only

        // A stem, not a filename: the loader adds the .bmp.
        if (!text_record::name_ok(sprite))
            return fail("'" + sprite + "' is not a usable sprite name "
                        "(letters, digits, _ and - only; no path separators)");

        // Read as a whole field and then converted, rather than with >> into a
        // float: >> stops at "120px" and leaves "px" for the extra-field check
        // below, which then blames an authored y the author never wrote.
        std::string x_raw;
        if (!(fields >> x_raw)) return fail("'" + sprite + "' has no x coordinate");
        float x = 0.0f;
        if (!text_record::decimal(x_raw, x)) {
            std::string why = "'" + sprite + "' has an x of '";
            why += x_raw;
            why += "', which is not a number (plain decimal, like 145 or 187.5)";
            return fail(why);
        }

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
