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
    const pacer::Interpolated mid = pacer::interpolate(10.0f, 20.0f, 12.0f, 20.0f, 0.5f, false);
    check("an ordinary move eases", !mid.snapped && mid.x == 11.0f && mid.y == 20.0f);

    // So does a fast one. A body at terminal velocity covers several cells a step
    // (Player::MAX_FALL_SPEED is about 6.7), and the old distance clamp snapped
    // every step of that dive -- the fall, and the camera following it, stuttered.
    const float dive = fx::to_float(fx::per_step(Player::MAX_FALL_SPEED));
    const pacer::Interpolated fall =
        pacer::interpolate(10.0f, 20.0f, 10.0f, 20.0f + dive, 0.5f, false);
    check("a terminal-velocity fall eases rather than snapping",
          !fall.snapped && fall.y > 20.0f && fall.y < 20.0f + dive, std::to_string(fall.y));

    // A teleport does not, however short. resolve_overlap can shove the body
    // several cells at once to push it out of terrain; easing across that draws the
    // player skating through the wall it was just rescued from.
    const pacer::Interpolated jump = pacer::interpolate(10.0f, 20.0f, 12.0f, 20.0f, 0.5f, true);
    check("a teleport snaps rather than easing",
          jump.snapped && jump.x == 12.0f && jump.y == 20.0f);

    // Both axes snap together. Interpolating one axis while snapping the other
    // draws a diagonal that never happened.
    const pacer::Interpolated diag = pacer::interpolate(10.0f, 20.0f, 11.0f, 60.0f, 0.5f, true);
    check("a teleport snaps both axes", diag.snapped && diag.x == 11.0f && diag.y == 60.0f);
}

// --- who says a move was a teleport ----------------------------------------
//
// The flag the interpolation reads, from the body that knows. A body buried
// shallowly is relocated by the ring search and says so; the next ordinary step
// clears it.
void test_teleport_flag() {
    Grid g(64, 64);
    for (int x = 0; x < 64; ++x) g.set_element(x, 60, ElementType::Wall);
    Player p(20, 60 - Player::HEIGHT);
    p.settle(g);
    PlayerInput none;
    p.update(g, none);
    check("an ordinary step is not a teleport", !p.teleported());

    // Sand poured over the lower half of the body: the ring search lifts it out.
    for (int y = 60 - Player::HEIGHT / 2; y < 60; ++y)
        for (int x = 20; x < 20 + Player::WIDTH; ++x) g.set_element(x, y, ElementType::Sand);
    p.update(g, none);
    check("a relocation out of terrain is reported as a teleport", p.teleported());
    p.update(g, none);
    check("and the report lasts one step", !p.teleported());
}

// --- the settings menu -----------------------------------------------------
//
// A 2560x1440 desktop with a taskbar, so windows stop at 2560x1080; fullscreen
// offers three modes. The window starts as a 1920x1080 window with VSync on and
// the SDL renderer -- the arrangement that reaches every branch below.
ScreenOptions desk() {
    ScreenOptions o;
    o.desktop = DisplayMode{2560, 1440};
    for (int i = 0; i < COMMON_SIZE_COUNT; ++i)
        o.windowed_fits[static_cast<size_t>(i)] = COMMON_SIZES[i].window_h <= 1080;
    o.fullscreen = {{{1280, 720}, 60}, {{1920, 1080}, 144}, {{2560, 1440}, 144}};
    return o;
}

DisplaySettings windowed_1080() {
    return DisplaySettings{WindowMode::Windowed, {1920, 1080}, true, RendererChoice::Sdl};
}

menu::Outcome press(menu::State& s, menu::Key k, const ScreenOptions& o) {
    return menu::key(s, k, false, o);
}

