#pragma once
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#include "camera.h"

// The window: how big it is, which of the three ways it is shown, and the one
// place that knows how a window size turns into a viewport. Also settings.txt,
// which is where all of that is remembered between runs.
//
// A mode switch changes how much world is on screen, never how big a cell is.
// The scale is the art direction, and the player body is sized to match it. That
// has a consequence worth stating rather than discovering: a player on an
// ultrawide genuinely sees more of the world than one at 1920x1080. Holding the
// framing constant across modes instead needs a fractional scale at the smaller
// sizes, which destroys the pixel grid everything in assets/ is drawn on.
//
// Every size here is in physical pixels. main.cpp makes the process
// per-monitor DPI aware before SDL starts, so on a desktop scaled to 125% or
// 150% a 3440x1440 window is 3440x1440 pixels, not a 2752x1152 one that Windows
// stretches -- which would blur the pixel grid in exactly the way a fractional
// scale would.
//
// SDL-free, like the rest of game/: the decisions are here, with suites
// (boot_test for the start-up choice and the file, shell_test for the menu),
// and shell/shell.cpp asks the display the questions they are decided from.

// A window size, decided at run time: by the settings, by what the display
// offers, or by the window system when it resizes the window under the game.
// The render targets are rebuilt to it whenever it moves.
//
// The scale is a default rather than a constant, which is why every method here
// takes it as an argument instead of reading Camera::DEFAULT_SCALE: a scene may state
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
    //
    // Never below 1. Every size the menu offers is at least 720 tall, but the
    // window system can hand the game a smaller one, and a scale of 0 draws no
    // text at all -- including the menu that would put it right.
    constexpr int ui_scale() const { return std::max(1, window_h / 270); }

    constexpr bool operator==(const DisplayMode&) const = default;
};

// The windowed sizes the menu offers, smallest first, and the order the
// resolution row steps through them. A window can be any size, but a list of
// the sizes monitors and their halves actually come in is a list a player can
// recognise, which a free width and height are not.
//
// Filtered at run time by what fits the usable area of the display the window
// is on (shell::screen_options), so the list is a statement about this desktop.
// Fullscreen sizes are not from here: they are whatever modes the display
// reports. Borderless has no list at all -- it is the desktop's size.
inline constexpr DisplayMode COMMON_SIZES[] = {
    {1280, 720},  {1366, 768},  {1600, 900},  {1920, 1080},
    {2560, 1080}, {2560, 1440}, {3440, 1440}, {3840, 2160},
};
inline constexpr int COMMON_SIZE_COUNT =
    static_cast<int>(sizeof(COMMON_SIZES) / sizeof(COMMON_SIZES[0]));

// The widest view the shipped scenes are held to: backdrop_set_test requires
// every backdrop to cover it at its scene's scale (344 cells across at scale 10),
// and grid_bench's light rows measure a field this size. 21:9 at 1440 rows is
// the widest common monitor. 3840x2160 is offered too, and works -- a scene
// smaller than that view is drawn at the window's top left with the clear colour
// past its edges, as any scene smaller than its window is -- but it is not a
// size the art is painted to fill, and holding the art to it would mean
// repainting every 344-wide scene for one display class.
inline constexpr DisplayMode WIDEST_VIEW{3440, 1440};
static_assert(std::find(std::begin(COMMON_SIZES), std::end(COMMON_SIZES), WIDEST_VIEW) !=
                  std::end(COMMON_SIZES),
              "the widest view the art is held to has to be a size the menu offers");

// Where the settings are remembered between runs. Beside the executable, in the
// working directory the assets are already loaded relative to.
inline constexpr const char* SETTINGS_PATH = "settings.txt";

// The three ways the window can be shown.
//
//   Windowed    a window with a title bar, at one of COMMON_SIZES that fits.
//   Borderless  a borderless window covering the display at the desktop's own
//               size and refresh. The mode to play an ultrawide at: no mode
//               switch, alt-tab is instant, and it is pixel for pixel.
//   Fullscreen  exclusive: the display is switched to the chosen mode, at that
//               size's highest refresh rate. For running below the desktop's
//               resolution on a machine that needs fewer pixels.
enum class WindowMode { Windowed, Borderless, Fullscreen };

// Which gfx::Canvas backend draws the game: SDL_Renderer or OpenGL 3.3 core
// (src/gpu/). Chosen in the settings menu, which writes renderer= -- it takes
// effect on the next launch, because switching backends means remaking every
// texture the game holds -- or overridden for one run with --renderer=gl|sdl.
//
// SDL stays the default while the GL backend is a parity backend: it draws the
// same frame, so there is nothing yet to choose it for, and an untested driver is
// a reason to opt in rather than out. GL falls back to SDL at startup on any
// failure (no 3.3 context, a missing entry point, a shader that will not
// compile), so asking for it never costs a session.
enum class RendererChoice { Sdl, Gl };

