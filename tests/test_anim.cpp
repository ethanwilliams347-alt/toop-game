#include <cstdio>
#include <string>
#include "physics/tool.h"
#include "render/player_anim.h"
#include "test_util.h"

// The animation selector. Headless because player_anim is deliberately SDL-free
// -- what it produces is a sheet row and column, and turning that into a source
// rect is main.cpp's job.
//
// The selector's failure modes are all silent and all look like art problems
// rather than logic problems, which is why they are worth pinning in code rather
// than in the manual checklist: a cycle that restarts every step reads as "the
// walk animation is bad", a one-shot that never completes reads as "the dig
// sprite is stuck", and a clock that advances per rendered frame reads as "the
// walk is too fast on my machine". None of those would send anyone to this file.

using namespace player_anim;
namespace ps = player_sprite;

namespace {

Conditions standing() { Conditions c; c.on_ground = true;  c.moving = false; return c; }
Conditions walking()  { Conditions c; c.on_ground = true;  c.moving = true;  return c; }
Conditions rising()   { Conditions c; c.on_ground = false; c.vel_y = -5;  return c; }
Conditions falling()  { Conditions c; c.on_ground = false; c.vel_y =  5;  return c; }

void run(State& s, const Conditions& c, int steps) {
    for (int i = 0; i < steps; ++i) update(s, c, 1);
}

// --- selection --------------------------------------------------------------

void test_selection() {
    check("standing selects idle", &select(standing()) == &ps::IDLE);
    check("moving on ground selects walk", &select(walking()) == &ps::WALK);
    check("rising selects rise", &select(rising()) == &ps::RISE);
    check("falling selects fall", &select(falling()) == &ps::FALL);

    // The apex of a jump passes through zero. If zero picked `fall`, the pose
    // would flip for exactly one step on the way up and flip back -- a one-frame
    // flicker at the top of every single jump.
    Conditions apex; apex.on_ground = false; apex.vel_y = 0;
    check("apex of a jump holds the rising pose", &select(apex) == &ps::RISE);

    // Input held against a wall is the case `moving` exists for. Selecting on
    // the input instead would walk on the spot against every wall in the game.
    Conditions blocked; blocked.on_ground = true; blocked.moving = false;
    check("blocked against a wall does not walk", &select(blocked) == &ps::IDLE);
}

// --- the shared row ---------------------------------------------------------

void test_shared_row() {
    // rise and fall are one sheet row holding two poses. If `col` were ignored
    // both would draw the same drawing, and the bug would look like missing art
    // rather than like an indexing mistake.
    State s;
    update(s, rising(), 1);
    const int rise_row = s.sheet_row(), rise_col = s.sheet_col();
    update(s, falling(), 1);
    check("rise and fall share a sheet row", s.sheet_row() == rise_row);
    check("rise and fall are different columns", s.sheet_col() != rise_col,
          "rise col " + std::to_string(rise_col) + ", fall col " + std::to_string(s.sheet_col()));
}

// --- the clock --------------------------------------------------------------

void test_walk_cycle() {
    State s;
    update(s, walking(), 1);
    check("walk starts at frame 0", s.frame == 0);

    // One frame's worth of steps advances exactly one frame. This is the
    // property that makes the cycle's speed a function of simulated time rather
    // than of frame rate.
    run(s, walking(), ps::WALK.wait - 1);
    check("walk holds its frame for `wait` steps", s.frame == 1,
          "frame " + std::to_string(s.frame));

    // And a whole cycle returns to the start rather than running off the end of
    // the row into whatever is drawn beside it.
    State t;
    update(t, walking(), 1);
    run(t, walking(), ps::WALK.wait * ps::WALK.frames - 1);
    check("walk loops back to frame 0", t.frame == 0, "frame " + std::to_string(t.frame));
}

void test_no_restart_on_reselect() {
    // The failure this pins: re-selecting the animation already playing resets
    // it, so every step restarts the cycle and the figure stands still with one
    // leg twitching. It is the most likely single bug in this file and it is
    // invisible in a screenshot.
    State s;
    update(s, walking(), 1);
    run(s, walking(), ps::WALK.wait * 2);
    check("a continuing walk is not restarted each step", s.frame == 2,
          "frame " + std::to_string(s.frame));
}

// --- the swing --------------------------------------------------------------
//
// The swing is a phase the tool reports rather than a one-shot latched on the
// step a dig lands, so what is asserted is that the phase maps onto the frames
// and that the swing ends when the tool says it has. The one-shot machinery
// itself is still tested, by the wing beat.

Conditions digging(float progress) {
    Conditions c = standing();
    c.dig_progress = progress;
    return c;
}

void test_dig_follows_the_tool() {
    State s;
    update(s, digging(0.0f), 1);
    check("a swing starting shows the swing's first frame",
          s.anim == &ps::DIG && s.frame == 0);

    // The frames must be spread across the swing rather than bunched at one end
    // -- a swing that never leaves its first frame is the visible defect.
    update(s, digging(0.5f), 1);
    check("half way through the swing is past the first frame",
          s.anim == &ps::DIG && s.frame > 0, "frame " + std::to_string(s.frame));

    update(s, digging(0.99f), 1);
    check("the end of the swing is the swing's last frame",
          s.anim == &ps::DIG && s.frame == ps::DIG.frames - 1,
          "frame " + std::to_string(s.frame));

    // It must hold through conditions that would otherwise select something
    // else, or the swing is cancelled by the player walking away mid-dig.
    Conditions c = walking();
    c.dig_progress = 0.5f;
    update(s, c, 1);
    check("a swing survives a change of conditions", s.anim == &ps::DIG);
}

void test_dig_ends_when_the_tool_says_so() {
    State s;
    update(s, digging(0.5f), 1);
    check("mid-swing shows the dig", s.anim == &ps::DIG);

    // A negative progress is the tool reporting no swing in progress. The
    // animation must let go on that step and not one later: it holds no clock of
    // its own to run down.
    run(s, standing(), 1);
    check("the swing ends the step the tool ends it", s.anim == &ps::IDLE,
          "still on " + std::string(s.anim == &ps::DIG ? "dig" : "something else"));
}

// A swing must not be able to walk off the end of its sheet row, whatever it is
// handed. The clamp this covers is cheap insurance against the two clocks ever
// being reunited by accident.
void test_dig_progress_is_clamped() {
    State s;
    update(s, digging(1.0f), 1);
    check("a full progress still lands on a real frame",
          s.frame >= 0 && s.frame < ps::DIG.frames, "frame " + std::to_string(s.frame));
}

// --- the held swing ----------------------------------------------------
//
// The one test in this file that drives the real tool rather than hand-set
// conditions, and that is the point of it. Every other case here sets the dig
// condition directly, which is why all of them pass while a held dig is visibly
// frozen on its first frame in the game: the defect is not in either clock, it
// is in the ratio between the tool's and the animation's, and a test that
// supplies the condition itself has quietly replaced the very number that was
// wrong.
//
// The requirement: a held dig cycles, and does so slower than the walk.
void test_held_dig_cycles() {
    Grid g(400, 80);
    DigTool tool;
    State s;

    // Long enough to contain several swings at any plausible swing length.
    const int steps = 8 * ps::DIG.wait * ps::DIG.frames;
    bool seen[16] = {false};
    int frames_seen = 0;
    int returns_to_start = 0;
    int last_frame = -1;

    for (int i = 0; i < steps; ++i) {
        // Refilled every step on purpose. Left alone, the tunnel outruns the dig's
        // range, the tool stops connecting, and the one-shot then completes
        // undisturbed -- which is the defect (release the button and one swing
        // plays) passing itself off as the cycle being tested. The scenario is a
        // button held against terrain that is still there.
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 400; ++x)
                g.set_element(x, y, ElementType::Wall);

        Conditions c = standing();
        tool.update(g, true, 200, 40, 399, 40);  // held, connecting
        c.dig_progress = tool.swing_progress();
        update(s, c, 1);

        if (s.anim != &ps::DIG) continue;
        if (!seen[s.frame]) { seen[s.frame] = true; ++frames_seen; }
        if (last_frame > 0 && s.frame == 0) ++returns_to_start;
        last_frame = s.frame;
    }

