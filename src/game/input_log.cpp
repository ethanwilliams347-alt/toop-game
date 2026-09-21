#include "game/input_log.h"
#include <cstdio>
#include <cstring>

namespace input_log {
namespace {

// "SLOPREC" plus a NUL, so a log opened in a text editor names itself. The
// version is a field rather than a digit in the magic: a refusal that can say
// "version 1, this build reads 2" is more useful than one that can only say the
// file is unrecognised.
constexpr char MAGIC[8] = {'S', 'L', 'O', 'P', 'R', 'E', 'C', '\0'};

// One record is eleven bytes, written field by field in little-endian rather
// than by dumping the struct. Input has padding and its layout is a compiler's
// choice; a log written by one build and read by another has to agree about
// bytes, not about a struct.
constexpr size_t RECORD_BYTES = 11;

void put8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}
void put64(std::vector<uint8_t>& b, uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
}

uint32_t get32(const std::vector<uint8_t>& b, size_t& off) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(b[off + i]) << (8 * i);
    off += 4;
    return v;
}
uint64_t get64(const std::vector<uint8_t>& b, size_t& off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
    off += 8;
    return v;
}

bool fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

constexpr size_t HEADER_BYTES = 8 + 4 + 4 + 4 + 8 + 4 + 8 + 8 + 4 + 4 + 4;  // + step count

} // namespace

uint64_t fingerprint(const Grid& grid) {
    // FNV-1a, 64-bit. Not a checksum with any cryptographic claim -- it answers
    // "is this the same world" and nothing else.
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ull;
        }
    };
    for (int y = 0; y < grid.get_height(); ++y) {
        for (int x = 0; x < grid.get_width(); ++x) {
            const Element e = grid.get_element(x, y);
            mix(static_cast<uint64_t>(e.type));
            mix(e.color);
            mix(e.updated_tag);
            mix(e.ticks);
        }
    }
    return h;
}

bool write(const char* path, const Log& log, std::string* error) {
    std::vector<uint8_t> buf;
    buf.reserve(HEADER_BYTES + log.steps.size() * RECORD_BYTES);

    for (char c : MAGIC) buf.push_back(static_cast<uint8_t>(c));
    put32(buf, FORMAT_VERSION);
    put32(buf, static_cast<uint32_t>(log.header.grid_w));
    put32(buf, static_cast<uint32_t>(log.header.grid_h));
    put64(buf, log.header.seed);
    put32(buf, static_cast<uint32_t>(log.header.scene_cells));
    put64(buf, log.header.start_fingerprint);
    put64(buf, log.header.end_fingerprint);
    put32(buf, static_cast<uint32_t>(log.header.end_player_x));
    put32(buf, static_cast<uint32_t>(log.header.end_player_y));
    put32(buf, static_cast<uint32_t>(log.steps.size()));

    for (const Input& in : log.steps) {
        const uint8_t buttons = static_cast<uint8_t>((in.left ? 1 : 0) | (in.right ? 2 : 0) |
                                                    (in.jump ? 4 : 0) | (in.dig ? 8 : 0) |
                                                    (in.brush_active ? 16 : 0));
        put8(buf, buttons);
        put8(buf, static_cast<uint8_t>(in.brush_type));
        put8(buf, static_cast<uint8_t>(in.brush_size));
        put32(buf, static_cast<uint32_t>(in.cursor_x));
        put32(buf, static_cast<uint32_t>(in.cursor_y));
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return fail(error, std::string(path) + ": could not be opened for writing");
    const size_t put = std::fwrite(buf.data(), 1, buf.size(), f);
    const bool closed_ok = std::fclose(f) == 0;
    if (put != buf.size() || !closed_ok)
        return fail(error, std::string(path) + ": the log was not fully written");
    return true;
}

bool read(const char* path, Log& log, std::string* error) {
    log = Log{};

    std::FILE* f = std::fopen(path, "rb");
    if (!f) return fail(error, std::string(path) + ": no such file");
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < static_cast<long>(HEADER_BYTES)) {
        std::fclose(f);
        return fail(error, std::string(path) + ": too small to hold a header");
    }
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    const size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) return fail(error, std::string(path) + ": short read");

    if (std::memcmp(buf.data(), MAGIC, sizeof(MAGIC)) != 0)
        return fail(error, std::string(path) + ": not a session log");

    size_t off = sizeof(MAGIC);
    Header h;
    h.version = get32(buf, off);
    if (h.version != FORMAT_VERSION)
        return fail(error, std::string(path) + ": written by format version " +
                               std::to_string(h.version) + ", this build reads version " +
                               std::to_string(FORMAT_VERSION) + " - re-record it");
    h.grid_w = static_cast<int32_t>(get32(buf, off));
    h.grid_h = static_cast<int32_t>(get32(buf, off));
    h.seed = get64(buf, off);
    h.scene_cells = static_cast<int32_t>(get32(buf, off));
    h.start_fingerprint = get64(buf, off);
    h.end_fingerprint = get64(buf, off);
    h.end_player_x = static_cast<int32_t>(get32(buf, off));
    h.end_player_y = static_cast<int32_t>(get32(buf, off));
    const uint32_t count = get32(buf, off);

    if (h.grid_w <= 0 || h.grid_h <= 0)
        return fail(error, std::string(path) + ": header names a zero-sized world");
    if (buf.size() - off != static_cast<size_t>(count) * RECORD_BYTES)
        return fail(error, std::string(path) + ": header claims " + std::to_string(count) +
                               " steps, which does not match the file's length - the log is truncated");

    log.header = h;
    log.steps.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        Input& in = log.steps[i];
        const uint8_t buttons = buf[off++];
        in.left = (buttons & 1) != 0;
        in.right = (buttons & 2) != 0;
        in.jump = (buttons & 4) != 0;
        in.dig = (buttons & 8) != 0;
        in.brush_active = (buttons & 16) != 0;

        const uint8_t type = buf[off++];
        // A brush type outside the table would index MATERIALS out of range on the
        // first painted step. Refused here rather than clamped: a log this corrupt
        // is not a session, and a clamped one would replay as a session that never
        // happened.
        if (type >= static_cast<uint8_t>(ElementType::Count))
            return fail(error, std::string(path) + ": step " + std::to_string(i) +
                                   " names material " + std::to_string(type) +
                                   ", which is not a material");
        in.brush_type = static_cast<ElementType>(type);

        in.brush_size = buf[off++];
        if (in.brush_size < 1)
            return fail(error, std::string(path) + ": step " + std::to_string(i) +
                                   " has a brush size of 0");

        in.cursor_x = static_cast<int32_t>(get32(buf, off));
        in.cursor_y = static_cast<int32_t>(get32(buf, off));
    }
    return true;
}

} // namespace input_log