void test_menu_navigation() {
    const ScreenOptions o = desk();
    menu::State s;
    s.cursor = 4;
    s.pending.vsync = false;  // left over from a visit that was abandoned
    menu::open(s, windowed_1080());
    check("opening puts the cursor on the top row", s.cursor == 0);
    check("opening shows what is in effect, not what was abandoned",
          s.pending == windowed_1080() && s.applied == windowed_1080());

    press(s, menu::Key::Prev, o);
    check("up from the top wraps onto quit", s.cursor == static_cast<int>(menu::Row::Quit));
    press(s, menu::Key::Next, o);
    check("down from quit wraps onto the top", s.cursor == 0);

    // Navigation says nothing and changes no screen.
    const menu::Outcome nav = press(s, menu::Key::Next, o);
    check("moving the cursor is not an action",
          nav.act == menu::Act::None && nav.notice == nullptr);
}

void test_menu_values() {
    const ScreenOptions o = desk();
    menu::State s;
    menu::open(s, windowed_1080());

    // The display row: each mode, with the size following it.
    press(s, menu::Key::Right, o);
    check("right on the display row: borderless, at the desktop's size",
          s.pending.window == WindowMode::Borderless && s.pending.size == o.desktop);
    press(s, menu::Key::Right, o);
    check("again: fullscreen, keeping the desktop's size because it is a mode",
          s.pending.window == WindowMode::Fullscreen && s.pending.size == DisplayMode{2560, 1440});
    press(s, menu::Key::Right, o);
    // Borderless -> windowed would carry 2560x1440 into a window the desktop
    // cannot hold; it lands on the largest window that fits instead.
    check("again: back to a window, at the largest that fits",
          s.pending.window == WindowMode::Windowed && s.pending.size == DisplayMode{2560, 1080});
    check("changing values touches nothing in effect", s.applied == windowed_1080());

    // The resolution row steps through the list and wraps.
    menu::open(s, windowed_1080());
    press(s, menu::Key::Next, o);
    press(s, menu::Key::Right, o);
    check("right on the resolution row: the next size up",
          s.pending.size == DisplayMode{2560, 1080});
    press(s, menu::Key::Right, o);
    check("past the largest window wraps to the smallest",
          s.pending.size == DisplayMode{1280, 720});
    press(s, menu::Key::Left, o);
    check("left steps back", s.pending.size == DisplayMode{2560, 1080});

    // Borderless has one size, and stepping it does nothing.
    menu::open(s, DisplaySettings{WindowMode::Borderless, {2560, 1440}, true, RendererChoice::Sdl});
    press(s, menu::Key::Next, o);
    press(s, menu::Key::Right, o);
    check("the borderless size does not step", s.pending.size == DisplayMode{2560, 1440});

    // A size off the list -- the window system resized the window -- lands on
    // the list at the first press rather than skipping past where it should be.
    menu::open(s, DisplaySettings{WindowMode::Windowed, {1700, 950}, true, RendererChoice::Sdl});
    press(s, menu::Key::Next, o);
    press(s, menu::Key::Right, o);
    check("a size off the list lands on the nearest one no larger",
          s.pending.size == DisplayMode{1600, 900});

    menu::open(s, windowed_1080());
    press(s, menu::Key::Next, o);
    press(s, menu::Key::Next, o);
    press(s, menu::Key::Left, o);
    check("the VSync row toggles", !s.pending.vsync);
    press(s, menu::Key::Next, o);
    press(s, menu::Key::Right, o);
    check("the renderer row toggles", s.pending.renderer == RendererChoice::Gl);
    press(s, menu::Key::Next, o);
    const DisplaySettings before = s.pending;
    press(s, menu::Key::Right, o);
    press(s, menu::Key::Next, o);
    press(s, menu::Key::Left, o);
    check("left and right on apply and quit change nothing", s.pending == before);
}

