#include "text.h"
#include <cctype>

namespace ui {

namespace {

// Each glyph is 5 rows of 3 columns, '#' lit and '.' blank -- authored by eye as
// ASCII art rather than packed into a bitmask, because a font is data meant to
// be read and corrected by a human, not computed.
struct Glyph {
    const char* rows[GLYPH_HEIGHT];
};

const Glyph& blank() {
    static const Glyph g{{"...", "...", "...", "...", "..."}};
    return g;
}

// Only the characters the HUD readout needs, plus the rest of A-Z and 0-9 while
// the pattern is already being typed out. The next screen that needs a character
// not here adds one row, the same way a new material adds one row to MATERIALS.
const Glyph* glyph_for(char c) {
    switch (std::toupper(static_cast<unsigned char>(c))) {
        case '0': { static const Glyph g{{"###", "#.#", "#.#", "#.#", "###"}}; return &g; }
        case '1': { static const Glyph g{{".#.", "##.", ".#.", ".#.", "###"}}; return &g; }
        case '2': { static const Glyph g{{"###", "..#", "###", "#..", "###"}}; return &g; }
        case '3': { static const Glyph g{{"###", "..#", "###", "..#", "###"}}; return &g; }
        case '4': { static const Glyph g{{"#.#", "#.#", "###", "..#", "..#"}}; return &g; }
        case '5': { static const Glyph g{{"###", "#..", "###", "..#", "###"}}; return &g; }
        case '6': { static const Glyph g{{"###", "#..", "###", "#.#", "###"}}; return &g; }
        case '7': { static const Glyph g{{"###", "..#", "..#", "..#", "..#"}}; return &g; }
        case '8': { static const Glyph g{{"###", "#.#", "###", "#.#", "###"}}; return &g; }
        case '9': { static const Glyph g{{"###", "#.#", "###", "..#", "###"}}; return &g; }

        case 'A': { static const Glyph g{{".#.", "#.#", "###", "#.#", "#.#"}}; return &g; }
        case 'B': { static const Glyph g{{"##.", "#.#", "##.", "#.#", "##."}}; return &g; }
        case 'C': { static const Glyph g{{"###", "#..", "#..", "#..", "###"}}; return &g; }
        case 'D': { static const Glyph g{{"##.", "#.#", "#.#", "#.#", "##."}}; return &g; }
        case 'E': { static const Glyph g{{"###", "#..", "###", "#..", "###"}}; return &g; }
        case 'F': { static const Glyph g{{"###", "#..", "###", "#..", "#.."}}; return &g; }
        case 'G': { static const Glyph g{{"###", "#..", "#.#", "#.#", "###"}}; return &g; }
        case 'H': { static const Glyph g{{"#.#", "#.#", "###", "#.#", "#.#"}}; return &g; }
        case 'I': { static const Glyph g{{"###", ".#.", ".#.", ".#.", "###"}}; return &g; }
        case 'J': { static const Glyph g{{"..#", "..#", "..#", "#.#", "###"}}; return &g; }
        case 'K': { static const Glyph g{{"#.#", "#.#", "##.", "#.#", "#.#"}}; return &g; }
        case 'L': { static const Glyph g{{"#..", "#..", "#..", "#..", "###"}}; return &g; }
        case 'M': { static const Glyph g{{"#.#", "###", "###", "#.#", "#.#"}}; return &g; }
        case 'N': { static const Glyph g{{"#.#", "##.", "#.#", ".##", "#.#"}}; return &g; }
        case 'O': { static const Glyph g{{"###", "#.#", "#.#", "#.#", "###"}}; return &g; }
        case 'P': { static const Glyph g{{"##.", "#.#", "##.", "#..", "#.."}}; return &g; }
        case 'Q': { static const Glyph g{{"###", "#.#", "#.#", "##.", "..#"}}; return &g; }
        case 'R': { static const Glyph g{{"##.", "#.#", "##.", "#.#", "#.#"}}; return &g; }
        case 'S': { static const Glyph g{{"###", "#..", "###", "..#", "###"}}; return &g; }
        case 'T': { static const Glyph g{{"###", ".#.", ".#.", ".#.", ".#."}}; return &g; }
        case 'U': { static const Glyph g{{"#.#", "#.#", "#.#", "#.#", "###"}}; return &g; }
        case 'V': { static const Glyph g{{"#.#", "#.#", "#.#", "#.#", ".#."}}; return &g; }
        case 'W': { static const Glyph g{{"#.#", "#.#", "#.#", "###", "#.#"}}; return &g; }
        case 'X': { static const Glyph g{{"#.#", "#.#", ".#.", "#.#", "#.#"}}; return &g; }
        case 'Y': { static const Glyph g{{"#.#", "#.#", ".#.", ".#.", ".#."}}; return &g; }
        case 'Z': { static const Glyph g{{"###", "..#", ".#.", "#..", "###"}}; return &g; }

        case ':': { static const Glyph g{{"...", ".#.", "...", ".#.", "..."}}; return &g; }
        case '(': { static const Glyph g{{".#.", "#..", "#..", "#..", ".#."}}; return &g; }
        case ')': { static const Glyph g{{".#.", "..#", "..#", "..#", ".#."}}; return &g; }
        case '.': { static const Glyph g{{"...", "...", "...", "...", ".#."}}; return &g; }
        case '-': { static const Glyph g{{"...", "...", "###", "...", "..."}}; return &g; }
        case '/': { static const Glyph g{{"..#", "..#", ".#.", "#..", "#.."}}; return &g; }

        default: return nullptr;
    }
}

// Cell width in screen pixels for one glyph at `scale`: the 3-column glyph plus
// one column of spacing, so adjacent letters do not touch.
int advance(int scale) { return (GLYPH_WIDTH + 1) * scale; }

} // namespace

void draw_text(SDL_Renderer* renderer, int x, int y, int scale, const std::string& text, uint32_t color) {
    const uint8_t a = static_cast<uint8_t>((color >> 24) & 0xFF);
    const uint8_t r = static_cast<uint8_t>((color >> 16) & 0xFF);
    const uint8_t g = static_cast<uint8_t>((color >> 8) & 0xFF);
    const uint8_t b = static_cast<uint8_t>(color & 0xFF);
    SDL_SetRenderDrawColor(renderer, r, g, b, a);

    int cursor_x = x;
    for (char c : text) {
        if (c != ' ') {
            const Glyph* glyph = glyph_for(c);
            if (glyph) {
                for (int row = 0; row < GLYPH_HEIGHT; ++row) {
                    for (int col = 0; col < GLYPH_WIDTH; ++col) {
                        if (glyph->rows[row][col] == '#') {
                            const SDL_Rect cell{cursor_x + col * scale, y + row * scale, scale, scale};
                            SDL_RenderFillRect(renderer, &cell);
                        }
                    }
                }
            }
        }
        cursor_x += advance(scale);
    }
}

int text_width(const std::string& text, int scale) {
    if (text.empty()) return 0;
    return static_cast<int>(text.size()) * advance(scale) - scale;  // no trailing gap after the last glyph
}

} // namespace ui
