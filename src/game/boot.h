#pragma once
#include <algorithm>
#include <cstdlib>
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

// The objective's column when a level does not state one, as a column rather
// than a point because the row it sits at is scanned off the terrain below it
// (see terrain_surface).
//
// This used to be a single constant, 1700, chosen for the 1920-wide fixture:
// past its jump ledges, across the water channel, out onto the sleeper run. Every
// authored scene since is 344 or 688 cells wide, so the column fell outside all
// of them and no shipped scene had an objective at all. A level that cares where
// its objective is says so in its level file (scene/level_list.h); this is the
// fallback, an eighth of the world in from the far edge -- away from the
// middle-of-the-world spawn, and clear of the border wall.
constexpr int default_objective_column(int world_w) { return world_w - world_w / 8; }

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
//
// `left` is the body's left column; -1 keeps the old middle-of-the-world spot.
inline void stand_player_on_floor(Run& run, int left = -1) {
    run.player = Player(left >= 0 ? left : run.grid.get_width() / 2,
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
inline Objective place_objective(Run& run, int column) {
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

// --- enemies -------------------------------------------------------------
//
// Spread across the world at a fixed step, standing on whatever is under each
// column, and none close enough to the player's start to have noticed them
// already -- a run that opens with something swiping at you has started before
// the player has.
//
// The step is a share of the world's width with a floor under it, because the
// shipped scenes span a factor of five in width (344 to 1920 cells) and a fixed
// step either crowds the small ones or leaves the big one nearly empty.
//
// Scanned off the terrain after the scene is stamped, the same as the props and
// the objective, so moving a hill in the material BMP moves the enemies with it
// instead of burying them. Deterministic in the grid: the same scene plants the
// same enemies in the same places, which is what keeps a session log's start
// state reproducible from the scene alone.
inline constexpr int ENEMY_MIN_SPACING = 12 * species::GHOUL.width;
inline constexpr int ENEMIES_ACROSS = 8;
inline constexpr int ENEMY_EDGE_MARGIN = 2 * species::GHOUL.width;

// From the player's start, box centre to box centre. Past the notice range by a
// body, so nothing planted starts the run already chasing.
constexpr int clearance_for(const Species& kind) { return kind.notice_x + kind.width; }
inline constexpr int ENEMY_CLEARANCE = clearance_for(species::GHOUL);

// What the planter did, for the launch line main.cpp prints.
struct EnemyPlanting {
    int placed = 0;   // every species
    int trolls = 0;   // of which trolls
    int skipped = 0;  // columns under water, or where the pool was full
};

// Where a body of `kind` would stand with its box's left column at x: the y of
// its box's top, or -1 if it cannot stand there -- not enough headroom above the
// surface, or the box is not open air. terrain_surface sees through water,
// because water is not solid, so without the open-air test a column over a pond
// plants an enemy standing on the pond's floor -- alive, underwater, and
// invisible.
inline int standing_y(const Grid& grid, int x, const Species& kind) {
    // Highest, not lowest, for the reason stand_player_on_ground gives: a body
    // stops on the first thing its footprint meets. A column with no terrain at
    // all stands on the world's bottom border, which is solid -- that is the
    // whole of the `floor` scenes, and a body dropped there would land there
    // anyway.
    int surface = highest_surface_under(grid, x, kind.width);
    if (surface < 0) surface = grid.get_height();
    if (surface < kind.height) return -1;
    const int y = surface - kind.height;
    for (int cy = y; cy < surface; ++cy)
        for (int cx = x; cx < x + kind.width; ++cx)
            if (grid.get_element(cx, cy).type != ElementType::Empty) return -1;
    return y;
}

inline EnemyPlanting plant_enemies(Run& run) {
    EnemyPlanting report;
    const int w = run.grid.get_width();
    const int player_cx = run.player.center_x();

    // --- one troll, as far from the start as it will stand ---
    //
    // One, because it is the thing at the far end of the level rather than a
    // population; as far as possible, because a troll is noticed from a long way
    // off and the run should get to see it before it sees them. Planted first so
    // the ghouls are spaced around it rather than it being squeezed in among
    // them. A world too narrow to keep it out of notice range of the start --
    // the smallest shipped scenes -- gets none, rather than one that is already
    // coming when the run begins.
    const Species& troll = species::TROLL;
    int troll_x = -1, troll_y = -1, best = -1;
    for (int x = ENEMY_EDGE_MARGIN; x + troll.width + ENEMY_EDGE_MARGIN <= w; x += troll.width / 2) {
        const int dist = std::abs(x + troll.width / 2 - player_cx);
        if (dist < clearance_for(troll) || dist <= best) continue;
        const int y = standing_y(run.grid, x, troll);
        if (y < 0) continue;
        troll_x = x;
        troll_y = y;
        best = dist;
    }
    if (troll_x >= 0 && run.spawn_enemy(troll_x, troll_y, troll)) {
        ++report.placed;
        ++report.trolls;
    }

    // --- ghouls, spread across the rest ---
    const Species& ghoul = species::GHOUL;
    const int spacing = std::max(ENEMY_MIN_SPACING, w / ENEMIES_ACROSS);
    for (int x = ENEMY_EDGE_MARGIN; x + ghoul.width <= w; x += spacing) {
        if (std::abs(x + ghoul.width / 2 - player_cx) < ENEMY_CLEARANCE) continue;
        // Not inside the troll, nor so close that they start the run overlapping.
        if (report.trolls > 0 && x + ghoul.width + ghoul.width > troll_x &&
            x < troll_x + troll.width + ghoul.width)
            continue;
        const int y = standing_y(run.grid, x, ghoul);
        if (y < 0 || !run.spawn_enemy(x, y, ghoul)) {
            ++report.skipped;
            continue;
        }
        ++report.placed;
    }
    return report;
}

}  // namespace boot
