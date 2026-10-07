#pragma once
#include <array>
#include "physics/arrow.h"
#include "physics/enemy.h"
#include "physics/grid.h"
#include "physics/player.h"
#include "physics/tool.h"

// Everything outside the run that one fixed step needs -- filled from SDL by
// main.cpp, and filled by hand in tests/test_run.cpp.
//
// Sampling the keyboard once per rendered frame and replaying that one sample
// into every fixed step the frame happened to contain makes the framerate an
// input to the simulation. One Input drives exactly one fixed step, so a
// recorded sequence replays to the same result regardless of how the original
// session was paced across frames.
//
// A plain struct of button and cursor state rather than SDL types, for the same
// reason PlayerInput is: it keeps everything under src/game/ and src/physics/
// testable without a window.
struct Input {
    bool left = false;
    bool right = false;
    bool jump = false;
    bool dig = false;

    // Cursor position in grid cells, shared by the dig tool's aim and by where
    // the brush paints -- both come from the same mouse position, so one pair of
    // coordinates covers both.
    int cursor_x = 0;
    int cursor_y = 0;

    // The world-editing brush. Folded in here rather than kept as a special case:
    // painted once per rendered frame, the amount of material laid down while the
    // button is held depends on render framerate rather than on how much simulated
    // time passed. It is now three more fields on the struct that drives a step,
    // painted once per step like everything else.
    bool brush_active = false;
    ElementType brush_type = ElementType::Sand;
    int brush_size = 1;

    // The bow, held: looses an arrow at the cursor every Quiver::DRAW_STEPS.
    bool shoot = false;

    // Puts an enemy down centred on the cursor, on this step. A one-shot rather
    // than a held state, and main.cpp clears it after the first step of the frame
    // that pressed it -- a frame that bought three steps would otherwise spawn
    // three. It is on Input rather than being a call main.cpp makes on the Run
    // directly because it changes the world, and every change to the world has to
    // be in the recorded stream or a replay of the session is a different session.
    bool spawn_enemy = false;
};

// Everything one play session needs, held as a single object instead of three
// locals main.cpp had to thread through by hand. SDL-free for the same reason
// src/physics/ is: a run that needs a window cannot be driven by a test.
class Run {
public:
    // How this run ended, or that it has not.
    //
    // Playing is a real state and not the absence of the other two, which is why
    // this is an enum rather than a pair of bools: a run that is somehow both won
    // and lost is unrepresentable, and the neither case has a name that reads
    // correctly at the call site.
    enum class Outcome : uint8_t { Playing, Won, Lost };

    // The simulation advances in fixed steps so that sand falls at the same rate on
    // a 60 Hz and a 144 Hz display; rendering runs as fast as the display allows and
    // calls step() as many times as have accumulated.
    //
    // This is the frame pacer's copy of the rate, not the simulation's. main.cpp
    // accumulates real elapsed seconds against it and interpolates the drawn
    // position by the leftover; nothing inside a step reads it. The physics uses
    // fx::STEPS_PER_SECOND instead, as an integer, because a step's worth of gravity
    // has to be the same number on every machine and a double reciprocal is not. So
    // it is derived from the integer rather than written out beside it: two
    // spellings of one rate is two chances to change one of them, and a pacer
    // running at a rate the physics does not believe in presents as a character
    // moving at the wrong speed.
    static constexpr double FIXED_DT = 1.0 / fx::STEPS_PER_SECOND;

    // The player spawns in mid-air over the middle of the world. There is no terrain
    // yet, but the world border reads as solid, so it falls to the bottom edge
    // rather than out of existence.
    Run(int width, int height, uint64_t seed = Grid::DEFAULT_SEED);

    // Puts the run back to what a fresh Run(width, height, seed) would be: `grid`
    // goes through Grid::reset(seed) rather than being reallocated, and
    // player/dig_tool are replaced outright since neither owns anything a wipe would
    // need to preserve.
    //
    // Two things it deliberately does not restore. The objective survives, for the
    // reason at the field below -- it is a property of the level and not of the run.
    // And the world's terrain does not come back, because Run does not own it: the
    // scene is loaded from two BMPs by main.cpp, so a caller that wants the level it
    // started with has to re-stamp it after calling this.
    //
    // The size arguments are the one way a grid's dimensions can change at all.
    // Grid::reset cannot resize and deliberately says so; a scene list whose rows
    // name different world sizes needs some path that can, and the choice is between
    // this and tearing the whole Run down at the call site -- which would leave
    // main.cpp's several lambdas holding a Run& to a dead object.
    //
    // 0 means keep the size, not a world of no cells, so every existing caller and
    // every recorded session is unaffected.
    //
    // A resize is a heavier reset than a wipe and the difference is observable:
    // Grid::reset's documented exception -- vent_radius surviving, because it is
    // configuration rather than world state -- does not survive a resize, because
    // the grid is a new object. Carrying it across would mean the new grid is not a
    // fresh Grid(w, h, seed), which is the one thing the reset contract promises.
    void reset(uint64_t seed, int new_width = 0, int new_height = 0);

