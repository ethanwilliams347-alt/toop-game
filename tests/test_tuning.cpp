// TUNING.md checked against the source it describes.
//
// TUNING.md once drifted far enough to document six mechanics that did not exist
// (coyote time, a jump buffer, wall slide, glide gravity, flap fall-cancel, a crush
// threshold) and to state wrong values for two that did. A reader who trusted it --
// human or agent -- would "restore" constants the design never had. A prose table
// cannot be static_asserted, so this suite is the next best thing: it parses the
// document and fails when the document and the code disagree.
//
// Three checks, of different strength:
//
//   1. Every row whose File column is a link, in any table, must name a
//      constant that the linked file actually declares ("NAME =" as a whole
//      identifier, on any line). This catches a removed or renamed constant,
//      which is how the drift started.
//   2. No link in the document may carry a line anchor (#L...). Rows used to
//      link the declaring line, and the line numbers went stale with every edit
//      above them; worse, every branch that re-pointed them conflicted with every
//      other branch that did. The file is enough to find a constant by name, and
//      check 1 is what keeps the name honest.
//   3. Every src/physics/player.h row must state the value the code holds, and
//      every constant listed in PLAYER_KNOBS below must have a row. Only player.h
//      rows are value-checked: they are the feel constants with plain numeric
//      values. Other tables describe values in words ("3 body heights") that
//      would need a parser per row to verify, and a parser that loose would be a
//      check that passes by accident.
//
// Runs from the source tree (WORKING_DIRECTORY in CMakeLists.txt), like
// scene_test, because the thing under test is a file there.

#include "physics/player.h"
#include "test_util.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Every player.h constant TUNING.md is expected to document, as its exact fx
// value. Plain-int constants are widened through fx::from_int so one comparison
// covers both kinds; the document writes both as decimals.
struct Knob {
    const char* name;
    int64_t value;  // fx raw bits
};

const Knob PLAYER_KNOBS[] = {
    {"MOVE_SPEED", Player::MOVE_SPEED},
    {"JUMP_SPEED", Player::JUMP_SPEED},
    {"GRAVITY", Player::GRAVITY},
    {"MAX_FALL_SPEED", Player::MAX_FALL_SPEED},
    {"MAX_STEP_HEIGHT", fx::from_int(Player::MAX_STEP_HEIGHT)},
    {"FLAP_IMPULSE", Player::FLAP_IMPULSE},
    {"FLAP_MAX_CLIMB", Player::FLAP_MAX_CLIMB},
    {"FLAP_INTERVAL_STEPS", fx::from_int(Player::FLAP_INTERVAL_STEPS)},
    {"MAX_HEALTH", fx::from_int(Player::MAX_HEALTH)},
    {"BURN_TEMPERATURE", fx::from_int(Player::BURN_TEMPERATURE)},
    {"BURN_DAMAGE", fx::from_int(Player::BURN_DAMAGE)},
    {"BURN_INTERVAL_STEPS", fx::from_int(Player::BURN_INTERVAL_STEPS)},
};

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(' ') - b + 1);
}

// "| a | b | c |" -> {"a", "b", "c"}
std::vector<std::string> cells_of(const std::string& row) {
    std::vector<std::string> out;
    size_t start = row.find('|');
    while (start != std::string::npos) {
        const size_t end = row.find('|', start + 1);
        if (end == std::string::npos) break;
        out.push_back(trim(row.substr(start + 1, end - start - 1)));
        start = end;
    }
    return out;
}

