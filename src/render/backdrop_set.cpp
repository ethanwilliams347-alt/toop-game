#include "render/backdrop_set.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace backdrop_set {

namespace {

// A number and nothing else: ">>" stops at "0.3x" and would leave the "x" for
// the next field to misreport.
bool parse_float(const std::string& raw, float& out) {
    std::istringstream num(raw);
    float v = 0.0f;
    char trailing = 0;
    if (!(num >> v) || (num >> trailing)) return false;
    out = v;
    return true;
}

bool parse_int(const std::string& raw, int& out) {
    std::istringstream num(raw);
    int v = 0;
    char trailing = 0;
    if (!(num >> v) || (num >> trailing)) return false;
    out = v;
    return true;
}

// "r0:r1" -- a half-open row range.
bool parse_range(const std::string& raw, int& r0, int& r1) {
    const size_t colon = raw.find(':');
    if (colon == std::string::npos) return false;
    return parse_int(raw.substr(0, colon), r0) && parse_int(raw.substr(colon + 1), r1);
}

// A layer file is a plain name in the set's directory: no separators, so a set
// cannot reach outside its own folder, and a .bmp, because that is what the
// loader reads.
bool file_ok(const std::string& f) {
    if (f.size() < 5 || f.compare(f.size() - 4, 4, ".bmp") != 0) return false;
    for (char c : f)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.'))
            return false;
    return f.find("..") == std::string::npos;
}

} // namespace

