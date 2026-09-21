#pragma once

// The settings menu's navigation and selection, as a state machine with no
// window attached.
//
// SDL-free and header-only, for debug_view.h's reasons: a decision that only
// exists inside an SDL event switch is a decision no suite can reach, and this
// was the largest remaining example -- what a confirm does on a mode that does
// not fit, what happens to the cursor when a switch fails, whether a failed save
// still closes the screen.
//
// What stayed in main.cpp is the half a test could not check anyway: the
// keysyms, and the actual apply_mode / save_display_mode calls, which need a
// window and a filesystem. This returns an Outcome saying what to attempt, and
// the caller reports back through mode_applied or mode_refused. That round-trip
// is the only shape in which "try the switch, and if it fails say so and change
// nothing" is expressible without a renderer in here.
namespace menu {

// The menu's alphabet, not SDL's. main.cpp maps keysyms onto these, which is
// where the fact that both W and Up move the cursor belongs: it is a binding,
// and bindings are the thing this file is deliberately ignorant of.
enum class Key { Prev, Next, Confirm, Back };

enum class Act {
    None,  // handled; stay on the menu (a cursor move, or a refusal)
    Close,      // back to play
    Quit,       // end the session
    ApplyMode,  // caller must attempt the switch to `mode`, then report back
};

// A notice is shown under the menu for a few seconds after something is
// attempted, so a refused mode says so instead of looking like a dead key. The
// text is here rather than at the call site because which refusal happened is
// exactly what this decides, and a caller free to pick its own wording is a
// caller that can describe a state that did not occur.
struct Outcome {
    Act act = Act::None;
    int mode = -1;  // meaningful only for ApplyMode
    const char* notice = nullptr;  // null when there is nothing to say
    double notice_seconds = 0.0;
};

// Items are one per display mode, then Quit. The cursor indexes that combined
// list rather than the mode table, so navigation does not need to know the last
// row is special.
inline int quit_index(int mode_count) { return mode_count; }
inline int item_count(int mode_count) { return mode_count + 1; }

struct State {
    int cursor = 0;
};

// Opening always puts the cursor on the mode in use, rather than leaving it
// where it was last time. The menu is short and the current setting is the thing
// you came to look at.
inline void open(State& s, int current_mode) { s.cursor = current_mode; }

inline Outcome key(State& s, Key k, const bool* fits, int mode_count, int current_mode) {
    const int n = item_count(mode_count);
    switch (k) {
        case Key::Prev:
            // Wraps, and does so by adding n-1 rather than by subtracting and
            // testing, because the list is short enough that wrapping is how you
            // reach Quit from the top.
            s.cursor = (s.cursor + n - 1) % n;
            return Outcome{};
        case Key::Next:
            s.cursor = (s.cursor + 1) % n;
            return Outcome{};
        case Key::Back:
            // ESC leaves the menu rather than quitting. A key that ends the session
            // without confirmation is the wrong key to leave next to a menu; quitting
            // is an item here, where it takes two deliberate presses.
            return Outcome{Act::Close};
        case Key::Confirm:
            break;
    }

    if (s.cursor == quit_index(mode_count)) return Outcome{Act::Quit};

    if (s.cursor < 0 || s.cursor >= mode_count || !fits[s.cursor]) {
        // Refused, and it says so. A mode larger than the display is drawn in the
        // list rather than hidden, so the list is a statement about the game and
        // not about this machine -- which means confirming one has to be answered
        // with a sentence.
        return Outcome{Act::None, -1, "That mode is larger than this display.", 4.0};
    }

    // Confirming the mode already in use is a way out of the menu, not a no-op:
    // it is what the Enter key means on the row with the marker on it.
    if (s.cursor == current_mode) return Outcome{Act::Close};

    return Outcome{Act::ApplyMode, s.cursor};
}

// The switch succeeded. `saved` is whether the choice reached settings.txt.
//
// A failed save does not close the menu, and that asymmetry is the point. The
// mode did change -- the player is looking at the proof -- so there is nothing
// to roll back; what there is, is a promise about next launch that the game
// cannot keep, and the only place to say so is the screen that is still open.
inline Outcome mode_applied(State& s, int mode, bool saved) {
    s.cursor = mode;
    if (saved) return Outcome{Act::Close};
    return Outcome{Act::None, -1, "Mode changed, but settings.txt could not be written.", 6.0};
}

// The switch failed. The caller's apply_mode left everything as it was, so there
// is nothing to roll back here either, only to say. The cursor goes back to the
// mode actually in use, so the marker and the highlight agree again.
inline Outcome mode_refused(State& s, int current_mode) {
    s.cursor = current_mode;
    return Outcome{Act::None, -1,
                   "Could not create render targets at that size; mode unchanged.", 6.0};
}

}  // namespace menu
