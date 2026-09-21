#pragma once
#include "camera.h"
#include <cstdio>

// The window sizes the game can be run at, and the one place that knows how a
// window size turns into a viewport.
//
// A mode switch changes how much world is on screen, never how big a cell is.
// The scale is the art direction, and the player body is sized to match it. That
// has a consequence worth stating rather than discovering: a player on an
// ultrawide genuinely sees more of the world than one at 1920x1080. Holding the
// framing constant across modes instead needs a fractional scale at the smaller
// sizes, which destroys the pixel grid everything in assets/ is drawn on.
//
// The scale is a default rather than a constant, which is why every method here
// takes it as an argument instead of reading Camera::SCALE: a scene may state
// its own, so the pair (window, scale) decides a viewport and neither half
// decides it alone. The parameter is deliberately not defaulted -- a default
// would let a call site silently frame a scene at the wrong scale, and there are
// many such sites in main.cpp.
struct DisplayMode {
    int window_w;
    int window_h;

    // How many whole cells fit on screen.
    constexpr int viewport_w(int scale) const { return window_w / scale; }
    constexpr int viewport_h(int scale) const { return window_h / scale; }

    // One extra cell on each axis, uploaded and drawn but never quite fully on
    // screen. The camera scrolls in fractions of a cell, so the world texture is
    // drawn shifted by up to one cell left and up, which uncovers a sliver along the
    // right and bottom edges that this margin fills. Everything downstream uses the
    // padded size; the camera is told its viewport is the padded size too, so its
    // clamp keeps the upload inside the grid.
    constexpr int padded_w(int scale) const { return viewport_w(scale) + 1; }
    constexpr int padded_h(int scale) const { return viewport_h(scale) + 1; }

    // Pixels per glyph cell for the HUD and the settings menu.
    //
    // Deliberately not fixed, and deliberately unlike the world's scale. The world
    // is locked at the scene's scale because a cell is art; UI text is not art, it
    // is something to read, and what makes it readable is how much of the window it
    // covers. Derived from the height so it holds a constant fraction of the screen.
    // The reticle is pointedly not scaled by this -- it is a cursor, and it belongs
    // to the mouse and the cell grid rather than to the UI.
    constexpr int ui_scale() const { return window_h / 270; }
};

// Offered in the settings menu in this order. Growing this list is the whole
// cost of adding a mode -- everything downstream reads the table.
inline constexpr DisplayMode DISPLAY_MODES[] = {
    {1920, 1080},
    {2560, 1440},
    {3440, 1440},
};
inline constexpr int DISPLAY_MODE_COUNT =
    static_cast<int>(sizeof(DISPLAY_MODES) / sizeof(DISPLAY_MODES[0]));

// Where the chosen mode is remembered between runs. Beside the executable, in
// the working directory the assets are already loaded relative to.
inline constexpr const char* SETTINGS_PATH = "settings.txt";

// Reads the stored mode and returns its index in DISPLAY_MODES, or -1 if there
// is nothing usable to read -- no file, an unparseable line, or a resolution
// that is no longer in the table.
//
// Stores the resolution, not the index. An index is a reference into a list this
// file is expected to grow, and inserting a mode at the top would silently move
// every player already using one. "2560x1440" cannot change meaning.
inline int load_display_mode(const char* path = SETTINGS_PATH) {
    std::FILE* f = std::fopen(path, "r");
    if (!f) return -1;

    int found = -1;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        int w = 0, h = 0;
        if (std::sscanf(line, "resolution=%dx%d", &w, &h) == 2) {
            for (int i = 0; i < DISPLAY_MODE_COUNT; ++i) {
                if (DISPLAY_MODES[i].window_w == w && DISPLAY_MODES[i].window_h == h) found = i;
            }
        }
    }
    std::fclose(f);
    return found;
}

// Which mode to open at, given which of them fit this desktop and what the last
// session stored. Pure, and separated from the SDL that answers "does it fit":
// the fitting test needs a display and this decision does not, and it is the
// decision that has the wrong answers in it.
//
// The stored setting wins if it is still valid and still fits -- a monitor can
// change between runs, and a saved oversized mode would otherwise open a window
// nobody can reach the settings menu inside. Failing that, the largest mode that
// fits, which shows the most world. And if nothing fits, the smallest anyway
// rather than refusing to start: an oversized window is a bad session, and no
// window at all is no session.
struct ModeChoice {
    int index = 0;
    // Each of these is a line the caller prints, and each names a path that is
    // invisible from the window alone -- "it opened smaller than I asked for" is not
    // something a player can debug by looking.
    enum class Why {
        Stored,        // the stored mode was valid and fits
        StoredTooBig,  // a stored mode was found and does not fit; ignored
        Largest,       // nothing stored; the largest fitting mode
        NothingFits,   // every mode is larger than the desktop
    } why = Why::Largest;
};

inline ModeChoice choose_display_mode(const bool* fits, int count, int stored) {
    ModeChoice choice;
    choice.index = -1;
    for (int i = 0; i < count; ++i)
        if (fits[i]) choice.index = i;

    if (choice.index < 0) {
        choice.index = 0;
        choice.why = ModeChoice::Why::NothingFits;
        // A stored mode cannot be honoured when nothing fits, and saying "ignoring
        // your setting" on top of "nothing fits at all" would bury the sharper of
        // the two lines under the vaguer one.
        return choice;
    }

    if (stored >= 0 && stored < count) {
        if (fits[stored]) {
            choice.index = stored;
            choice.why = ModeChoice::Why::Stored;
        } else {
            choice.why = ModeChoice::Why::StoredTooBig;
        }
    }
    return choice;
}

// Writes the chosen mode. Failure is reported to the caller rather than
// swallowed: a setting that silently does not persist is worse than one that
// says it could not, because the player's only evidence either way is what
// happens on the next launch.
inline bool save_display_mode(const DisplayMode& mode, const char* path = SETTINGS_PATH) {
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f, "# SLOP Pixel Physics settings. Rewritten whenever a setting changes.\n");
    std::fprintf(f, "resolution=%dx%d\n", mode.window_w, mode.window_h);
    return std::fclose(f) == 0;
}
