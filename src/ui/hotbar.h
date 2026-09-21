#pragma once
#include "physics/material.h"
#include <SDL.h>

// The eight materials behind the eight number keys, and their icons.
//
// The table is here rather than in main.cpp so the keydown handler and the
// renderer read the same list. Two lists that have to agree about which material
// key 6 places fail silently, and an icon that lies about what the key does is
// worse than no icon.
//
// SDL-side like text.h and for the same reason: there is nothing to assert
// headlessly about a pixel shape a human is going to look at.
namespace ui {

// A hand-authored icon is 8x8. Big enough for a recognisable silhouette at the
// glyph font's sensibility, small enough that a row of them is still forty-odd
// bytes of ASCII art each.
constexpr int ICON_SIZE = 8;

struct HotbarSlot {
    SDL_Keycode key;
    ElementType type;
    char label;  // what is printed under the slot - the key, in one character
};

// Order is the order they are drawn, left to right, and the order the keys sit
// in on the keyboard.
//
// Grouped by MoveKind, heaviest behaviour first: the two Static structural
// materials, then the Powder, then the two Liquids, then the two Gases, with the
// eraser last. That puts the things you build with under the fingers that rest
// on 1 and 2, and neighbouring keys behave alike, so a mis-hit lands on
// something that does roughly what you meant. The eraser is last because it is
// not a material at all.
inline constexpr HotbarSlot HOTBAR[] = {
    {SDLK_1, ElementType::Wood,  '1'},
    {SDLK_2, ElementType::Wall,  '2'},
    {SDLK_3, ElementType::Sand,  '3'},
    {SDLK_4, ElementType::Water, '4'},
    {SDLK_5, ElementType::Oil,   '5'},
    {SDLK_6, ElementType::Steam, '6'},
    {SDLK_7, ElementType::Fire,  '7'},
    {SDLK_8, ElementType::Empty, '8'},
};
inline constexpr int HOTBAR_COUNT = static_cast<int>(sizeof(HOTBAR) / sizeof(HOTBAR[0]));

// Draws one material's icon with its top-left at (x, y), each authored pixel
// `scale` screen pixels square. Colours come from MATERIALS, so an icon cannot
// drift from the palette it depicts -- the art here is the shape only.
void draw_icon(SDL_Renderer* renderer, int x, int y, int scale, ElementType type);

// Draws the whole row anchored to the top-right corner of a `window_w`-wide
// window, inset from both edges by a margin derived from `ui_scale`. Only the
// width is needed: the row hangs from the top, so the window's height never
// enters the placement. `selected` is an index into HOTBAR; anything out of
// range simply draws no highlight.
//
// `ui_scale` is DisplayMode::ui_scale(), not a constant: on a large display a
// hotbar authored in fixed pixels is a postage stamp, and this one has to be
// legible at a glance mid-dig or it is not an affordance at all.
void draw_hotbar(SDL_Renderer* renderer, int window_w, int ui_scale, int selected);

} // namespace ui