void test_menu_confirm_keys() {
    const ScreenOptions o = desk();

    // Back leaves the menu. It must never quit: a key that ends a session with no
    // confirmation is the wrong key to leave next to a menu.
    {
        menu::State s;
        menu::open(s, windowed_1080());
        press(s, menu::Key::Right, o);
        check("back closes the menu rather than quitting, pending or not",
              press(s, menu::Key::Back, o).act == menu::Act::Close);
    }

    // Enter with nothing changed is a way out, not a no-op -- from any row.
    {
        menu::State s;
        menu::open(s, windowed_1080());
        press(s, menu::Key::Next, o);
        check("enter with nothing pending closes the menu",
              press(s, menu::Key::Confirm, o).act == menu::Act::Close);
    }

    // Quit is reachable only by landing on its row and confirming -- two
    // deliberate presses -- and it does not apply on the way out.
    {
        menu::State s;
        menu::open(s, windowed_1080());
        press(s, menu::Key::Right, o);
        press(s, menu::Key::Prev, o);
        check("enter on quit quits, even with a change pending",
              press(s, menu::Key::Confirm, o).act == menu::Act::Quit);
    }

    // Enter with something pending hands the whole pending set to the caller,
    // from a value row or from the apply row alike.
    for (int row : {static_cast<int>(menu::Row::Resolution), static_cast<int>(menu::Row::Apply)}) {
        menu::State s;
        menu::open(s, windowed_1080());
        press(s, menu::Key::Next, o);
        press(s, menu::Key::Right, o);
        s.cursor = row;
        const menu::Outcome out = press(s, menu::Key::Confirm, o);
        check(("enter applies what is pending, from row " + std::to_string(row)).c_str(),
              out.act == menu::Act::Apply && out.settings.size == DisplayMode{2560, 1080} &&
                  out.settings.window == WindowMode::Windowed);
    }
}

void test_menu_round_trip() {
    const ScreenOptions o = desk();

    // VSync alone cannot leave a screen unusable, so it is saved at once.
    {
        menu::State s;
        menu::open(s, windowed_1080());
        s.cursor = static_cast<int>(menu::Row::VSync);
        press(s, menu::Key::Right, o);
        const menu::Outcome a = press(s, menu::Key::Confirm, o);
        const menu::Outcome b = menu::applied(s, true);
        check("a VSync change is saved without asking", a.act == menu::Act::Apply &&
                                                            b.act == menu::Act::Save &&
                                                            !b.settings.vsync && !s.confirming);
        const menu::Outcome c = menu::saved(true);
        check("a save keeps the menu open and says so",
              c.act == menu::Act::None && c.notice != nullptr);
        check("in effect is what was applied", !s.applied.vsync && s.pending == s.applied);
        check("a second enter, nothing pending now, closes",
              press(s, menu::Key::Confirm, o).act == menu::Act::Close);
    }

    // A save that fails says so, and the menu stays open: the change happened,
    // and what failed is a promise about the next launch.
    {
        const menu::Outcome o2 = menu::saved(false);
        check("a failed save stays open and says so",
              o2.act == menu::Act::None && o2.notice != nullptr && o2.notice_seconds > 0.0);
    }

    // A switch that fails is said, and the rows go back to what is in effect.
    {
        menu::State s;
        menu::open(s, windowed_1080());
        press(s, menu::Key::Right, o);
        press(s, menu::Key::Confirm, o);
        const menu::Outcome out = menu::applied(s, false);
        check("a failed switch says so and asks nothing more",
              out.act == menu::Act::None && out.notice != nullptr);
        check("...and the rows show what is in effect again",
              s.pending == windowed_1080() && s.applied == windowed_1080() && !s.confirming);
    }
}

