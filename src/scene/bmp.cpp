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

    // BI_BITFIELDS says where each channel is, and the rows below are read as
    // BGRA -- so the masks have to say BGRA, or the image decodes with its
    // channels swapped and nothing fails. They follow the 40-byte info header,
    // which is also where a V4/V5 header keeps them, so offset 54 either way.
    if (compression == 3) {
        if (data.size() < 66)
            return fail(error, std::string(path) + ": bitfield BMP is missing its colour masks");
        const uint32_t r = u32(data, 54), g = u32(data, 58), b = u32(data, 62);
        if (r != 0x00FF0000u || g != 0x0000FF00u || b != 0x000000FFu) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "R=%08X G=%08X B=%08X", r, g, b);
            return fail(error, std::string(path) +
                                   ": bitfield BMP with channel masks other than BGRA (" + buf +
                                   ") is not read; re-export it as plain 24- or 32-bit");
        }
    }
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

Scene load(const char* material_path, const char* albedo_path, std::string* error) {
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

    // A pixel that names no material is a fault in the scene file, not an empty
    // cell, and it rejects the whole file -- the rule every parser here follows.
    // Loading it as Empty is a scene that renders, renders wrong, and says so
    // only on stderr; legend.h says why that conflation is the bug the legend
    // exists to prevent. The message names the first offender's position and
    // colour, and up to four distinct colours, because "3 unmatched" sends you
    // looking and "#4444FF at (12, 40)" tells you what happened.
    std::vector<ElementType> materials(mat.pixels.size(), ElementType::Empty);
    int unmatched = 0;
    size_t first_at = 0;
    uint32_t colours[4] = {0, 0, 0, 0};
    int colours_n = 0;
    for (size_t i = 0; i < materials.size(); ++i) {
        // The legend is its own frozen table (scene/legend.h), deliberately not
        // the render palette.
        if (element_from_legend(mat.pixels[i], materials[i])) continue;
        const uint32_t rgb = mat.pixels[i] & 0xFFFFFF;
        if (unmatched == 0) first_at = i;
        bool seen = false;
        for (int k = 0; k < colours_n; ++k)
            if (colours[k] == rgb) seen = true;
        if (!seen && colours_n < 4) colours[colours_n++] = rgb;
        ++unmatched;
    }
    if (unmatched > 0) {
        if (error) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          ": %d pixel(s) name no material, the first at (%d, %d);", unmatched,
                          static_cast<int>(first_at % static_cast<size_t>(mat.width)),
                          static_cast<int>(first_at / static_cast<size_t>(mat.width)));
            *error = std::string(material_path) + buf + " colours:";
            for (int i = 0; i < colours_n; ++i) {
                std::snprintf(buf, sizeof(buf), " #%06X", colours[i]);
                *error += buf;
            }
            *error += ". The legend is src/scene/legend.h and is frozen; the render palette in "
                      "material.h is not the legend.";
        }
        return scene;
    }

    scene.width = mat.width;
    scene.height = mat.height;
    scene.materials = std::move(materials);
    scene.albedo.resize(alb.pixels.size());
    for (size_t i = 0; i < alb.pixels.size(); ++i)
        scene.albedo[i] = alb.pixels[i] | 0xFF000000;  // force alpha
    return scene;
}

} // namespace bmp