namespace display_detail {

// Whether `value` is exactly `word`, ignoring a line ending and trailing spaces.
// Every value in the file is one word, and every reader below is "is it this
// word", so they share this rather than each trimming its own way.
inline bool value_is(const char* value, const char* word) {
    size_t n = std::strlen(value);
    while (n > 0 && (value[n - 1] == '\n' || value[n - 1] == '\r' || value[n - 1] == ' ')) --n;
    return n == std::strlen(word) && std::strncmp(value, word, n) == 0;
}

}  // namespace display_detail

// One value, "gl" or "sdl", with or without a line ending. Anything else is no
// answer and leaves `out` as it was.
inline bool parse_renderer_choice(const char* value, RendererChoice& out) {
    if (display_detail::value_is(value, "gl")) {
        out = RendererChoice::Gl;
        return true;
    }
    if (display_detail::value_is(value, "sdl")) {
        out = RendererChoice::Sdl;
        return true;
    }
    return false;
}

// The file's spelling of a window mode, and back.
inline const char* window_mode_name(WindowMode m) {
    switch (m) {
        case WindowMode::Windowed:   return "windowed";
        case WindowMode::Borderless: return "borderless";
        case WindowMode::Fullscreen: return "fullscreen";
    }
    return "windowed";
}

inline bool parse_window_mode(const char* value, WindowMode& out) {
    for (WindowMode m : {WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen}) {
        if (display_detail::value_is(value, window_mode_name(m))) {
            out = m;
            return true;
        }
    }
    return false;
}

// Everything settings.txt holds, and everything the settings menu changes.
//
// `size` is the window's size in the mode it goes with: the windowed size, the
// fullscreen mode's size, or -- borderless -- the desktop's. 0x0 means none was
// stored, and the start-up choice picks one.
struct DisplaySettings {
    WindowMode window = WindowMode::Windowed;
    DisplayMode size{0, 0};
    // On by default, as it was before it could be turned off: an unpaced frame
    // loop spends a whole core and the GPU on frames the display never shows.
    bool vsync = true;
    RendererChoice renderer = RendererChoice::Sdl;

    bool operator==(const DisplaySettings&) const = default;
};

// Reads settings.txt. Missing keys, an unreadable value, or no file at all
// leave the defaults above; there is no error to report, because there is
// nothing a player could do about one at launch that the menu cannot do better.
//
// The format is one `key=value` per line:
//
//   display=windowed|borderless|fullscreen
//   resolution=<W>x<H>
//   vsync=on|off
//   renderer=sdl|gl
//
// A file from before display= and vsync= existed holds resolution= alone (and
// perhaps renderer=), and loads as the windowed size it always meant.
//
// Stores the resolution, not an index into a list. A list is expected to grow
// and differs between displays; "2560x1440" cannot change meaning.
inline DisplaySettings load_settings(const char* path = SETTINGS_PATH) {
    DisplaySettings s;
    std::FILE* f = std::fopen(path, "r");
    if (!f) return s;
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        int w = 0, h = 0;
        // A misread number is caught by the w > 0 && h > 0 below and the line
        // counts as no setting -- the same answer a reported conversion error
        // would give, since a size is only ever looked up in a list of sizes.
        // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion)
        if (std::sscanf(line, "resolution=%dx%d", &w, &h) == 2) {
            if (w > 0 && h > 0) s.size = DisplayMode{w, h};
        } else if (std::strncmp(line, "display=", 8) == 0) {
            parse_window_mode(line + 8, s.window);
        } else if (std::strncmp(line, "vsync=", 6) == 0) {
            if (display_detail::value_is(line + 6, "on")) s.vsync = true;
            if (display_detail::value_is(line + 6, "off")) s.vsync = false;
        } else if (std::strncmp(line, "renderer=", 9) == 0) {
            parse_renderer_choice(line + 9, s.renderer);
        }
    }
    std::fclose(f);
    return s;
}

