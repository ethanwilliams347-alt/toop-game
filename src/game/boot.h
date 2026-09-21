#pragma once
#include <vector>
#include "game/run.h"
#include "physics/grid.h"
#include "physics/material.h"
#include "physics/player.h"
#include "scene/props.h"

// The decisions main() takes before its first frame, kept out of the SDL shell
// so a test can reach them. Everything here previously ran inline in main(),
// once at startup, and could only be checked by a person reading a printed line.
//
// A header rather than a sixth source-set variable: the guard in CMakeLists.txt
// is a set of variables kept apart so that a simulation source reaching for a
// renderer has to be written into the build file to compile, and every new
// variable is a chance to blur that. This header links ENGINE_SOURCES and
// nothing else -- PropDef is a struct definition, not a link dependency.
//
// No SDL, by rule. The one thing startup genuinely needs a window for is a
// prop's size, since that comes from its texture, so plant_props is handed
// widths as data: main.cpp queries SDL for them, this decides where each prop
// stands, and the decision has a test.
namespace boot {

// The simulated world's size, in cells, independent of the window. Deliberately
// not any window size divided by the camera's scale: the shipped scene is
// authored larger than every viewport in the display table, which exercises the
// panning half of the camera rather than only the decoupling half.
//
// It has to clear the largest viewport, not the one the game happens to launch
// at, or the widest mode would have nothing to pan across.
inline constexpr int GRID_WIDTH = 1920;
inline constexpr int GRID_HEIGHT = 1080;

// The objective, as a column rather than a point, because the row it sits at is
// scanned off the terrain below it (see terrain_surface). Placing a y here would
// be the mistake the prop format refuses by construction -- a number an author
// tunes for an afternoon while the loader ignores it.
//
// The column is chosen for what stands between it and the spawn rather than for
// where it is: past the jump ledges, across the water channel, and out onto the
// sleeper run. That is a traverse the character cannot walk, which makes flight
// the thing the run is actually about.
//
// Hard-coded, which is this spike's stated limit. A real objective is placed by
// a generator into a level format with a slot for it, and neither exists yet.
inline constexpr int OBJECTIVE_X = 1700;

// The first solid row in a column, or -1 if the column is open all the way down.
// Kept separate from the prop planter's scan rather than shared with it: that
// one takes the lowest surface across a sprite's width so a tree leans into a
// hill, and this one is a single column, so a shared helper would have to be
// told which of the two it was being.
inline int terrain_surface(const Grid& grid, int x) {
    if (x < 0 || x >= grid.get_width()) return -1;
    for (int y = 0; y < grid.get_height(); ++y)
        if (is_solid(grid.get_element(x, y).type)) return y;
    return -1;
}

// The lowest first-solid row found anywhere across [x0, x0 + width), or -1 if no
// column in that span has ground under it.
//
// Lowest, not the centre column's, which is what makes a tree on a slope lean
// into the hill instead of floating off its uphill edge. Columns outside the
// world are skipped rather than treated as open air, so a prop half off the map
// is planted on the half that exists.
inline int lowest_surface_under(const Grid& grid, int x0, int width) {
    int lowest = -1;
    for (int x = x0; x < x0 + width; ++x) {
        const int surface = terrain_surface(grid, x);
        if (surface > lowest) lowest = surface;
    }
    return lowest;
}

// The highest first-solid row across [x0, x0 + width), or -1 if no column in
// that span has ground under it.
//
// The mirror of lowest_surface_under, and the pair is worth reading together,
// because picking the wrong one is a silent bug rather than a crash. A prop
// wants the lowest so a tree leans into a hill; a body wants the highest,
// because a body falling onto uneven ground stops on the first thing its
// footprint meets, not on the deepest. Give a body the lowest and it stands with
// its feet inside the hill.
inline int highest_surface_under(const Grid& grid, int x0, int width) {
    int highest = -1;
    for (int x = x0; x < x0 + width; ++x) {
        const int surface = terrain_surface(grid, x);
        if (surface >= 0 && (highest < 0 || surface < highest)) highest = surface;
    }
    return highest;
}

// Whether the body found ground to stand on, and the row its feet ended on.
struct Standing {
    bool placed = false;
    int surface = 0;
};

// Stands the body on the terrain under its own spawn column, instead of leaving
// it where Run's constructor put it.
//
// Run spawns the player part-way down the world because it is built before any
// terrain is, which with a scene loaded is a long free fall on every launch.
//
// The drop is removed rather than shortened, and that is a decision: a short
// fall would still spend Player::has_landed -- the flag that makes the spawn
// drop free of fall damage -- on the first frame of the game, which is a
// mechanic being consumed by the camera getting into position. Standing the body
// on the surface leaves it for the first jump, where it means something.
//
// A column with no ground under it leaves the body where it was, the same
// refusal place_objective makes: the only fallback available is a guess, and a
// body placed at a guessed row is worse than one that falls, because falling at
// least ends up somewhere real. The caller warns.
inline Standing stand_player_on_ground(Run& run) {
    const int surface = highest_surface_under(run.grid, run.player.cell_x(), Player::WIDTH);
    if (surface < 0) return Standing{};
    run.player = Player(run.player.cell_x(), surface - Player::HEIGHT);
    return Standing{true, surface};
}

// Stands the body on the world's bottom border instead of on terrain.
//
// For a scene that is meant to have no terrain, and not as a fallback for
// stand_player_on_ground failing. That distinction is why this is a second
// function rather than a branch inside that one: a terrain scan that comes back
// empty is either a scene with no ground under the spawn column -- a defect the
// caller warns about -- or a scene declared empty in assets/scenes.txt, which is
// not. Collapsing the two would make a broken scene look playable.
//
// The border is what it stands on: the world edge reads as solid, so a body with
// no terrain under it falls to the bottom edge rather than out of existence.
// This puts it there directly, so the drop is removed rather than shortened and
// Player::has_landed is still unspent when the player takes their first real
// fall.
inline void stand_player_on_floor(Run& run) {
    run.player = Player(run.grid.get_width() / 2,
                        run.grid.get_height() - Player::HEIGHT);
}

// Where the objective ended up, and whether it got placed at all.
struct Objective {
    bool placed = false;
    int x = 0;
    int y = 0;
};

// Plants the objective on whatever terrain is actually at `column`, the same way
// a prop is planted. Its row is the centre of a body standing on that surface,
// so reaching the objective means standing where the marker is rather than
// working something out from a floating icon.
//
// A column with no ground under it drops the objective rather than defaulting
// it, which is the prop planter's rule: the only fallback available is the top
// of the world, and an objective hanging in the sky is exactly as wrong as one
// buried. A run with no objective is still playable, so the caller warns rather
// than refusing to start.
inline Objective place_objective(Run& run, int column = OBJECTIVE_X) {
    const int surface = terrain_surface(run.grid, column);
    if (surface < 0) return Objective{};
    const int y = surface - Player::HEIGHT / 2;
    run.set_objective(column, y);
    return Objective{true, column, y};
}

// One prop that found ground, as an index back into the caller's PropDef list
// plus the row its bottom edge sits on.
struct Planted {
    int def_index = 0;
    int anchor_y = 0;
};

// What the planting scan did with each record. Three outcomes, held apart,
// because the count the launch check prints has to include both ways a prop can
// be dropped -- a sprite that would not load, and a prop with no ground under
// it. A count taken before the second of those can happen reports every prop
// placed on a run that drew fewer.
struct PlantingReport {
    std::vector<Planted> planted;
    std::vector<int> no_texture;  // def indices whose sprite did not load
    std::vector<int> no_ground;  // def indices with no solid cell underneath
};

// Plants each prop on the terrain that is actually under it, rather than on a
// hardcoded ground line. Props authored against a floor slab's height are buried
// by anything standing on that slab, and invisibly so if they sit off-screen at
// spawn.
//
// widths[i] is defs[i]'s sprite width in world cells, or 0 for a sprite that did
// not load, which is how the SDL half of this reaches a function that knows no
// SDL. A short `widths` is treated as all-zero from that point, so a caller that
// gets the two lists out of step drops props loudly rather than reading off the
// end.
//
// Runs once, after the scene is stamped and before the first frame: props are
// not simulated, so terrain that moves later does not drag them with it -- which
// is correct for a tree.
inline PlantingReport plant_props(const Grid& grid,
                                  const std::vector<PropDef>& defs,
                                  const std::vector<int>& widths) {
    PlantingReport report;
    report.planted.reserve(defs.size());
    for (int i = 0; i < static_cast<int>(defs.size()); ++i) {
        const int w = i < static_cast<int>(widths.size()) ? widths[i] : 0;
        if (w <= 0) {
            report.no_texture.push_back(i);
            continue;
        }
        const int x0 = static_cast<int>(defs[i].x - w / 2.0f);
        const int surface = lowest_surface_under(grid, x0, w);
        // No solid ground anywhere under it is a scene-authoring mistake, not
        // something to paper over with a default: a prop hanging in the air is
        // exactly as wrong as one buried.
        if (surface < 0) {
            report.no_ground.push_back(i);
            continue;
        }
        report.planted.push_back(Planted{i, surface});
    }
    return report;
}

}  // namespace boot
