#pragma once
#include "scene/scene.h"
#include <cstdint>
#include <string>
#include <vector>

// Reading an authored BMP without SDL, and turning a material/albedo pair into
// a Scene.
//
// Headless by requirement: the benchmark replays a recorded session and has to
// stamp the same fixture scene the game stamps, with no window, renderer or SDL.
//
// Deliberately not a second implementation -- main.cpp calls this too, and has
// no SDL_LoadBMP path of its own. Two readers agreeing today are two readers
// that can disagree later.
namespace bmp {

// One decoded image, top row first regardless of how the file stores it, as
// 0xAARRGGBB with alpha forced opaque. Matches read_bmp in tools/pixel_art.py,
// which is the other reader of these same files.
struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint32_t> pixels;
};

// 24-bit and 32-bit uncompressed BMPs, which is what every tool in tools/
// writes, and 32-bit BI_BITFIELDS when its masks say plain BGRA. Anything else
// is refused with a message rather than half-read: a palette-indexed or RLE
// file decoded as raw rows, or a bitfield file with its channels in another
// order, produces a plausible image, and a scene that loads as noise is harder
// to diagnose than one that does not load.
bool read(const char* path, Image& out, std::string* error);

// Builds a Scene from a material map and an albedo map, exactly as the game
// does. Returns an empty scene on failure with `error` set -- including a
// material pixel whose colour is in no legend entry, which rejects the whole
// file rather than loading that cell as Empty.
Scene load(const char* material_path, const char* albedo_path, std::string* error);

} // namespace bmp
