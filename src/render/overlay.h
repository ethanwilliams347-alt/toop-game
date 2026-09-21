#pragma once
#include <SDL.h>
#include <cstdint>
#include <string>
#include <vector>
#include "game/display.h"

// The screen-space layer of one frame: the reticle, the HUD stack, the hotbar,
// the run-over wash and the settings screen, in the order they are drawn.
//
// This is not frame.cpp and must not become part of it. frame.h's rule -- "UI is
// not here and does not become here" -- is about the light pass, not about which
// file the code sits in: everything in frame.cpp is in the world and gets lit,
// and a reticle that goes orange near a flame is the failure that prevents. That
// is why this is a second translation unit with its own entry point, called by
// main.cpp after frame::compose returns, rather than a run of rows appended to
// the layer table. The composition never learns these exist.
//
// Behind one function taking one struct, they are also inside golden_frame_test,
// where questions like "does the HUD backing still cover the text" have a
// checksum for an answer instead of a window.
namespace overlay {

// One line of the HUD stack under the main readout, with its colour. A vector
// rather than a fixed set of optional fields because several of them are
// conditional, and the point of the cursor this draws with is that the lines do
// not know their own y -- see the note at `draw` in overlay.cpp.
struct Line {
    std::string text;
    uint32_t colour = 0xFFE0E0E0;
};

// Everything the screen-space layer reads, and nothing else.
//
// The same seam frame::Params draws: every field is something the caller already
// had, there is no state here, and nothing in this struct knows about a Run, a
// Grid or a window. The HUD's strings arrive already built -- what the readout
// says is a decision about simulation state and belongs to the caller; where it
// lands on the screen is what this file owns.
struct Params {
    // The window, in screen pixels, and the UI scale that goes with it.
    int window_w = 0;
    int window_h = 0;
    int ui_scale = 1;

    // The active scene's screen pixels per world cell, which the settings menu
    // needs in order to say how many cells each resolution shows.
    //
    // Passed in rather than read off Camera::DEFAULT_SCALE, because the answer
    // depends on the scene: the same window size means different cell counts at
    // different scales, and a menu quoting the default would be wrong in exactly
    // the scenes that state their own.
    int view_scale = 4;

    // The reticle. `show` is false when the pointer is not over this window --
    // SDL_GetMouseState keeps reporting the last position inside it after the mouse
    // leaves, and the caller is the only one that can tell.
    bool show_reticle = false;
    int mouse_x = 0;
    int mouse_y = 0;
    bool in_range = false;

    // The main readout, then the stack under it in draw order.
    std::string hud_text;
    std::vector<Line> hud_lines;

    // Index into ui::HOTBAR, or -1 for no highlight.
    int hotbar_selected = -1;

    // The run's ending. `won` picks the headline and its colour.
    bool run_over = false;
    bool won = false;

    // The settings screen. `modes`/`available` are arrays of `mode_count`, and
    // `cursor` may be `mode_count` itself, which is the Quit row -- the same index
    // menu::quit_index returns, passed rather than recomputed so the drawing and the
    // state machine cannot disagree about which row is which.
    bool settings_open = false;
    int cursor = 0;
    int mode_count = 0;
    int current_mode = -1;
    const DisplayMode* modes = nullptr;
    const bool* available = nullptr;
    std::string notice;  // empty draws nothing
};

// Draws the screen-space layer over whatever is already in the framebuffer.
// Presents nothing, and leaves the draw colour and blend mode as it found them.
void draw(SDL_Renderer* renderer, const Params& p);

} // namespace overlay