    check("a held dig plays every frame of the swing", frames_seen == ps::DIG.frames,
          std::to_string(frames_seen) + " of " + std::to_string(ps::DIG.frames) +
              " frames reached");
    check("a held dig keeps swinging rather than playing once", returns_to_start >= 2,
          std::to_string(returns_to_start) + " swings in " + std::to_string(steps) +
              " steps");

    // Slower than the walk, which is the shape of the request rather than a number
    // pulled out of the air. Derived from the walk's own length so retuning the walk
    // cannot silently invalidate this.
    const int walk_cycle = ps::WALK.wait * ps::WALK.frames;
    const int swing_cycle = steps / (returns_to_start > 0 ? returns_to_start : steps);
    check("a swing is slower than a walk cycle", swing_cycle > walk_cycle,
          "swing " + std::to_string(swing_cycle) + " steps vs walk " +
              std::to_string(walk_cycle));
}

// --- the fixed-step contract ------------------------------------------------

void test_batch_equivalence() {
    // Advancing by N is the same as advancing by 1 N times. This is what makes
    // it sound to drive the clock off a count of fixed steps: a frame that
    // happens to contain three steps must land where three frames of one step
    // each would have.
    State a, b;
    const int n = ps::WALK.wait * ps::WALK.frames + 3;
    update(a, walking(), n);
    run(b, walking(), n);
    check("advancing N steps equals N single steps",
          a.anim == b.anim && a.frame == b.frame && a.elapsed == b.elapsed,
          "batch frame " + std::to_string(a.frame) + " vs stepped " + std::to_string(b.frame));
}

void test_static_pose_does_not_spin() {
    // rise/fall have wait 0. A naive advance would divide by it or loop
    // forever; this asserts it simply holds.
    State s;
    run(s, falling(), 500);
    check("a wait-0 pose holds rather than spinning", s.frame == 0 && s.anim == &ps::FALL);
}

}  // namespace

int main() {
    test_selection();
    test_shared_row();
    test_walk_cycle();
    test_no_restart_on_reselect();
    test_dig_follows_the_tool();
    test_dig_ends_when_the_tool_says_so();
    test_dig_progress_is_clamped();
    test_held_dig_cycles();
    test_batch_equivalence();
    test_static_pose_does_not_spin();
    return report();
}