// The confirmation, which is the whole defence against a mode the monitor
// cannot show: nothing about such a mode is saved until somebody who can see it
// says to keep it, and doing nothing at all goes back.
void test_menu_confirmation() {
    const ScreenOptions o = desk();
    auto switched = [&]() {
        menu::State s;
        menu::open(s, windowed_1080());
        press(s, menu::Key::Right, o);  // borderless
        press(s, menu::Key::Confirm, o);
        menu::applied(s, true);
        return s;
    };

    {
        menu::State s = switched();
        check("a new mode waits to be kept, unsaved",
              s.confirming && s.applied.window == WindowMode::Borderless &&
                  s.confirm_left == menu::CONFIRM_SECONDS);
        // Every other key is ignored while the question is on screen.
        press(s, menu::Key::Next, o);
        press(s, menu::Key::Right, o);
        check("while waiting, moving and changing do nothing",
              s.cursor == 0 && s.pending.window == WindowMode::Borderless);
        const menu::Outcome keep = press(s, menu::Key::Confirm, o);
        check("enter keeps it, and only then is it saved",
              keep.act == menu::Act::Save && keep.settings.window == WindowMode::Borderless &&
                  !s.confirming);
        check("a kept mode does not revert later", menu::tick(s, 60.0).act == menu::Act::None &&
                                                       s.applied.window == WindowMode::Borderless);
    }

    {
        menu::State s = switched();
        const menu::Outcome back = press(s, menu::Key::Back, o);
        check("ESC while waiting goes back rather than leaving the menu",
              back.act == menu::Act::Apply && back.settings == windowed_1080());
        const menu::Outcome done = menu::applied(s, true);
        check("a revert is not itself asked to be confirmed, and says it happened",
              !s.confirming && done.act == menu::Act::None && done.notice != nullptr);
        check("...and the menu shows the old mode in effect again",
              s.applied == windowed_1080() && s.pending == windowed_1080());
    }

    {
        menu::State s = switched();
        check("the countdown does nothing before it runs out",
              menu::tick(s, menu::CONFIRM_SECONDS - 0.1).act == menu::Act::None && s.confirming);
        const menu::Outcome out = menu::tick(s, 0.2);
        check("doing nothing goes back when it runs out",
              out.act == menu::Act::Apply && out.settings == windowed_1080());
        check("while the revert is being carried out, the clock asks nothing twice",
              menu::tick(s, 1.0).act == menu::Act::None);
        check("...and keys cannot keep a mode already being undone",
              press(s, menu::Key::Confirm, o).act == menu::Act::None);
        menu::applied(s, true);
        check("the timed revert lands on the old mode", s.applied == windowed_1080());
    }

    {
        menu::State s = switched();
        press(s, menu::Key::Back, o);
        const menu::Outcome out = menu::applied(s, false);
        check("a revert that fails says so, and the new mode stays unsaved",
              out.notice != nullptr && out.act == menu::Act::None && !s.confirming &&
                  s.applied.window == WindowMode::Borderless);
    }

    {
        // A size change in the same mode is confirmed too; VSync is not.
        DisplaySettings a = windowed_1080();
        DisplaySettings b = a;
        b.size = DisplayMode{1280, 720};
        check("a resolution change needs keeping", menu::needs_confirmation(a, b));
        b = a;
        b.vsync = false;
        b.renderer = RendererChoice::Gl;
        check("VSync and the renderer do not", !menu::needs_confirmation(a, b));
    }
}

// Held keys. The one that matters is Enter: a held Enter would apply a mode and,
// on its next repeat, keep it -- before the player has seen it.
void test_menu_repeat() {
    const ScreenOptions o = desk();
    check("navigation repeats",
          menu::accepts_repeat(menu::Key::Next) && menu::accepts_repeat(menu::Key::Prev) &&
              menu::accepts_repeat(menu::Key::Right) && menu::accepts_repeat(menu::Key::Left));
    check("enter and ESC do not",
          !menu::accepts_repeat(menu::Key::Confirm) && !menu::accepts_repeat(menu::Key::Back));

    menu::State s;
    menu::open(s, windowed_1080());
    menu::key(s, menu::Key::Next, true, o);
    menu::key(s, menu::Key::Right, true, o);
    check("a held arrow still moves and changes",
          s.cursor == 1 && s.pending.size == DisplayMode{2560, 1080});
    check("a held enter does not apply",
          menu::key(s, menu::Key::Confirm, true, o).act == menu::Act::None);
    check("a held ESC does not close",
          menu::key(s, menu::Key::Back, true, o).act == menu::Act::None);

    press(s, menu::Key::Confirm, o);
    menu::applied(s, true);
    check("a held enter does not keep a new mode",
          menu::key(s, menu::Key::Confirm, true, o).act == menu::Act::None && s.confirming);
}