Set load(const std::string& path, const std::string& dir, std::string* error) {
    Set set;
    set.dir = dir;

    int line_no = 0;
    auto fail = [&](const std::string& why) {
        if (error) {
            std::ostringstream msg;
            msg << path;
            if (line_no > 0) msg << ":" << line_no;
            msg << ": " << why;
            *error = msg.str();
        }
        Set empty;
        empty.dir = dir;
        return empty;
    };

    std::ifstream in(path);
    if (!in) return fail("cannot be opened");

    int size_line = 0, anchor_line = 0, rig_line = 0, ripple_line = 0;
    std::string line;
    while (std::getline(in, line)) {
        ++line_no;
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);

        std::istringstream fields(line);
        std::string kind;
        if (!(fields >> kind)) continue;

        // Everything is stated in the art's own rows, so nothing can be checked
        // until the size is known -- and a second statement of any set-wide record
        // is two authors who disagree.
        if (kind != "size" && !size_line)
            return fail("`" + kind +
                        "` before `size`; the size comes first, since "
                        "every row below is checked against it");
        auto once = [&](int& seen) -> bool {
            if (seen) return false;
            seen = line_no;
            return true;
        };

        if (kind == "size") {
            if (!once(size_line)) return fail("a second `size` line");
            std::string w, h;
            if (!(fields >> w >> h) || !parse_int(w, set.native_w) || !parse_int(h, set.native_h) ||
                set.native_w <= 0 || set.native_h <= 0)
                return fail("`size` needs a width and a height, positive integers");
        } else if (kind == "anchor") {
            if (!once(anchor_line)) return fail("a second `anchor` line");
            std::string a;
            fields >> a;
            if (a == "corner")
                set.anchor = Anchor::Corner;
            else if (a == "standing")
                set.anchor = Anchor::Standing;
            else
                return fail("`anchor` is `corner` or `standing`, not '" + a + "'");
        } else if (kind == "rig") {
            if (!once(rig_line)) return fail("a second `rig` line");
            std::string h, c, k;
            if (!(fields >> h >> c >> k) || !parse_int(h, set.rig.horizon_row) ||
                !parse_int(c, set.rig.contact_row) || !parse_float(k, set.rig.vertical_strength))
                return fail("`rig` needs a horizon row, a contact row and a vertical strength");
            if (set.rig.horizon_row < 0 || set.rig.horizon_row >= set.rig.contact_row ||
                set.rig.contact_row > set.native_h)
                return fail("`rig` needs 0 <= horizon < contact <= the art's height");
            if (set.rig.vertical_strength < 0.0f || set.rig.vertical_strength > 1.0f)
                return fail("`rig`'s vertical strength is between 0 and 1");
            set.has_rig = true;
        } else if (kind == "ripple") {
            if (!once(ripple_line)) return fail("a second `ripple` line");
            std::string a;
            if (!(fields >> a) || !parse_float(a, set.ripple_amplitude) ||
                set.ripple_amplitude < 0.0f)
                return fail("`ripple` needs an amplitude in cells, not negative");
        } else if (kind == "layer") {
            Layer l;
            l.line = line_no;
            if (!(fields >> l.file) || !file_ok(l.file))
                return fail("`layer` needs a .bmp file name in the set's own directory");

            int ways = 0;
            std::string tok;
            while (fields >> tok) {
                const size_t eq = tok.find('=');
                const std::string key = tok.substr(0, eq);
                const std::string value = eq == std::string::npos ? "" : tok.substr(eq + 1);
                const bool has_value = eq != std::string::npos;

                if (key == "factor" && has_value) {
                    if (!parse_float(value, l.factor))
                        return fail("factor='" + value + "' is not a number");
                    l.has_factor = true;
                    ++ways;
                } else if (key == "foot" && has_value) {
                    if (!parse_int(value, l.foot_row) || l.foot_row < 0 ||
                        l.foot_row >= set.native_h)
                        return fail("foot='" + value + "' is not a row of the art");
                    ++ways;
                } else if (key == "plane" && !has_value) {
                    l.plane = true;
                    ++ways;
                } else if (key == "bands" && has_value) {
                    // r0:r1:f,r0:r1:f,... -- contiguous from row 0 to the bottom,
                    // because a gap shows the sky through the ground and an overlap
                    // draws one range twice at two offsets.
                    std::istringstream list(value);
                    std::string item;
                    int next = 0;
                    while (std::getline(list, item, ',')) {
                        const size_t c1 = item.find(':');
                        const size_t c2 = c1 == std::string::npos ? c1 : item.find(':', c1 + 1);
                        Band b{};
                        if (c2 == std::string::npos || !parse_int(item.substr(0, c1), b.row0) ||
                            !parse_int(item.substr(c1 + 1, c2 - c1 - 1), b.row1) ||
                            !parse_float(item.substr(c2 + 1), b.parallax_x))
                            return fail("band '" + item + "' is not row0:row1:factor");
                        if (b.row0 != next || b.row1 <= b.row0)
                            return fail("band '" + item +
                                        "' does not start where the last "
                                        "one ended (row " +
                                        std::to_string(next) + ")");
                        next = b.row1;
                        l.bands.push_back(b);
                    }
                    if (l.bands.empty() || next != set.native_h)
                        return fail("the bands stop at row " + std::to_string(next) +
                                    "; they must cover the art to row " +
                                    std::to_string(set.native_h));
                    ++ways;
                } else if (key == "opaque" && !has_value) {
                    l.opaque = true;
                } else if (key == "foreground" && !has_value) {
                    l.foreground = true;
                } else if (key == "on_plane" && !has_value) {
                    l.on_plane = true;
                } else if (key == "ripple" && has_value) {
                    if (!parse_range(value, l.ripple_row0, l.ripple_row1) || l.ripple_row0 < 0 ||
                        l.ripple_row1 <= l.ripple_row0 || l.ripple_row1 > set.native_h)
                        return fail("ripple='" + value + "' is not a range of the art's rows");
                } else if (key == "drift" && has_value) {
                    if (!parse_float(value, l.drift))
                        return fail("drift='" + value + "' is not a number");
                } else if (key == "width" && has_value) {
                    if (!parse_int(value, l.width) || l.width <= 0)
                        return fail("width='" + value + "' is not a positive number of cells");
                } else {
                    return fail("'" + tok +
                                "' is not a layer field (factor=, foot=, plane, "
                                "bands=, opaque, foreground, on_plane, ripple=, drift=, width=)");
                }
            }

            if (ways != 1)
                return fail("layer '" + l.file +
                            "' needs exactly one of factor=, foot=, "
                            "plane or bands=");
            if ((l.foot_row >= 0 || l.plane || l.on_plane) && !set.has_rig)
                return fail("layer '" + l.file +
                            "' stands on the plane, which needs a `rig` "
                            "line above it");
            if (l.on_plane && l.ripple_row1 <= l.ripple_row0)
                return fail("layer '" + l.file +
                            "' is on_plane, which is drawn over its "
                            "ripple= rows, and has none");
            // Bands are cut on flat paint so that one image can scroll at three
            // rates without a visible step. Vertical parallax would also have to
            // stretch each band about the horizon, and the cut that was invisible
            // sideways becomes a tear. The bg1 family is vertically locked for the
            // same reason (one painted plane under every layer), so a banded layer
            // belongs to a locked set.
            if (!l.bands.empty() && set.has_rig && set.rig.vertical_strength != 0.0f)
                return fail("layer '" + l.file +
                            "' is banded in a set with vertical "
                            "parallax; bands need `rig ... 0`, or no rig");
            set.layers.push_back(l);
        } else {
            return fail("'" + kind + "' is not a record (size, anchor, rig, ripple or layer)");
        }

        std::string extra;
        if (kind != "layer" && (fields >> extra)) {
            std::string msg = "unexpected '";
            msg += extra;
            msg += "' after `";
            msg += kind;
            msg += "`";
            return fail(msg);
        }
    }

    line_no = 0;
    if (!size_line) return fail("has no `size` line");
    if (set.layers.empty()) return fail("lists no layers");
    return set;
}

}  // namespace backdrop_set
