// The shell's per-frame and per-keypress decisions, headless.
//
// One suite for two headers. game/pacer.h and game/settings_menu.h came out of
// the same frame loop, neither is large, and a suite per header would mean two
// more rows in CMakeLists.txt to buy no separation that matters. Split them the
// day one of them grows a fixture the other does not want.
//
// What is asserted here is arithmetic and branching that could not have had a
// test before: it lived inside main()'s frame loop and inside an SDL event
// switch. Each of the three groups below names the failure it exists to catch,
// because none of them look wrong in a screenshot.
#include <string>
#include "game/pacer.h"
#include "game/settings_menu.h"
#include "test_util.h"

namespace {

// --- the freeze rule -------------------------------------------------------
//
// The one thing to get right is that a frozen frame does not bank time. The
// rejected design skips the step loop while the accumulator keeps filling, which
// looks identical until you resume: every frozen second is spent in one burst of
// catch-up steps, clamped only by the stall clamp. That is a visible lurch, and
// it is worst exactly where freezing matters most -- a finished run, where the
// last thing that happened is the thing to look at.
void test_freeze_rule() {
    check("advances while playing", pacer::world_advances(false, false, false));
    check("menu freezes", !pacer::world_advances(true, false, false));
    check("finished run freezes", !pacer::world_advances(false, true, false));
    check("pause freezes", !pacer::world_advances(false, false, true));
    check("two reasons at once still freezes",
          !pacer::world_advances(true, true, false));

    // The behavioural half of the same claim, which is the one that would
    // actually catch the rejected design: sixty frozen frames buy nothing, and
    // the frame after the freeze buys exactly what it is worth and no more.
    pacer::Pacer p;
    for (int i = 0; i < 60; ++i) p.steps(1.0 / 60.0, false);
    check("a minute of frozen frames banks nothing", p.accumulator == 0.0);
    const int after = p.steps(1.0 / 60.0, true);
    check("the frame after a freeze runs one step, not sixty", after == 1,
          "ran " + std::to_string(after));
}

// --- the step count --------------------------------------------------------
void test_steps() {
    pacer::Pacer p;

    // A frame at a high refresh rate is shorter than a step, so most frames buy
    // none. This is the normal case and the reason prev_* has to live outside
    // the loop.
    const int first = p.steps(1.0 / 165.0, true);
    check("a 165 Hz frame usually buys no step", first == 0);

    // ...and the remainder is not lost.
    int total = first;
    for (int i = 0; i < 2; ++i) total += p.steps(1.0 / 165.0, true);
    check("the remainder carries and lands a step", total == 1,
          "ran " + std::to_string(total));

    // Sixty steps in a second, however the second is cut up. Off-by-one in the
    // drain loop's comparison is the classic way to get 59 or 61 here, and the
    // symptom is a character that moves at the wrong speed -- a bug that looks
    // like a tuning complaint and gets answered as one.
    pacer::Pacer q;
    int in_a_second = 0;
    for (int i = 0; i < 165; ++i) in_a_second += q.steps(1.0 / 165.0, true);
    check("one second buys about sixty steps", in_a_second == 59 || in_a_second == 60,
          std::to_string(in_a_second));

    // The stall clamp. A ten-second frame -- a breakpoint, or a dragged window --
    // must not hand the loop ten seconds of debt.
    pacer::Pacer r;
    const int stalled = r.steps(10.0, true);
    check("a ten-second stall is clamped to a quarter second of steps",
          stalled <= 16, std::to_string(stalled) + " steps");

    // Neither a negative delta nor a NaN may reach the accumulator: the first
    // would run the clock backwards, and the second poisons it permanently --
    // every later comparison against FIXED_DT is false, so the world silently
    // stops stepping and nothing says why.
    pacer::Pacer s;
    s.steps(-1.0, true);
    check("a negative frame time is ignored", s.accumulator == 0.0);
}

// --- the interpolation alpha and its teleport clamp ------------------------
void test_interpolation() {
    pacer::Pacer p;
    p.steps(1.0 / 120.0, true);  // half a step of residue
    const float a = p.alpha(false);
    check("alpha is the fraction of a step in hand", a > 0.45f && a < 0.55f,
          std::to_string(a));

    // Pinned to 1 while paused. A single-step debugger showing a picture part-way
    // between two steps is showing a world that never existed.
    check("alpha is pinned to 1 while paused", p.alpha(true) == 1.0f);

    // An ordinary stride interpolates.
    const pacer::Interpolated mid = pacer::interpolate(10.0f, 20.0f, 12.0f, 20.0f, 0.5f);
    check("an ordinary move eases", !mid.snapped && mid.x == 11.0f && mid.y == 20.0f);

    // A teleport does not. resolve_overlap can shove the body several cells at
    // once to push it out of terrain; easing across that draws the player skating
    // through the wall it was just rescued from.
    const pacer::Interpolated jump = pacer::interpolate(10.0f, 20.0f, 40.0f, 20.0f, 0.5f);
    check("a teleport snaps rather than easing",
          jump.snapped && jump.x == 40.0f && jump.y == 20.0f);

    // Either axis over the limit snaps both. Interpolating the small axis while
    // snapping the large one draws a diagonal that never happened.
    const pacer::Interpolated diag = pacer::interpolate(10.0f, 20.0f, 11.0f, 60.0f, 0.5f);
    check("a teleport on one axis snaps the other too",
          diag.snapped && diag.x == 11.0f && diag.y == 60.0f);

    // The clamp is a distance, so it must fire in both directions.
    const pacer::Interpolated back = pacer::interpolate(40.0f, 20.0f, 10.0f, 20.0f, 0.5f);
    check("the teleport clamp is symmetric", back.snapped);
}

// --- the settings menu -----------------------------------------------------
//
// Three display modes, the middle one in use, the last too large for this
// display -- the arrangement that produces every branch worth testing.
void test_menu_navigation() {
    const bool fits[3] = {true, true, false};
    menu::State s;
    menu::open(s, 1);
    check("opening puts the cursor on the mode in use", s.cursor == 1);

    // Four items: three modes and Quit.
    check("quit is the row after the last mode", menu::quit_index(3) == 3);

    menu::key(s, menu::Key::Next, fits, 3, 1);
    menu::key(s, menu::Key::Next, fits, 3, 1);
    check("down moves the cursor", s.cursor == 3);
    menu::key(s, menu::Key::Next, fits, 3, 1);
    check("down wraps off the end onto the first mode", s.cursor == 0);
    menu::key(s, menu::Key::Prev, fits, 3, 1);
    check("up wraps backwards onto quit", s.cursor == 3);

    // Navigation says nothing and changes no screen.
    const menu::Outcome nav = menu::key(s, menu::Key::Prev, fits, 3, 1);
    check("moving the cursor is not an action",
          nav.act == menu::Act::None && nav.notice == nullptr);
}

void test_menu_selection() {
    const bool fits[3] = {true, true, false};

    // Back leaves the menu. It must never quit: a key that ends a session with no
    // confirmation is the wrong key to leave next to a menu.
    {
        menu::State s; menu::open(s, 1);
        check("back closes the menu rather than quitting",
              menu::key(s, menu::Key::Back, fits, 3, 1).act == menu::Act::Close);
    }

    // Confirming the row already in use is a way out, not a no-op.
    {
        menu::State s; menu::open(s, 1);
        check("confirming the current mode closes the menu",
              menu::key(s, menu::Key::Confirm, fits, 3, 1).act == menu::Act::Close);
    }

    // Quit is reachable only by landing on its row and confirming -- two
    // deliberate presses.
    {
        menu::State s; s.cursor = menu::quit_index(3);
        check("confirming quit quits",
              menu::key(s, menu::Key::Confirm, fits, 3, 1).act == menu::Act::Quit);
    }

    // A mode too large for this display is refused with a sentence. Silence here
    // is indistinguishable from a dead key, which is the whole reason the notice
    // exists.
    {
        menu::State s; s.cursor = 2;
        const menu::Outcome o = menu::key(s, menu::Key::Confirm, fits, 3, 1);
        check("a mode that does not fit is refused and says so",
              o.act == menu::Act::None && o.notice != nullptr && o.notice_seconds > 0.0);
        check("a refused mode leaves the cursor alone", s.cursor == 2);
    }

    // A real switch is handed back to the caller rather than performed here.
    {
        menu::State s; s.cursor = 0;
        const menu::Outcome o = menu::key(s, menu::Key::Confirm, fits, 3, 1);
        check("confirming a different mode asks the caller to apply it",
              o.act == menu::Act::ApplyMode && o.mode == 0);
    }
}

void test_menu_switch_results() {
    // Applied and saved: the menu closes and the cursor holds the new mode.
    {
        menu::State s; s.cursor = 0;
        const menu::Outcome o = menu::mode_applied(s, 0, true);
        check("a saved switch closes the menu",
              o.act == menu::Act::Close && o.notice == nullptr && s.cursor == 0);
    }

    // Applied but not saved: the menu stays open. The mode did change -- the
    // player is looking at the proof -- so there is nothing to roll back; what
    // there is, is a promise about next launch the game cannot keep, and the only
    // place to say so is the screen still open.
    {
        menu::State s; s.cursor = 0;
        const menu::Outcome o = menu::mode_applied(s, 0, false);
        check("a switch that could not be saved stays open and says so",
              o.act == menu::Act::None && o.notice != nullptr);
    }

    // Refused: nothing changed, so the cursor returns to the mode in use and the
    // marker and the highlight agree again.
    {
        menu::State s; s.cursor = 2;
        const menu::Outcome o = menu::mode_refused(s, 1);
        check("a refused switch returns the cursor to the mode in use",
              s.cursor == 1 && o.act == menu::Act::None && o.notice != nullptr);
    }
}

}  // namespace

int main() {
    test_freeze_rule();
    test_steps();
    test_interpolation();
    test_menu_navigation();
    test_menu_selection();
    test_menu_switch_results();
    return report();
}
