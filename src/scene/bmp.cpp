#include "scene/bmp.h"
#include "scene/legend.h"
#include <cstdio>

namespace bmp {
namespace {

uint16_t u16(const std::vector<uint8_t>& d, size_t off) {
    return static_cast<uint16_t>(d[off] | (d[off + 1] << 8));
}
uint32_t u32(const std::vector<uint8_t>& d, size_t off) {
    return static_cast<uint32_t>(d[off]) | (static_cast<uint32_t>(d[off + 1]) << 8) |
           (static_cast<uint32_t>(d[off + 2]) << 16) | (static_cast<uint32_t>(d[off + 3]) << 24);
}
int32_t i32(const std::vector<uint8_t>& d, size_t off) {
    return static_cast<int32_t>(u32(d, off));
}

bool fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

} // namespace

bool read(const char* path, Image& out, std::string* error) {
    out = Image{};

    std::FILE* f = std::fopen(path, "rb");
    if (!f) return fail(error, std::string(path) + ": could not be opened");

    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 54) {
        std::fclose(f);
        return fail(error, std::string(path) + ": too small to be a BMP");
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) return fail(error, std::string(path) + ": short read");

    if (data[0] != 'B' || data[1] != 'M') return fail(error, std::string(path) + ": not a BMP");

    const uint32_t pixel_offset = u32(data, 10);
    const int32_t width = i32(data, 18);
    const int32_t height_raw = i32(data, 22);
    const uint16_t bpp = u16(data, 28);
    const uint32_t compression = u32(data, 30);

    if (bpp != 24 && bpp != 32) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%u-bit", static_cast<unsigned>(bpp));
        return fail(error, std::string(path) + ": only 24- and 32-bit uncompressed BMPs are read (got " +
                               buf + ")");
    }
    // 0 is BI_RGB; 3 is BI_BITFIELDS, which 32-bit BMPs written by some tools
    // use for a plain BGRA layout. Anything else is genuinely compressed.
    if (compression != 0 && !(compression == 3 && bpp == 32))
        return fail(error, std::string(path) + ": compressed BMPs are not read");
    if (width <= 0 || height_raw == 0) return fail(error, std::string(path) + ": zero-sized image");

    // A negative height means the rows are stored top-down. Both orders exist in
    // the wild and tools/pixel_art.py handles both; a reader that assumed one would
    // load half the project's own files upside down.
    const bool top_down = height_raw < 0;
    const int height = height_raw < 0 ? -height_raw : height_raw;
    const int bytes_per_pixel = bpp / 8;
    const size_t row_size = ((static_cast<size_t>(width) * bytes_per_pixel + 3) / 4) * 4;

    const size_t needed = pixel_offset + row_size * static_cast<size_t>(height);
    if (needed > data.size()) return fail(error, std::string(path) + ": pixel data is truncated");

    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height);

    for (int row = 0; row < height; ++row) {
        const int file_row = top_down ? row : (height - 1 - row);
        const size_t off = pixel_offset + row_size * static_cast<size_t>(file_row);
        for (int x = 0; x < width; ++x) {
            const size_t p = off + static_cast<size_t>(x) * bytes_per_pixel;
            const uint32_t b = data[p], g = data[p + 1], r = data[p + 2];
            out.pixels[static_cast<size_t>(row) * width + x] =
                0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
    return true;
}

Scene load(const char* material_path, const char* albedo_path,
           std::string* error, std::string* warning) {
    Scene scene;

    Image mat, alb;
    if (!read(material_path, mat, error)) return scene;
    if (!read(albedo_path, alb, error)) return scene;

    if (mat.width != alb.width || mat.height != alb.height) {
        if (error) *error = "scene BMP dimensions do not match: " +
                            std::string(material_path) + " is " + std::to_string(mat.width) + "x" +
                            std::to_string(mat.height) + ", " + albedo_path + " is " +
                            std::to_string(alb.width) + "x" + std::to_string(alb.height);
        return scene;
    }

    scene.width = mat.width;
    scene.height = mat.height;
    scene.materials.resize(static_cast<size_t>(scene.width) * scene.height, ElementType::Empty);
    scene.albedo.resize(static_cast<size_t>(scene.width) * scene.height, 0);

    // A pixel that names no material is a fault in the scene file, not an empty
    // cell. Counted, reported, and the first few offenders named, because "3
    // unmatched" sends you looking and "#4444FF" tells you what happened.
    int unmatched = 0;
    uint32_t first_unmatched[4] = {0, 0, 0, 0};
    int first_unmatched_n = 0;

    for (size_t i = 0; i < scene.materials.size(); ++i) {
        // The legend is its own frozen table (scene/legend.h), deliberately not
        // the render palette.
        ElementType type = ElementType::Empty;
        if (!element_from_legend(mat.pixels[i], type)) {
            const uint32_t rgb = mat.pixels[i] & 0xFFFFFF;
            bool seen = false;
            for (int k = 0; k < first_unmatched_n; ++k)
                if (first_unmatched[k] == rgb) seen = true;
            if (!seen && first_unmatched_n < 4) first_unmatched[first_unmatched_n++] = rgb;
            ++unmatched;
            type = ElementType::Empty;
        }
        scene.materials[i] = type;
        scene.albedo[i] = alb.pixels[i] | 0xFF000000;  // force alpha
    }

    if (unmatched > 0 && warning) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%d", unmatched);
        *warning = std::string(material_path) + " has " + buf +
                   " pixel(s) whose colour is in no legend entry; they loaded as Empty."
                   "\n         Unrecognised colours include:";
        for (int i = 0; i < first_unmatched_n; ++i) {
            std::snprintf(buf, sizeof(buf), " #%06X", first_unmatched[i]);
            *warning += buf;
        }
        *warning += "\n         The legend is src/scene/legend.h and is frozen; the render palette"
                    " in material.h is not the legend.";
    }

    return scene;
}

} // namespace bmp