// Writes settings.txt. Failure is reported to the caller rather than swallowed:
// a setting that silently does not persist is worse than one that says it could
// not, because the player's only evidence either way is what happens on the
// next launch.
//
// Every line already in the file that is not one of these four keys is kept,
// in order, so a key some later feature adds by hand survives a trip through
// the menu. Comment and blank lines are dropped and the header rewritten.
inline bool save_settings(const DisplaySettings& s, const char* path = SETTINGS_PATH) {
    std::vector<std::string> keep;
    if (std::FILE* in = std::fopen(path, "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), in)) {
            if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
            if (std::strncmp(line, "display=", 8) == 0 ||
                std::strncmp(line, "resolution=", 11) == 0 ||
                std::strncmp(line, "vsync=", 6) == 0 || std::strncmp(line, "renderer=", 9) == 0)
                continue;
            keep.emplace_back(line);
            if (keep.back().back() != '\n') keep.back() += '\n';
        }
        std::fclose(in);
    }
    std::FILE* f = std::fopen(path, "w");
    if (!f) return false;
    std::fprintf(f, "# SLOP Pixel Physics settings. Rewritten whenever a setting changes.\n");
    std::fprintf(f, "display=%s\n", window_mode_name(s.window));
    if (s.size.window_w > 0 && s.size.window_h > 0)
        std::fprintf(f, "resolution=%dx%d\n", s.size.window_w, s.size.window_h);
    std::fprintf(f, "vsync=%s\n", s.vsync ? "on" : "off");
    std::fprintf(f, "renderer=%s\n", s.renderer == RendererChoice::Gl ? "gl" : "sdl");
    for (const std::string& l : keep) std::fputs(l.c_str(), f);
    return std::fclose(f) == 0;
}

// Which of a list of windowed sizes to open at, given which of them fit this
// desktop and which one the last session stored (an index into the list, or -1).
// Pure, and separated from the SDL that answers "does it fit": the fitting test
// needs a display and this decision does not, and it is the decision that has
// the wrong answers in it.
//
// The stored setting wins if it is still valid and still fits -- a monitor can
// change between runs, and a saved oversized window would otherwise open with
// its title bar, and the settings menu inside it, off the edge of the desktop.
// Failing that, the largest that fits, which shows the most world. And if
// nothing fits, the smallest anyway rather than refusing to start: an oversized
// window is a bad session, and no window at all is no session.
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

// Where `size` is in COMMON_SIZES, or -1.
inline int common_size_index(DisplayMode size) {
    for (int i = 0; i < COMMON_SIZE_COUNT; ++i)
        if (COMMON_SIZES[i] == size) return i;
    return -1;
}

// One exclusive-fullscreen mode: a size and the refresh rate it is entered at.
struct FullscreenMode {
    DisplayMode size;
    int refresh_hz = 0;
};

// The display's modes as the menu offers them: one entry per size, at that
// size's highest refresh rate, smallest first by width then height.
//
// A display reports each size several times over -- per refresh rate, per pixel
// format -- and a list with 1920x1080 in it six times is six rows that look the
// same and are not. The highest refresh is the one anybody choosing a size for
// a game wants; somebody who wants 60 Hz on a 144 Hz panel has VSync and the
// driver's own controls.
//
// Sizes smaller than the smallest windowed size on either axis are dropped: the
// HUD, the hotbar and this menu are laid out for 1280x720 and up, and 640x480
// is a mode every display reports and nobody wants.
inline std::vector<FullscreenMode> fullscreen_sizes(const std::vector<FullscreenMode>& reported) {
    std::vector<FullscreenMode> out;
    for (const FullscreenMode& m : reported) {
        if (m.size.window_w < COMMON_SIZES[0].window_w ||
            m.size.window_h < COMMON_SIZES[0].window_h)
            continue;
        bool merged = false;
        for (FullscreenMode& o : out) {
            if (o.size == m.size) {
                o.refresh_hz = std::max(o.refresh_hz, m.refresh_hz);
                merged = true;
            }
        }
        if (!merged) out.push_back(m);
    }
    std::sort(out.begin(), out.end(), [](const FullscreenMode& a, const FullscreenMode& b) {
        return a.size.window_w != b.size.window_w ? a.size.window_w < b.size.window_w
                                                  : a.size.window_h < b.size.window_h;
    });
    return out;
}

// What the display the window is on offers, asked by shell::screen_options and
// decided from here. Gathered again each time the menu opens, because the
// window can be dragged to another display between two looks at it.
struct ScreenOptions {
    // The display's own size, which is what borderless is.
    DisplayMode desktop{0, 0};
    // Per COMMON_SIZES entry, whether a window that size, title bar included,
    // fits the display's usable area (the desktop minus the taskbar).
    std::array<bool, COMMON_SIZE_COUNT> windowed_fits{};
    // fullscreen_sizes() of the display's modes. Empty when the driver lists
    // none, in which case the desktop's own size is the one fullscreen offers.
    std::vector<FullscreenMode> fullscreen;
};