// What the screen says. Each string is a claim about the state, so each is held
// to the state here.
void test_menu_view() {
    const ScreenOptions o = desk();
    menu::State s;
    menu::open(s, windowed_1080());
    menu::View v = menu::view(s, o, RendererChoice::Sdl, 10);
    check("six rows, in order", v.rows.size() == 6 && v.rows[0].label == "DISPLAY" &&
                                    v.rows[1].label == "RESOLUTION" && v.rows[2].label == "VSYNC" &&
                                    v.rows[3].label == "RENDERER" && v.rows[4].label == "APPLY" &&
                                    v.rows[5].label == "QUIT");
    if (v.rows.size() != 6) return;
    check("the resolution says how many cells it shows at the scene's scale",
          v.rows[1].value == "1920X1080" && v.rows[1].note == "192X108 CELLS", v.rows[1].note);
    check("nothing pending: nothing amber, apply greyed",
          !v.rows[0].changed && !v.rows[1].changed && !v.rows[4].enabled);
    check("the cursor's row is the selected one", v.rows[0].selected && !v.rows[1].selected);
    check("the key help is the footer while nothing waits", !v.confirming && !v.footer.empty());

    s.cursor = static_cast<int>(menu::Row::Resolution);
    press(s, menu::Key::Right, o);
    v = menu::view(s, o, RendererChoice::Sdl, 10);
    check("a pending value is marked changed, and apply lights up",
          v.rows[1].changed && v.rows[4].enabled);
    check("at the top of a cut-short window list, it says why it stops",
          v.rows[1].note.find("LARGEST THAT FITS") != std::string::npos, v.rows[1].note);

    s.cursor = static_cast<int>(menu::Row::Renderer);
    press(s, menu::Key::Right, o);
    v = menu::view(s, o, RendererChoice::Sdl, 10);
    check("a renderer other than the one drawing says it waits for a restart",
          v.rows[3].value == "OPENGL" && v.rows[3].note == "ON RESTART");
    v = menu::view(s, o, RendererChoice::Gl, 10);
    check("...and says nothing when it is the one drawing", v.rows[3].note.empty());

    menu::open(s, DisplaySettings{WindowMode::Borderless, {2560, 1440}, true, RendererChoice::Sdl});
    v = menu::view(s, o, RendererChoice::Sdl, 10);
    check("borderless's size is the desktop's and cannot be stepped",
          !v.rows[1].adjustable && v.rows[1].note.find("DESKTOP") == 0, v.rows[1].note);

    menu::open(s, windowed_1080());
    press(s, menu::Key::Right, o);
    press(s, menu::Key::Confirm, o);
    menu::applied(s, true);
    v = menu::view(s, o, RendererChoice::Sdl, 10);
    check("while waiting, the footer is the countdown, from ten",
          v.confirming && v.footer.find("REVERTING IN 10") != std::string::npos, v.footer);
    menu::tick(s, menu::CONFIRM_SECONDS - 0.3);
    v = menu::view(s, o, RendererChoice::Sdl, 10);
    check("the last part-second reads 1, not 0",
          v.footer.find("REVERTING IN 1") != std::string::npos &&
              v.footer.find("REVERTING IN 10") == std::string::npos,
          v.footer);

    // ui::draw_text draws these as gaps, so a notice using one would read wrong.
    auto drawable = [](const char* t) {
        return t && std::string(t).find_first_of(",;'?!") == std::string::npos;
    };
    menu::State n;
    menu::open(n, windowed_1080());
    press(n, menu::Key::Right, o);
    press(n, menu::Key::Confirm, o);
    const bool notices_ok = drawable(menu::saved(true).notice) &&
                            drawable(menu::saved(false).notice) &&
                            drawable(menu::applied(n, false).notice);
    check("every notice is made of characters the HUD font has", notices_ok);
}

}  // namespace

int main() {
    test_freeze_rule();
    test_steps();
    test_interpolation();
    test_teleport_flag();
    test_menu_navigation();
    test_menu_values();
    test_menu_confirm_keys();
    test_menu_round_trip();
    test_menu_confirmation();
    test_menu_repeat();
    test_menu_view();
    return report();
}
