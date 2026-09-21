#pragma once
#include <cmath>

#include "game/run.h"

// The fixed-step frame pacer: how much simulated time a wall-clock frame buys,
// how many steps that is, and where between two steps the picture falls.
//
// SDL-free and header-only. Nothing here asks a clock what time it is; main.cpp
// reads SDL_GetPerformanceCounter, subtracts, and passes the seconds in. The SDL
// half gets told, not asked.
//
// Worth extracting despite being a page of arithmetic, because every line of it
// is a decision with a wrong answer that looks fine on screen for a while. A
// pacer that banks time while frozen lurches on resume. One that interpolates a
// teleport draws the player skating through a wall. An alpha that keeps running
// while paused shows a world that never existed. None of those is visible in a
// screenshot and all three are assertable.
namespace pacer {

// Clamp on a stalled frame, so a breakpoint or a dragged window does not hand
// the loop a second of debt to spend in one burst.
inline constexpr double MAX_FRAME_TIME = 0.25;

inline double clamp_frame_time(double seconds) {
    if (!(seconds > 0.0)) return 0.0;  // also catches NaN, which would poison the accumulator
    return seconds > MAX_FRAME_TIME ? MAX_FRAME_TIME : seconds;
}

// The freeze rule, as one function, because there is one mechanism and several
// callers of it. The mechanism is always not accumulating time -- never skipping
// the step loop with the accumulator still filling, which banks every frozen
// second and spends it in one catch-up burst at the clamp above.
//
// Stated as separate reasons rather than one `bool frozen` on purpose: a caller
// that has to name which one it means cannot accidentally freeze for a reason
// nobody wrote down.
inline bool world_advances(bool menu_open, bool run_over, bool paused) {
    return !menu_open && !run_over && !paused;
}

struct Pacer {
    // Simulated time owed. Survives across frames, which is the point: at a high
    // refresh rate most frames buy no step at all, and the remainder is what makes
    // the sixty-per-second average come out right.
    double accumulator = 0.0;

    // Advances the clock and returns how many fixed steps the caller must run this
    // frame. The residue stays in `accumulator` for `alpha` below.
    //
    // The caller runs the steps rather than this doing it, because a step needs the
    // world, the input and the animation, and a pacer that reached for those would
    // be the frame loop again under a new name.
    int steps(double frame_time, bool advancing) {
        if (advancing) accumulator += clamp_frame_time(frame_time);
        int n = 0;
        while (accumulator >= Run::FIXED_DT) {
            accumulator -= Run::FIXED_DT;
            ++n;
        }
        return n;
    }

    // How far between the last simulated state and the current one this frame falls.
    // Zero steps this frame is the normal case, and it is why the caller keeps its
    // prev_* outside the loop: alpha keeps climbing towards 1 and the draw keeps
    // easing towards the state already computed, rather than freezing until the next
    // step lands.
    //
    // Pinned to 1 while paused, so what is on screen is the state that was actually
    // simulated rather than a fraction of the way towards it. A single-step debugger
    // whose picture is part-way between two steps is showing a world that never
    // existed, and the whole reason to stop the world is to look at it.
    float alpha(bool paused) const {
        if (paused) return 1.0f;
        return static_cast<float>(accumulator / Run::FIXED_DT);
    }
};

// Teleports must not be interpolated. resolve_overlap can shift the body several
// cells at once to push it out of terrain, and easing across that draws the
// player skating through the wall it was just rescued from. Anything larger than
// a stride is a jump, not motion.
inline constexpr float MAX_INTERPOLATED_CELLS = 4.0f;

// The clamp is on the two-axis distance, not per axis, which is why this takes
// both and returns both. A body pushed four cells sideways and one down has
// teleported on both axes; interpolating the small one and snapping the big one
// draws a diagonal that never happened.
struct Interpolated {
    float x = 0.0f;
    float y = 0.0f;
    bool snapped = false;  // true when the move was treated as a teleport
};

inline Interpolated interpolate(float prev_x, float prev_y, float now_x, float now_y,
                                float alpha) {
    if (std::abs(now_x - prev_x) > MAX_INTERPOLATED_CELLS ||
        std::abs(now_y - prev_y) > MAX_INTERPOLATED_CELLS) {
        return Interpolated{now_x, now_y, true};
    }
    return Interpolated{prev_x + (now_x - prev_x) * alpha,
                        prev_y + (now_y - prev_y) * alpha, false};
}

}  // namespace pacer
