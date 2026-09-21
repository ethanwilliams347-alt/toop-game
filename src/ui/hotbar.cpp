#include "hotbar.h"
#include "text.h"
#include <algorithm>

namespace ui {

namespace {

// Authored like the font in text.cpp: ASCII art, read and corrected by eye,
// never packed into a bitmask. Three tones plus transparent, and the tones are
// relative -- '#' is the material's own colour from MATERIALS, '+' and '-' are
// the same colour lifted and dropped. Nothing here names a colour, so the
// palette can be relit without reopening this file.
//
//   '.' nothing   '-' shadow   '#' base   '+' highlight
struct Icon {
    const char* rows[ICON_SIZE];
};

// Every shape is a silhouette first. These are read at a glance while the player
// is doing something else, so each is picked to be distinguishable by outline
// alone, before colour helps at all. The two that are hardest to tell apart,
// Water and Oil, are a falling drop and a flat spreading pool for that reason
// rather than two drops in different colours.
const Icon& icon_art(ElementType type) {
    switch (type) {
        // A heap. Powder is the one behaviour in the table you can draw
        // directly: it piles into a slope, so the icon is the slope.
        case ElementType::Sand: {
            static const Icon g{{"........",
                                 "........",
                                 "...+....",
                                 "..###...",
                                 "..#-##..",
                                 ".#####+.",
                                 "#-#####-",
                                 "########"}};
            return g;
        }
        // A drop, mid-fall.
        case ElementType::Water: {
            static const Icon g{{"...+....",
                                 "...#....",
                                 "..###...",
                                 "..#+##..",
                                 ".##+###.",
                                 ".###+##.",
                                 "..####..",
                                 "...--..."}};
            return g;
        }
        // Courses of brick. The mortar lines are what make this unmistakable at
        // one glance, so they run all the way across rather than being hinted.
        case ElementType::Wall: {
            static const Icon g{{"########",
                                 "#+#..#+#",
                                 "########",
                                 "..#+#...",
                                 "########",
                                 "#+#..#+#",
                                 "########",
                                 "........"}};
            return g;
        }
        // The eraser. Not a material but a hole, so it is drawn as an outline
        // with nothing inside it -- the one shape in the row with no fill.
        case ElementType::Empty: {
            // The inner ring is drawn in the highlight tone, not the shadow tone:
            // this box has no fill, so a dark ring sits on the dark backing panel
            // and is simply not there at real size.
            static const Icon g{{"++++++++",
                                 "+......+",
                                 "+.++++.+",
                                 "+.+..+.+",
                                 "+.+..+.+",
                                 "+.++++.+",
                                 "+......+",
                                 "++++++++"}};
            return g;
        }
        // A plank seen end-on, with grain. Structural, so it reads as a bar
        // rather than as a heap -- the same distinction the physics makes.
        case ElementType::Wood: {
            static const Icon g{{"........",
                                 "++++++++",
                                 "#-####-#",
                                 "###-####",
                                 "#-####-#",
                                 "####-###",
                                 "--------",
                                 "........"}};
            return g;
        }
        // A pool, spread flat, with a slick on it. Deliberately the opposite
        // silhouette to Water's drop; both are Liquid, and the icons cannot lean
        // on colour to separate them because Oil is nearly black.
        case ElementType::Oil: {
            // The sheen is on the slick rather than above it. Two detached
            // highlight pixels floating over the pool read as stray noise at
            // real size, not as a reflection.
            static const Icon g{{"........",
                                 "........",
                                 "........",
                                 "........",
                                 ".######.",
                                 "##+###+#",
                                 "########",
                                 ".-####-."}};
            return g;
        }
        // A puff, rising, with wisps breaking off the top the way steam does.
        case ElementType::Steam: {
            static const Icon g{{"..+..+..",
                                 ".+..+...",
                                 "..####..",
                                 ".######.",
                                 "###--###",
                                 ".######.",
                                 "..####..",
                                 "........"}};
            return g;
        }
        // A flame. The dark core is not shading -- Fire's row carries the widest
        // jitter in the table because its variation is the drawing, and a flat
        // orange teardrop is the one shape that would misrepresent it.
        case ElementType::Fire: {
            static const Icon g{{"...+....",
                                 "..++....",
                                 "..+#+...",
                                 ".+#+#+..",
                                 ".+#-#+..",
                                 "+#-.-#+.",
                                 "+#---#+.",
                                 ".+###+.."}};
            return g;
        }
        // Charred is not on the hotbar -- it is a state wood arrives at, not
        // something you place -- but a row exists so the switch is total and a
        // future slot cannot silently fall through to a blank icon.
        case ElementType::Charred: {
            static const Icon g{{"........",
                                 "..####..",
                                 ".#-##-#.",
                                 "##-##-##",
                                 "#-####-#",
                                 "##-##-##",
                                 ".######.",
                                 "..####.."}};
            return g;
        }
        default: {
            static const Icon g{{"........", "........", "........", "........",
                                 "........", "........", "........", "........"}};
            return g;
        }
    }
}

uint32_t shade(uint32_t argb, float factor) {
    const auto channel = [&](int shift) {
        const int v = static_cast<int>(((argb >> shift) & 0xFF) * factor);
        return static_cast<uint32_t>(std::clamp(v, 0, 255));
    };
    return (argb & 0xFF000000u) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}

// The one place an icon is allowed to depart from its material's colour. The
// darkest materials sit on a dark panel, and painted honestly they are black
// squares: technically faithful and functionally absent. A base darker than this
// floor is lifted to it proportionally, which keeps the hue and gives the
// silhouette something to be seen against. Every other row passes through
// untouched.
uint32_t icon_base(ElementType type) {
    const uint32_t c = material_of(type).color;
    // The eraser has no colour at all -- Empty's row is transparent by design --
    // so it gets the HUD's own grey. It is a tool, not a material.
    if (type == ElementType::Empty) return 0xFFB0B0B0;

    const int brightest = std::max({(c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF});
    constexpr int FLOOR = 0x60;
    if (brightest >= FLOOR || brightest == 0) return c;
    return shade(c, static_cast<float>(FLOOR) / static_cast<float>(brightest));
}

void fill(SDL_Renderer* r, const SDL_Rect& rect, uint32_t argb) {
    SDL_SetRenderDrawColor(r,
                           static_cast<uint8_t>((argb >> 16) & 0xFF),
                           static_cast<uint8_t>((argb >> 8) & 0xFF),
                           static_cast<uint8_t>(argb & 0xFF),
                           static_cast<uint8_t>((argb >> 24) & 0xFF));
    SDL_RenderFillRect(r, &rect);
}

// Geometry, in one place so the row and the height agree by construction. A slot
// is the icon plus a one-authored-pixel border of padding on each side, then the
// key number under it in the glyph font.
int icon_scale(int ui_scale) { return std::max(1, ui_scale); }
int slot_size(int ui_scale) { return (ICON_SIZE + 2) * icon_scale(ui_scale); }
int slot_gap(int ui_scale) { return 2 * icon_scale(ui_scale); }
int label_gap(int ui_scale) { return icon_scale(ui_scale); }

} // namespace

void draw_icon(SDL_Renderer* renderer, int x, int y, int scale, ElementType type) {
    const Icon& art = icon_art(type);
    const uint32_t base = icon_base(type);
    const uint32_t light = shade(base, 1.45f);
    const uint32_t dark = shade(base, 0.55f);

    for (int row = 0; row < ICON_SIZE; ++row) {
        for (int col = 0; col < ICON_SIZE; ++col) {
            uint32_t colour;
            switch (art.rows[row][col]) {
                case '#': colour = base;  break;
                case '+': colour = light; break;
                case '-': colour = dark;  break;
                default: continue;
            }
            const SDL_Rect px{x + col * scale, y + row * scale, scale, scale};
            fill(renderer, px, colour);
        }
    }
}

void draw_hotbar(SDL_Renderer* renderer, int window_w, int ui_scale, int selected) {
    const int px = icon_scale(ui_scale);
    const int slot = slot_size(ui_scale);
    const int gap = slot_gap(ui_scale);
    const int row_w = HOTBAR_COUNT * slot + (HOTBAR_COUNT - 1) * gap;

    // Top right. The readout owns top-left and the reticle is at the mouse, so
    // this is the one corner with nothing already on it. Top-centre does not
    // work: the readout's first line reaches far enough across that the two
    // draw calls would overlap on an ordinary HUD line.
    const int margin = 6 * px;
    const int x0 = window_w - margin - row_w;
    const int y0 = margin;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < HOTBAR_COUNT; ++i) {
        const int x = x0 + i * (slot + gap);
        const bool active = (i == selected);

        // A backing per slot rather than one panel behind the row: the selected
        // slot has to be findable in peripheral vision, and a brightness step
        // between neighbouring boxes reads faster than a border does.
        fill(renderer, SDL_Rect{x, y0, slot, slot}, active ? 0xE0303030u : 0xB4101010u);

        if (active) {
            // A frame in the material's own colour, so what is selected is stated
            // twice -- by the box and by the icon inside it.
            const uint32_t frame = shade(icon_base(HOTBAR[i].type), 1.45f);
            fill(renderer, SDL_Rect{x, y0, slot, px}, frame);
            fill(renderer, SDL_Rect{x, y0 + slot - px, slot, px}, frame);
            fill(renderer, SDL_Rect{x, y0, px, slot}, frame);
            fill(renderer, SDL_Rect{x + slot - px, y0, px, slot}, frame);
        }

        draw_icon(renderer, x + px, y0 + px, px, HOTBAR[i].type);

        const std::string label(1, HOTBAR[i].label);
        const int label_x = x + (slot - text_width(label, ui_scale)) / 2;
        draw_text(renderer, label_x, y0 + slot + label_gap(ui_scale), ui_scale, label,
                  active ? 0xFFFFFFFFu : 0xFF909090u);
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

} // namespace ui
