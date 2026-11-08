#pragma once
#include <string>
#include <utility>
#include <vector>
#include "display.h"

// The settings menu's navigation, its values and its apply-then-confirm round
// trip, as a state machine with no window attached.
//
// SDL-free and header-only, for debug_view.h's reasons: a decision that only
// exists inside an SDL event switch is a decision no suite can reach, and this
// is the largest one in the game -- what Enter does with nothing changed, what a
// held Enter does to a mode switch nobody has looked at yet, what happens when
// the switch fails, when the countdown runs out, when the save does.
//
// What stays in main.cpp is the half a test could not check anyway: the
// keysyms, and the calls that need a window and a filesystem (shell::apply_window,
// rebuilding the render targets, save_settings). This returns an Outcome saying
// what to attempt, and the caller reports back through applied() and saved().
// That round trip is the only shape in which "try the switch, and if it fails
// say so and change nothing" is expressible without a renderer in here.
//
// The rows hold pending values. Left and right change the row's value without
// touching the window; Enter applies everything pending at once. A switch of
// window mode or size then has to be kept: the menu asks, and goes back on its
// own after CONFIRM_SECONDS if nobody answers. That is the one way out of a mode
// the monitor cannot show -- a black screen has no menu on it to navigate, and
// waiting is the one thing a player who cannot see can still do.
namespace menu {

// The menu's alphabet, not SDL's. main.cpp maps keysyms onto these, which is
// where the fact that both W and Up move the cursor belongs: it is a binding,
// and bindings are the thing this file is deliberately ignorant of.
enum class Key { Prev, Next, Left, Right, Confirm, Back };

// The rows, top to bottom.
enum class Row { Display, Resolution, VSync, Renderer, Apply, Quit };
inline constexpr int ROW_COUNT = 6;

enum class Act {
    None,   // handled; stay on the menu
    Close,  // back to play
    Quit,   // end the session
    Apply,  // caller: switch to `settings`, then report with applied()
    Save,   // caller: write `settings` to settings.txt, then report with saved()
};

// A notice is shown under the menu for a few seconds after something is
// attempted, so a refused switch says so instead of looking like a dead key. The
// text is here rather than at the call site because which refusal happened is
// exactly what this decides, and a caller free to pick its own wording is a
// caller that can describe a state that did not occur.
//
// Written without commas, apostrophes or question marks: ui::draw_text has no
// glyph for them and would leave a gap.
struct Outcome {
    Act act = Act::None;
    DisplaySettings settings{};  // meaningful for Apply and Save
    const char* notice = nullptr;  // null when there is nothing to say
    double notice_seconds = 0.0;
};

// How long a new window mode or size stands before it is undone unasked.
// Ten seconds is the convention players already know from their own desktop's
// display settings, and long enough for a monitor to finish resyncing -- a few
// seconds of black on some panels -- and leave time to read the question.
inline constexpr double CONFIRM_SECONDS = 10.0;

struct State {
    int cursor = 0;
    // What the window is now, and what the rows show. They differ while
    // something is changed and not yet applied.
    DisplaySettings applied;
    DisplaySettings pending;

    // A switch waiting to be kept: the countdown, and what it goes back to.
    bool confirming = false;
    double confirm_left = 0.0;
    DisplaySettings fallback;