// A decimal as written in the Now column ("112.5", "3", "0.50") parsed into fx
// exactly, as the rational digits / 10^k -- the same from_ratio the source uses,
// so no float stands between the document and the comparison. Returns false on
// anything that is not a plain non-negative decimal.
bool parse_decimal_fx(const std::string& s, int64_t* out) {
    if (s.empty()) return false;
    int64_t num = 0, den = 1;
    bool seen_point = false, seen_digit = false;
    for (char c : s) {
        if (c == '.' && !seen_point) {
            seen_point = true;
        } else if (c >= '0' && c <= '9') {
            num = num * 10 + (c - '0');
            if (seen_point) den *= 10;
            seen_digit = true;
            if (num > 1'000'000'000) return false;
        } else {
            return false;
        }
    }
    if (!seen_digit) return false;
    *out = fx::from_ratio(num, den);
    return true;
}

// The declaration line must contain "NAME =" as a whole identifier, not merely
// mention NAME in a comment or inside a longer name.
bool declares(const std::string& line, const std::string& name) {
    size_t pos = 0;
    while ((pos = line.find(name, pos)) != std::string::npos) {
        const bool left_ok = pos == 0 || !(std::isalnum(static_cast<unsigned char>(line[pos - 1])) ||
                                           line[pos - 1] == '_');
        size_t after = pos + name.size();
        while (after < line.size() && line[after] == ' ') after++;
        if (left_ok && after < line.size() && line[after] == '=' &&
            (after + 1 >= line.size() || line[after + 1] != '='))
            return true;
        pos += name.size();
    }
    return false;
}

} // namespace

int main() {
    const std::vector<std::string> doc = read_lines("TUNING.md");
    check("TUNING.md is readable from the source tree", !doc.empty());
    if (doc.empty()) return report();

    std::vector<std::string> documented_player;
    int linked_rows = 0;

    for (const std::string& row : doc) {
        if (row.rfind("| `", 0) != 0) continue;
        const std::vector<std::string> cells = cells_of(row);
        if (cells.size() < 3) continue;

        // `NAME`
        const std::string& first = cells[0];
        if (first.size() < 3 || first.front() != '`' || first.back() != '`') continue;
        const std::string name = first.substr(1, first.size() - 2);
        if (name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
            continue;  // `idle` etc. in the animation table: not a constant

        // [file](path)
        const std::string& link = cells[1];
        const size_t open = link.find("](");
        const size_t close = link.find(')', open == std::string::npos ? 0 : open);
        if (link.empty() || link.front() != '[' || open == std::string::npos ||
            close == std::string::npos)
            continue;  // unlinked row: nothing to check a declaration against
        const std::string path = link.substr(open + 2, close - open - 2);
        linked_rows++;

        bool declared = false;
        for (const std::string& line : read_lines(path)) declared |= declares(line, name);
        check(("`" + name + "` is declared in " + path).c_str(), declared,
              "the constant was renamed or removed, or the row links the wrong file");

        if (path != "src/physics/player.h") continue;
        documented_player.push_back(name);

        const Knob* knob = nullptr;
        for (const Knob& k : PLAYER_KNOBS)
            if (name == k.name) knob = &k;
        check(("`" + name + "` is a known player knob").c_str(), knob != nullptr,
              "add it to PLAYER_KNOBS in tests/test_tuning.cpp");
        if (!knob) continue;

        int64_t stated = 0;
        const bool parsed = parse_decimal_fx(cells[2], &stated);
        check(("`" + name + "` states " + cells[2] + ", which the code holds").c_str(),
              parsed && stated == knob->value,
              parsed ? "code holds raw fx " + std::to_string(knob->value) +
                           " (/65536), doc says raw " + std::to_string(stated)
                     : "Now column is not a plain decimal");
    }

    for (size_t i = 0; i < doc.size(); ++i) {
        const bool anchored = doc[i].find(".h#L") != std::string::npos ||
                              doc[i].find(".cpp#L") != std::string::npos;
        check(("TUNING.md:" + std::to_string(i + 1) + " links a file, not a line").c_str(),
              !anchored, "drop the #L anchor; rows link the declaring file only");
    }

    check("TUNING.md has linked rows at all", linked_rows > 0,
          "table format changed? this suite would pass vacuously");

    for (const Knob& k : PLAYER_KNOBS) {
        bool found = false;
        for (const std::string& n : documented_player) found |= n == k.name;
        check((std::string("`") + k.name + "` has a TUNING.md row").c_str(), found);
    }

    return report();
}