// The sizes the resolution row steps through in `mode`, smallest first.
//
// Windowed: the common sizes that fit, or the smallest alone when none does --
// the same answer choose_display_mode gives, so the menu never offers an empty
// row. Borderless: the desktop, alone. Fullscreen: the display's modes, or the
// desktop when it reports none.
inline std::vector<DisplayMode> resolutions(WindowMode mode, const ScreenOptions& o) {
    std::vector<DisplayMode> out;
    switch (mode) {
        case WindowMode::Windowed:
            for (int i = 0; i < COMMON_SIZE_COUNT; ++i)
                if (o.windowed_fits[static_cast<size_t>(i)]) out.push_back(COMMON_SIZES[i]);
            if (out.empty()) out.push_back(COMMON_SIZES[0]);
            break;
        case WindowMode::Borderless: out.push_back(o.desktop); break;
        case WindowMode::Fullscreen:
            for (const FullscreenMode& m : o.fullscreen) out.push_back(m.size);
            if (out.empty()) out.push_back(o.desktop);
            break;
    }
    return out;
}

// The size a switch into `mode` lands on, starting from `wanted`: `wanted`
// itself when the mode offers it; otherwise the largest size the mode offers
// that is no larger on either axis, so a switch never grows the window past
// what was asked for; otherwise the smallest it offers. Borderless is always
// the desktop.
//
// Used when the display row moves, so Windowed 1920x1080 -> Fullscreen keeps
// 1920x1080 when the display has that mode, and Borderless -> Windowed on a
// 3440x1440 desktop lands on the largest window that fits rather than on a
// size that does not.
inline DisplayMode fit_size(WindowMode mode, DisplayMode wanted, const ScreenOptions& o) {
    if (mode == WindowMode::Borderless) return o.desktop;
    const std::vector<DisplayMode> list = resolutions(mode, o);
    DisplayMode best{0, 0};
    for (const DisplayMode& m : list) {
        if (m == wanted) return m;
        if (m.window_w <= wanted.window_w && m.window_h <= wanted.window_h &&
            m.window_w * m.window_h > best.window_w * best.window_h)
            best = m;
    }
    return best.window_w > 0 ? best : list.front();
}

// The settings the game opens with: the stored ones, with the size made one
// this display can show.
struct StartChoice {
    DisplaySettings settings;
    // Each value other than Stored is a line the caller prints, for the reason
    // ModeChoice's are.
    enum class Why {
        Stored,         // as stored (or nothing stored that needed a fallback)
        Largest,        // windowed, no usable size stored: the largest that fits
        StoredTooBig,   // windowed, the stored size no longer fits: the largest that does
        NothingFits,    // windowed, nothing fits this desktop: the smallest anyway
        NotAMode,       // fullscreen, the stored size is not a mode of this display
        DesktopWindow,  // windowed at exactly the desktop's size: opened borderless
    } why = Why::Stored;
};

inline StartChoice choose_start(const DisplaySettings& stored, const ScreenOptions& o) {
    StartChoice c;
    c.settings = stored;
    switch (stored.window) {
        case WindowMode::Windowed: {
            // A window exactly the desktop's size cannot be shown with its title
            // bar, ever, so whatever stored it -- a settings.txt from before
            // borderless existed, when a desktop-sized window was the only way to
            // fill an ultrawide, or a hand edit -- meant the screen filled, which
            // is what borderless is. Falling to the largest window instead would
            // answer a player who had the whole screen with a smaller window.
            if (stored.size.window_w > 0 && stored.size == o.desktop) {
                c.settings.window = WindowMode::Borderless;
                c.why = StartChoice::Why::DesktopWindow;
                break;
            }
            // The windowed choice is choose_display_mode's, over the common
            // sizes, so its four answers and their tests carry over unchanged.
            const ModeChoice m = choose_display_mode(o.windowed_fits.data(), COMMON_SIZE_COUNT,
                                                     common_size_index(stored.size));
            c.settings.size = COMMON_SIZES[m.index];
            switch (m.why) {
                case ModeChoice::Why::Stored:       c.why = StartChoice::Why::Stored; break;
                case ModeChoice::Why::StoredTooBig: c.why = StartChoice::Why::StoredTooBig; break;
                case ModeChoice::Why::Largest:      c.why = StartChoice::Why::Largest; break;
                case ModeChoice::Why::NothingFits:  c.why = StartChoice::Why::NothingFits; break;
            }
            break;
        }
        case WindowMode::Borderless: c.settings.size = o.desktop; break;
        case WindowMode::Fullscreen: {
            // A stored size the display has no mode for -- a different monitor
            // than last time -- opens at the desktop's size, which is the one mode
            // every display is known to be able to show.
            const std::vector<DisplayMode> list = resolutions(WindowMode::Fullscreen, o);
            if (std::find(list.begin(), list.end(), stored.size) != list.end()) break;
            const bool has_desktop = std::find(list.begin(), list.end(), o.desktop) != list.end();
            c.settings.size =
                has_desktop ? o.desktop : fit_size(WindowMode::Fullscreen, o.desktop, o);
            if (stored.size.window_w > 0) c.why = StartChoice::Why::NotAMode;
            break;
        }
    }
    return c;
}