    // Which Apply the caller is carrying out, so applied() knows whether a
    // success is a change to confirm or the end of a revert. Without it a
    // successful revert -- itself a change of mode -- would ask to be confirmed.
    enum class Asked { Nothing, Change, Revert };
    Asked asked = Asked::Nothing;
};

// Whether a switch from `from` to `to` has to be kept before it is saved.
//
// Window mode and size, and nothing else: those are the changes that can leave
// a player looking at a monitor that shows nothing. VSync and the renderer
// cannot -- the first is invisible until it is wrong and the second does not
// take effect until the next launch -- so asking about them would be a question
// with no reason behind it.
inline bool needs_confirmation(const DisplaySettings& from, const DisplaySettings& to) {
    return from.window != to.window || from.size != to.size;
}

// Whether a held key's repeats count as presses.
//
// Moving the cursor and stepping a value repeat, because holding Down or Right
// to run through a list is how lists are used. Enter and ESC do not. A held
// Enter would apply a new mode and then, on its next repeat, keep it -- before
// the player has seen it, which is the one thing the confirmation exists to
// prevent. A held ESC would close the menu and then, its repeats landing on the
// play screen, open it again.
inline bool accepts_repeat(Key k) { return k != Key::Confirm && k != Key::Back; }

// Opening shows what is in effect: anything left pending from the last visit
// was abandoned by leaving, and showing it again would offer a change the
// player already walked away from. The cursor goes to the top row, because
// every value is on screen and the top is where a list is read from.
inline void open(State& s, const DisplaySettings& in_effect) {
    s.cursor = 0;
    s.applied = in_effect;
    s.pending = in_effect;
    s.confirming = false;
    s.confirm_left = 0.0;
    s.asked = State::Asked::Nothing;
}

namespace detail {

inline int cycle(int i, int n, int step) { return ((i + step) % n + n) % n; }

// Steps `size` through the list `mode` offers. A size not in the list -- the
// window system resized the window, or the display changed -- starts from where
// fit_size would put it.
inline DisplayMode step_size(WindowMode mode, DisplayMode size, int step, const ScreenOptions& o) {
    const DisplayMode on_list = fit_size(mode, size, o);
    if (on_list != size) return on_list;  // the first press lands on the list
    const std::vector<DisplayMode> list = resolutions(mode, o);
    int i = 0;
    for (int k = 0; k < static_cast<int>(list.size()); ++k)
        if (list[static_cast<size_t>(k)] == size) i = k;
    return list[static_cast<size_t>(cycle(i, static_cast<int>(list.size()), step))];
}

}  // namespace detail

// Left (-1) or right (+1) on the cursor's row. Every value wraps: the lists are
// short, and a value that stops at the end reads as a key that stopped working.
inline void change(State& s, int step, const ScreenOptions& o) {
    DisplaySettings& p = s.pending;
    switch (static_cast<Row>(s.cursor)) {
        case Row::Display: {
            p.window = static_cast<WindowMode>(detail::cycle(static_cast<int>(p.window), 3, step));
            // The size moves with the mode, to the nearest one the new mode offers
            // -- see fit_size. Without this, Borderless -> Windowed on an
            // ultrawide would carry the desktop's size into a window it cannot fit.
            p.size = fit_size(p.window, p.size, o);
            break;
        }
        case Row::Resolution:
            // Borderless has one size, the desktop's; stepping it is a no-op
            // rather than a refusal, and the row says DESKTOP so it is not a
            // mystery.
            if (p.window != WindowMode::Borderless)
                p.size = detail::step_size(p.window, p.size, step, o);
            break;
        case Row::VSync: p.vsync = !p.vsync; break;
        case Row::Renderer:
            p.renderer =
                p.renderer == RendererChoice::Sdl ? RendererChoice::Gl : RendererChoice::Sdl;
            break;
        case Row::Apply:
        case Row::Quit:  break;
    }
}

inline Outcome key(State& s, Key k, bool repeat, const ScreenOptions& o) {
    if (repeat && !accepts_repeat(k)) return Outcome{};

    // While a switch waits to be kept, the menu asks one question and takes two
    // answers. Every other key is ignored rather than acted on: moving the cursor
    // or changing a value under a question that is still on screen would leave
    // the answer meaning something different from what it meant when it was
    // asked.
    if (s.confirming) {
        if (s.asked != State::Asked::Nothing) return Outcome{};
        if (k == Key::Confirm) {
            s.confirming = false;
            return Outcome{Act::Save, s.applied};
        }
        if (k == Key::Back) {
            s.asked = State::Asked::Revert;
            return Outcome{Act::Apply, s.fallback};
        }
        return Outcome{};
    }

    switch (k) {
        case Key::Prev:  s.cursor = detail::cycle(s.cursor, ROW_COUNT, -1); return Outcome{};
        case Key::Next:  s.cursor = detail::cycle(s.cursor, ROW_COUNT, 1); return Outcome{};
        case Key::Left:  change(s, -1, o); return Outcome{};
        case Key::Right: change(s, 1, o); return Outcome{};
        case Key::Back:
            // ESC leaves the menu rather than quitting. A key that ends the session
            // without confirmation is the wrong key to leave next to a menu; quitting
            // is a row here, where it takes two deliberate presses. Anything pending
            // is dropped: leaving is how a player says no to it.
            return Outcome{Act::Close};
        case Key::Confirm:
            break;
    }

    if (static_cast<Row>(s.cursor) == Row::Quit) return Outcome{Act::Quit};

    // Enter applies what is pending, from any row; the Apply row is where a
    // player who does not know that looks for it. With nothing pending it is a
    // way out of the menu, not a no-op, which is what Enter meant on the old
    // menu's row with the marker on it.
    if (s.pending == s.applied) return Outcome{Act::Close};

    s.fallback = s.applied;
    s.asked = State::Asked::Change;
    return Outcome{Act::Apply, s.pending};
}

// The caller carried out an Apply; `ok` is whether the window and the render
// targets are now what was asked for. A failed apply has already been put back
// by the caller, so there is nothing to undo here, only to say.
inline Outcome applied(State& s, bool ok) {
    const State::Asked asked = s.asked;
    s.asked = State::Asked::Nothing;

    if (asked == State::Asked::Revert) {
        s.confirming = false;
        if (ok) {
            s.applied = s.fallback;
            s.pending = s.fallback;
            return Outcome{Act::None, {}, "Went back to the previous display mode.", 4.0};
        }
        // Still in the new mode, which the player can evidently see well enough
        // to have read this. It was never saved, so the next launch is the old
        // one either way.
        s.pending = s.applied;
        return Outcome{Act::None, {}, "Could not go back. This mode stays until you quit.", 6.0};
    }

    if (!ok) {
        // The cursor stays where it was; the rows go back to what is in effect,
        // so they and the window agree again.
        s.pending = s.applied;
        return Outcome{Act::None, {}, "Could not switch to that mode. Nothing changed.", 6.0};
    }

    const DisplaySettings was = s.applied;
    s.applied = s.pending;
    if (needs_confirmation(was, s.applied)) {
        // Not saved yet, and that is the point: a mode the monitor cannot show,
        // saved before anyone kept it, is the mode the next launch opens into
        // with no menu visible to change it.
        s.confirming = true;
        s.confirm_left = CONFIRM_SECONDS;
        s.fallback = was;
        return Outcome{};
    }
    return Outcome{Act::Save, s.applied};
}

// The caller wrote the settings; `ok` is whether settings.txt took them.
//
// The menu stays open either way. On success that is so the player can go on
// to the next row; on failure it is because the change did happen -- the
// player is looking at it -- and what failed is a promise about the next
// launch, which the screen still open is the only place to say.
inline Outcome saved(bool ok) {
    if (ok) return Outcome{Act::None, {}, "Saved.", 2.0};
    return Outcome{Act::None, {}, "Changed but settings.txt could not be written.", 6.0};
}

// The menu's clock, in real seconds. Runs the confirmation countdown and asks
// for the revert when it runs out.
inline Outcome tick(State& s, double seconds) {
    if (!s.confirming || s.asked != State::Asked::Nothing) return Outcome{};
    s.confirm_left -= seconds;
    if (s.confirm_left > 0.0) return Outcome{};
    s.asked = State::Asked::Revert;
    return Outcome{Act::Apply, s.fallback};
}

// --- what the screen says ------------------------------------------------
//
// The rows as text, for render/overlay.cpp to lay out. Decided here rather than
// there because each string is a claim about the state -- that a value is not
// applied yet, that the renderer waits for a restart, how many seconds are left
// -- and the suite that checks the state is the one that can check the claims.

struct RowView {
    std::string label;
    std::string value;  // empty on Apply and Quit
    std::string note;   // dim, after the value; may be empty
    bool selected = false;
    bool changed = false;     // pending, not yet applied
    bool adjustable = false;  // left and right do something here
    bool enabled = true;      // Apply with nothing to apply is not
};

struct View {
    std::vector<RowView> rows;
    // The key help, or while a switch waits to be kept, the question and its
    // countdown.
    std::string footer;
    bool confirming = false;
};

inline std::string size_text(DisplayMode m) {
    return std::to_string(m.window_w) + "X" + std::to_string(m.window_h);
}

// `running` is the renderer drawing this session, which can differ from the
// stored one: a --renderer override, or GL that fell back. `view_scale` is the
// scene's, so the cell counts are the ones the player would actually get.
inline View view(const State& s, const ScreenOptions& o, RendererChoice running, int view_scale) {
    View v;
    const DisplaySettings& p = s.pending;
    const DisplaySettings& a = s.applied;
    v.confirming = s.confirming;
    // Reserved, because row() hands back a pointer into the vector for a note to
    // be added through, and a reallocation would leave it dangling.
    v.rows.reserve(ROW_COUNT);

    auto row = [&](Row r, std::string label, std::string value, bool changed, bool adjustable) {
        RowView rv;
        rv.label = std::move(label);
        rv.value = std::move(value);
        rv.selected = s.cursor == static_cast<int>(r);
        rv.changed = changed;
        rv.adjustable = adjustable;
        v.rows.push_back(rv);
        return &v.rows.back();
    };

    static const char* const MODE_NAMES[] = {"WINDOWED", "BORDERLESS", "FULLSCREEN"};
    row(Row::Display, "DISPLAY", MODE_NAMES[static_cast<int>(p.window)], p.window != a.window,
        true);

    {
        const bool borderless = p.window == WindowMode::Borderless;
        RowView* r =
            row(Row::Resolution, "RESOLUTION", size_text(p.size), p.size != a.size, !borderless);
        r->note = std::to_string(p.size.viewport_w(view_scale)) + "X" +
                  std::to_string(p.size.viewport_h(view_scale)) + " CELLS";
        if (borderless) {
            r->note = "DESKTOP  " + r->note;
        } else if (p.window == WindowMode::Windowed) {
            // The windowed list stops at what fits, which on a 3440x1440 desktop
            // is well short of 3440x1440. Saying so at the top of the list is
            // the difference between "the game cannot do my resolution" and
            // "this mode cannot".
            const std::vector<DisplayMode> list = resolutions(WindowMode::Windowed, o);
            bool cut = false;
            for (int i = 0; i < COMMON_SIZE_COUNT; ++i)
                cut |= !o.windowed_fits[static_cast<size_t>(i)];
            if (cut && p.size == list.back()) r->note += "  LARGEST THAT FITS";
        }
    }

    row(Row::VSync, "VSYNC", p.vsync ? "ON" : "OFF", p.vsync != a.vsync, true);

    {
        RowView* r =
            row(Row::Renderer, "RENDERER", p.renderer == RendererChoice::Gl ? "OPENGL" : "SDL",
                p.renderer != a.renderer, true);
        // The renderer is never switched live -- every texture the game holds
        // belongs to the backend that made it -- so a value other than the one
        // drawing says when it will be.
        if (p.renderer != running) r->note = "ON RESTART";
    }

    row(Row::Apply, "APPLY", "", false, false)->enabled = p != a;
    row(Row::Quit, "QUIT", "", false, false);

    if (s.confirming) {
        // Whole seconds, rounded up, so the last second reads 1 rather than 0.
        int left = static_cast<int>(s.confirm_left);
        if (static_cast<double>(left) < s.confirm_left) ++left;
        v.footer = "ENTER KEEPS THIS MODE  ESC GOES BACK  REVERTING IN " + std::to_string(left);
    } else {
        v.footer = "ARROWS CHANGE  ENTER APPLIES  ESC RESUMES";
    }
    return v;
}

}  // namespace menu
