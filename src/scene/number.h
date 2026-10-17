#pragma once
#include <charconv>
#include <cmath>
#include <string>
#include <system_error>

// How a scene file reads a number: the whole field is the number, or the field
// is not a number. Shared by the level file and the prop list, so a column means
// the same thing in both.
//
// std::from_chars rather than a stream read. `>>` takes the longest number it
// can and stops, so "120px" reads as 120 and leaves "px" for the next field to
// misreport (as an authored y, in both files), and it is generous about what a
// number is: a leading '+', an exponent ("1e3" is a thousand), and on MSVC a
// hex float ("0x10" is sixteen). Each of those is a typo that loads as a
// different position rather than an error. from_chars takes no sign but '-', no
// whitespace and no locale, and says where it stopped, so "the whole field" is
// one pointer comparison. It also reports overflow instead of handing back a
// float too large for the int it was about to be cast to.
//
// No exceptions on purpose: the build compiles without them, which is the same
// reason scene_list.cpp does not use std::stoi.
namespace number {

// A whole, non-negative count of cells: "120". Refuses "120.5", "-5", "+5",
// "1e2", "0x40", "120px", an empty field, and anything past INT_MAX.
inline bool whole(const std::string& raw, int& out) {
    const char* const first = raw.data();
    const char* const last = first + raw.size();
    int v = 0;
    const auto [ptr, ec] = std::from_chars(first, last, v);
    if (ec != std::errc{} || ptr != last || v < 0) return false;
    out = v;
    return true;
}

// A decimal in plain notation: "187.5", "-12", "100". Refuses "120px", "1e3",
// "+12", "0x10", "inf", "nan" and an empty field. Fixed notation is what bars the
// exponent and the hex; isfinite is what bars inf and nan, which from_chars
// accepts in every notation.
inline bool decimal(const std::string& raw, float& out) {
    const char* const first = raw.data();
    const char* const last = first + raw.size();
    float v = 0.0f;
    const auto [ptr, ec] = std::from_chars(first, last, v, std::chars_format::fixed);
    if (ec != std::errc{} || ptr != last || !std::isfinite(v)) return false;
    out = v;
    return true;
}

} // namespace number
