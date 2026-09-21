#pragma once
#include <SDL.h>
#include <cstdint>
#include <string>

// A small hand-authored bitmap font, covering only the characters actually
// needed.
//
// The UI is immediate-mode and drawn directly against SDL_Renderer with no new
// dependency. SDL draws no glyphs by itself, and pulling in SDL_ttf for a HUD
// readout would be the kind of dependency that decision exists to avoid.
//
// SDL-side by design, unlike src/physics: there is nothing to assert about a
// pixel shape that a human is going to read.
namespace ui {

constexpr int GLYPH_WIDTH = 3;
constexpr int GLYPH_HEIGHT = 5;

// Draws `text` at (x, y) in screen pixels, each glyph cell `scale` pixels wide,
// left to right. Characters outside A-Z (case-insensitive), 0-9, and the
// punctuation glyph_for() defines draw as blank space rather than failing -- a
// HUD string is authored, not user input, so an unmapped glyph is a gap to
// notice on screen and add, not a reason to stop the frame.
void draw_text(SDL_Renderer* renderer, int x, int y, int scale, const std::string& text, uint32_t color);

// Width in screen pixels a call to draw_text with the same text and scale would
// occupy, for right-aligning or centring a string before it is drawn.
int text_width(const std::string& text, int scale);

} // namespace ui
