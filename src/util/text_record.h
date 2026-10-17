#pragma once
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <system_error>

// What every line-per-record file in assets/ has in common: the scene list, the
// prop list, a level file, the sprite manifest and a backdrop set. Each of those
// parsers used to carry its own copy of all four pieces below, and the copies had
// started to disagree -- three hand-written name checks, a fourth with a different
// character test, two number readers that took "1e3" and "+5" and two that did
// not, and an error line that said `path:0:` in one file and `path:` in another.
//
// Here rather than under src/scene/ because a backdrop set is parsed in
// src/render/, and render does not include scene. Header-only and standard-library
// only, so it adds no source set, no link and no SDL to anything that includes it:
// every target already has src/ on its include path.
//
// The record format each file uses -- which fields, in what order, what a
// missing one means -- stays in that file's own parser. This is the vocabulary,
// not the grammar.
namespace text_record {

// `#` to the end of the line is a comment, in every file. Stripped before the
// fields are read, so a comment can follow a record on the same line.
inline void strip_comment(std::string& line) {
    const size_t hash = line.find('#');
    if (hash != std::string::npos) line.erase(hash);
}

// True if `name` is something a parser will turn into a path under assets/, or
// use as an identifier: letters, digits, `_` and `-`, and `.` only when
// `allow_dot` says the name is a whole filename rather than a stem.
//
// A whitelist rather than a list of refusals, because data that names a path
// can name any path. A separator (`/` or `\`) is outside the set, so a name
// never leaves the directory it is resolved in; `..` is refused separately for
// the one spelling the set would otherwise let through. Plain ASCII ranges
// rather than std::isalnum, which reads the C locale and would let a different
// locale widen the set.
inline bool name_ok(std::string_view name, bool allow_dot = false) {
    if (name.empty()) return false;
    for (const char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-' || (allow_dot && c == '.');
        if (!ok) return false;
    }
    return name.find("..") == std::string_view::npos;
}

// --- numbers ---------------------------------------------------------------
//
// The whole field is the number, or the field is not a number.
//
// std::from_chars rather than a stream read. `>>` takes the longest number it
// can and stops, so "120px" reads as 120 and leaves "px" for the next field to
// misreport, and it is generous about what a number is: a leading '+', an
// exponent ("1e3" is a thousand), and on MSVC a hex float ("0x10" is sixteen).
// Each of those is a typo that loads as a different value rather than an error.
// from_chars takes no sign but '-', no whitespace and no locale, and says where
// it stopped, so "the whole field" is one pointer comparison. It also reports
// overflow instead of handing back a value too large for the int it was about
// to become.
//
// No exceptions on purpose: the build compiles without them, which is why none
// of the parsers use std::stoi.

// An integer, signed: "-8", "0", "120". Refuses "+5", "1e2", "0x40", "120px",
// "12.5", an empty field and anything outside int. For a field whose own range
// check names the bound better than "not a number" would.
inline bool integer(std::string_view raw, int& out) {
    const char* const first = raw.data();
    const char* const last = first + raw.size();
    int v = 0;
    const auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || ptr != last) return false;
    out = v;
    return true;
}

// A whole, non-negative count: "120". integer() and refuses "-5" as well.
inline bool whole(std::string_view raw, int& out) {
    int v = 0;
    if (!integer(raw, v) || v < 0) return false;
    out = v;
    return true;
}

// A decimal in plain notation: "187.5", "-12", "100", ".5". Refuses "120px",
// "1e3", "+12", "0x10", "inf", "nan" and an empty field. Fixed notation is what
// bars the exponent and the hex; isfinite is what bars inf and nan, which
// from_chars accepts in every notation.
inline bool decimal(std::string_view raw, float& out) {
    const char* const first = raw.data();
    const char* const last = first + raw.size();
    float v = 0.0f;
    const auto [ptr, ec] = std::from_chars(first, last, v, std::chars_format::fixed);
    if (ec != std::errc{} || ptr != last || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// --- errors ----------------------------------------------------------------

// "path:line: why", the one shape every parser reports in, so an editor or a
// terminal can jump to the line. `line` 0 is a complaint about the file as a
// whole (it cannot be opened, it lists nothing), which has no line to name and
// is written "path: why" rather than pointing at a line 0 that does not exist.
inline std::string at(const std::string& path, int line, const std::string& why) {
    std::string msg = path;
    if (line > 0) {
        msg += ':';
        msg += std::to_string(line);
    }
    msg += ": ";
    msg += why;
    return msg;
}

} // namespace text_record