    // Advances grid, player, dig tool, arrows and enemies by exactly one fixed
    // step, in that order.
    // The brush paints first, before the grid steps: a cell should not move on the
    // same step it was placed.
    //
    // Returns whether the dig tool actually removed material this step --
    // DigTool::update's own answer, forwarded rather than recomputed.
    //
    // Nothing currently reads it. The animation reads DigTool::swing_progress()
    // instead, because an impact is one step and a swing is a duration, and it was
    // the duration that needed portraying. Kept because "a blow landed this step" is
    // a real event that a hit reaction, a sound or a screen shake will each want, and
    // because it costs a forwarded bool. Do not reintroduce it as an animation
    // trigger.
    bool step(const Input& input);

    // --- the objective, and how a run ends -----------------------------
    //
    // The objective is a point in the world, not a cell, and that is a deliberate
    // limit. A cell would mean a row in MATERIALS, which would mean answering what
    // happens when it is dug, burnt, displaced or buried. A point is reached or it
    // is not.
    //
    // Placed by the caller rather than by a generator, because there is no
    // generator: main.cpp scans the terrain for a surface the same way it plants
    // props.
    void set_objective(int x, int y);

    // Removes the objective outright.
    //
    // The escape hatch for the exception recorded at objective_set below. Surviving
    // reset() is right when the caller re-stamps the same scene; switching to a
    // scene with no terrain leaves the previous level's objective hanging in empty
    // space -- placed, unreachable, and claiming the run is winnable. So the rule is
    // narrowed rather than changed: the objective survives a reset, and a caller
    // that changes the level says so.
    void clear_objective();
    bool has_objective() const { return objective_set; }
    int objective_x() const { return goal_x; }
    int objective_y() const { return goal_y; }

    // How close the body has to get, in cells, measured from the objective to the
    // nearest point of the collision box rather than to the body's centre -- so a
    // body reaches the same objective from the same distance whether it is standing
    // beside it or under it.
    //
    // A little under half a body height: small enough to read as touching the
    // marker, large enough not to need pixel-accurate positioning.
    static constexpr int OBJECTIVE_REACH = 6;

    Outcome outcome() const { return run_outcome; }

    // --- enemies ---------------------------------------------------------
    //
    // A fixed pool, for the step loop's no-allocation rule: spawning on a step
    // reuses a dead slot rather than growing anything. Twenty-four is several
    // screens' worth -- the planter puts down a handful, and the spawn key is a
    // development tool.
    static constexpr int MAX_ENEMIES = 24;

    // Brings a dead slot to life with the box's top-left at (x, y). Refused, and
    // false, when every slot is taken or the box would start inside something
    // solid -- an enemy spawned into a wall would spend its life stuck there.
    bool spawn_enemy(int x, int y);

    int enemies_alive() const;

    // Enemies brought down this run. A count rather than an event list: it is for
    // the readout, and the readout wants the number.
    int kills() const { return kill_count; }

    Grid grid;
    Player player;
    DigTool dig_tool;
    Quiver quiver;
    std::array<Enemy, MAX_ENEMIES> enemies{};

private:
    // The outcome latches. Once a run is over it stays over, even though step()
    // keeps simulating: freezing the world is a presentation decision and main.cpp
    // makes it by not accumulating time. A Run driven headlessly past its own ending
    // must not have the answer flicker back.
    Outcome run_outcome = Outcome::Playing;
    int kill_count = 0;

    // The objective survives reset(), and this is the second documented exception to
    // "reset restores a fresh Run", after Grid::vent_radius. It is a property of the
    // level, not of the run played through it, and the caller re-stamps the same
    // scene on reset, so a cleared objective would be one the caller has to remember
    // to place again. Forgetting would produce a run that is unwinnable and says
    // nothing.
    bool objective_set = false;
    int goal_x = 0;
    int goal_y = 0;
};
