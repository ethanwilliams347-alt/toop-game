// The startup decisions, headless.
//
// Everything asserted below used to run inside main(), print a line to stdout,
// and be checkable only by a person reading the line at launch.
//
// Two halves. The unit half covers the decisions in game/boot.h and
// choose_display_mode in game/display.h against worlds this file builds. The
// fixture half runs the shipped scene, which covers the launch lines that were
// otherwise eyeballed on stdout -- the objective's position and the placed-prop
// count.
//
// It reads real BMPs for the prop widths rather than assuming a size, because
// the width decides which columns a prop's footprint scans, and a planting test
// against a made-up width would pass over art that had changed shape. One BMP
// pixel is one world cell, so bmp::read gives exactly the number
// SDL_QueryTexture gives the game.
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <set>
#include <string>
#include <vector>
#include "game/boot.h"
#include "game/display.h"
#include "game/run.h"
#include "game/scene_activation.h"
#include "physics/grid.h"
#include "render/bg1_backdrop.h"
#include "scene/bmp.h"
#include "scene/props.h"
#include "scene/scene.h"
#include "scene/scene_list.h"
#include "test_util.h"

namespace {

// A floor at `top` across [x0, x1), in a world otherwise empty.
void build_floor(Grid& grid, int x0, int x1, int top) {
    for (int x = x0; x < x1; ++x)
        for (int y = top; y < top + 4; ++y)
            grid.set_element(x, y, ElementType::Wall);
}

void test_terrain_surface() {
    Grid grid(64, 64);
    build_floor(grid, 10, 20, 30);

    check("terrain_surface: the first solid row in a built column",
          boot::terrain_surface(grid, 12) == 30);
    check("terrain_surface: -1 in a column that is open all the way down",
          boot::terrain_surface(grid, 5) == -1);
    // Out of bounds is -1 and not a read off the end. The prop planter walks a
    // footprint that can hang over the edge of the world, so this is the case it
    // hits rather than a defensive one.
    check("terrain_surface: -1 left of the world", boot::terrain_surface(grid, -1) == -1);
    check("terrain_surface: -1 right of the world", boot::terrain_surface(grid, 64) == -1);
}

void test_lowest_surface_under() {
    Grid grid(64, 64);
    // A step: the left half of the span is higher ground than the right.
    build_floor(grid, 0, 10, 20);
    build_floor(grid, 10, 30, 34);

    // The lowest, not the nearest and not the centre column's. This is the whole of
    // why a tree on a slope leans into the hill rather than floating off its uphill
    // edge; taking the highest instead is the version that buries it.
    check("lowest_surface_under: takes the lowest surface across the footprint",
          boot::lowest_surface_under(grid, 5, 10) == 34,
          std::to_string(boot::lowest_surface_under(grid, 5, 10)));
    check("lowest_surface_under: -1 when no column in the span has ground",
          boot::lowest_surface_under(grid, 40, 10) == -1);
    // Half off the map plants on the half that exists. The columns outside the world
    // are skipped, not counted as open air -- counting them would be the same answer
    // here and a different one if the rule were "highest".
    check("lowest_surface_under: a footprint hanging off the left edge still plants",
          boot::lowest_surface_under(grid, -5, 10) == 20);
}

void test_place_objective() {
    {
        Run run(64, 64);
        build_floor(run.grid, 30, 40, 25);
        const boot::Objective obj = boot::place_objective(run, 35);
        check("place_objective: places on the terrain actually at that column",
              obj.placed && obj.x == 35 && obj.y == 25 - Player::HEIGHT / 2,
              std::to_string(obj.y));
        check("place_objective: ...and the run agrees",
              run.has_objective() && run.objective_x() == obj.x &&
                  run.objective_y() == obj.y);
    }
    {
        // Dropped rather than defaulted. The only fallback available is the top of
        // the world, and an objective hanging in the sky is exactly as wrong as one
        // buried -- so a column with no ground leaves the run without an objective,
        // and the caller says so.
        Run run(64, 64);
        const boot::Objective obj = boot::place_objective(run, 35);
        check("place_objective: a column with no ground places nothing", !obj.placed);
        check("place_objective: ...and does not leave the run holding one",
              !run.has_objective());
    }
}

void test_plant_props() {
    Grid grid(64, 64);
    build_floor(grid, 0, 10, 20);
    build_floor(grid, 10, 30, 34);

    std::vector<PropDef> defs = {
        {"on_the_step", 10.0f, 1},  // footprint spans both floor heights
        {"over_air", 50.0f, 2},  // nothing under it
        {"no_sprite", 5.0f, 3},  // texture did not load
    };
    const std::vector<int> widths = {8, 8, 0};

    const boot::PlantingReport r = boot::plant_props(grid, defs, widths);

    check("plant_props: a prop over ground is planted", r.planted.size() == 1 &&
          r.planted[0].def_index == 0);
    check("plant_props: ...at the lowest surface under its own footprint",
          r.planted.size() == 1 && r.planted[0].anchor_y == 34,
          r.planted.empty() ? "nothing planted" : std::to_string(r.planted[0].anchor_y));
    check("plant_props: a prop over open air is dropped, not defaulted",
          r.no_ground.size() == 1 && r.no_ground[0] == 1);
    check("plant_props: a prop whose sprite did not load is dropped separately",
          r.no_texture.size() == 1 && r.no_texture[0] == 2);
    // Both drops have to be reachable from the report, because the launch line
    // counts placed against defined. A report that merged the two would still
    // produce the right total, and would stop the caller being able to say which
    // happened.
    check("plant_props: every record is accounted for exactly once",
          r.planted.size() + r.no_ground.size() + r.no_texture.size() == defs.size());

    // A caller that gets the two lists out of step drops props loudly rather than
    // reading off the end of the shorter one.
    const boot::PlantingReport shortened = boot::plant_props(grid, defs, {8});
    check("plant_props: a short widths list drops the records it does not cover",
          shortened.planted.size() == 1 && shortened.no_texture.size() == 2);
}

void test_choose_display_mode() {
    const bool all_fit[3] = {true, true, true};
    const bool small_only[3] = {true, false, false};
    const bool none_fit[3] = {false, false, false};

    {
        const ModeChoice c = choose_display_mode(all_fit, 3, -1);
        check("choose_display_mode: nothing stored opens at the largest that fits",
              c.index == 2 && c.why == ModeChoice::Why::Largest);
    }
    {
        const ModeChoice c = choose_display_mode(all_fit, 3, 0);
        check("choose_display_mode: a stored mode that still fits wins",
              c.index == 0 && c.why == ModeChoice::Why::Stored);
    }
    {
        // The monitor changed between runs. Opening at a stored oversized mode puts
        // the settings menu -- the one way back -- off the edge of the display.
        const ModeChoice c = choose_display_mode(small_only, 3, 2);
        check("choose_display_mode: a stored mode that no longer fits is ignored",
              c.index == 0 && c.why == ModeChoice::Why::StoredTooBig);
    }
    {
        // An oversized window is a bad session; no window at all is no session.
        const ModeChoice c = choose_display_mode(none_fit, 3, 2);
        check("choose_display_mode: nothing fitting still opens, at the smallest",
              c.index == 0 && c.why == ModeChoice::Why::NothingFits);
    }
}

// The empty scene's spawn. A world with no terrain at all is a legitimate scene
// -- assets/scenes.txt ships one so the backdrop can be looked at with nothing
// standing in front of it -- and the body has to end up somewhere real in it.
void test_stand_player_on_floor() {
    Run run(40, 40);  // no terrain stamped: this world is entirely Empty
    boot::stand_player_on_floor(run);

    check("stand_player_on_floor: the body is inside the world",
          run.player.cell_y() + Player::HEIGHT <= 40,
          std::to_string(run.player.cell_y()) + " + " + std::to_string(Player::HEIGHT));
    check("stand_player_on_floor: ...and as low as it can be",
          run.player.cell_y() + Player::HEIGHT == 40,
          std::to_string(run.player.cell_y() + Player::HEIGHT));

    // The claim that matters is that it rests there, and it rests on the world
    // border rather than on anything stamped. A step is taken because is_on_ground
    // is recomputed by the step, not by the placement.
    run.step(Input{});
    check("stand_player_on_floor: the world border holds it up",
          run.player.is_on_ground());
    check("stand_player_on_floor: ...and it has not fallen through",
          run.player.cell_y() + Player::HEIGHT == 40,
          std::to_string(run.player.cell_y() + Player::HEIGHT));

    // The drop is removed, not shortened -- so the free first landing is still
    // unspent for the player's first real fall. Same argument as
    // stand_player_on_ground's, and the reason neither of them just lets the body
    // fall.
    check("stand_player_on_floor: the spawn costs no health",
          run.player.health() == Player::MAX_HEALTH,
          std::to_string(run.player.health()));
}

void test_stand_player_on_ground() {
    // The mirror pair. Giving a body the lowest surface under its footprint puts its
    // feet inside the hill, which is a silent bug rather than a crash, so the two
    // are asserted against the same terrain in one place.
    Grid g(40, 40);
    for (int x = 0; x < 20; ++x) g.set_element(x, 30, ElementType::Wall);  // low step
    for (int x = 20; x < 40; ++x) g.set_element(x, 24, ElementType::Wall);  // high step
    check("highest_surface_under: a footprint straddling a step takes the high side",
          boot::highest_surface_under(g, 16, 8) == 24);
    check("lowest_surface_under: ...and the prop planter still takes the low one",
          boot::lowest_surface_under(g, 16, 8) == 30);

    {
        // The body ends up standing on the terrain rather than falling to it.
        Run run(40, 40);
        // Deliberately not the row the body already spawns at: a surface there
        // would put the body exactly where it was, and the assertion below would
        // pass on a stand_player_on_ground that does nothing at all.
        for (int x = 0; x < 40; ++x) run.grid.set_element(x, 34, ElementType::Wall);
        const int before = run.player.cell_y();
        const boot::Standing s = boot::stand_player_on_ground(run);
        check("stand_player_on_ground: it finds the ground", s.placed && s.surface == 34,
              std::to_string(s.surface));
        check("stand_player_on_ground: the feet land exactly on the surface",
              run.player.cell_y() + Player::HEIGHT == 34,
              std::to_string(run.player.cell_y()));
        check("stand_player_on_ground: ...which is somewhere it was not", before != run.player.cell_y());
    }
    {
        // A world with nothing under the spawn column leaves the body where it was,
        // rather than guessing a row. Same refusal place_objective makes.
        Run run(40, 40);
        const int before = run.player.cell_y();
        const boot::Standing s = boot::stand_player_on_ground(run);
        check("stand_player_on_ground: no ground means no placement", !s.placed);
        check("stand_player_on_ground: ...and the body is not moved to a guess",
              run.player.cell_y() == before);
    }
}

// --- the fixture half: the launch check, as assertions ---

void test_shipped_fixture() {
    std::string error, warning;
    Scene scene = bmp::load("assets/test_material.bmp", "assets/test_albedo.bmp",
                            &error, &warning);
    if (!error.empty()) {
        check("fixture: the shipped scene loads", false, error);
        return;
    }

    Run run(boot::GRID_WIDTH, boot::GRID_HEIGHT);
    const int cells = load_scene(run.grid, scene, 0, 0);
    check("fixture: the shipped scene stamps cells into the world", cells > 0,
          std::to_string(cells));

    // The spawn, on the scene the game actually loads. The unit case above proves
    // the scan; this proves the shipped fixture still has floor under the spawn
    // corridor. A regression here is the long free fall coming back.
    const boot::Standing stand = boot::stand_player_on_ground(run);
    check("fixture: the body stands on the shipped scene rather than falling to it",
          stand.placed, "no ground under the spawn column");
    check("fixture: ...with its feet on the surface",
          stand.placed && run.player.cell_y() + Player::HEIGHT == stand.surface,
          std::to_string(run.player.cell_y()) + " + " + std::to_string(Player::HEIGHT) +
              " vs surface " + std::to_string(stand.surface));

    // The objective's position stops being a line to read. A run with no objective
    // cannot be won, and the only thing that otherwise says so is a stderr warning
    // nobody sees unless they are looking.
    const int column = boot::default_objective_column(run.grid.get_width());
    const boot::Objective obj = boot::place_objective(run, column);
    check("fixture: the objective plants on the shipped scene",
          obj.placed && obj.x == column,
          "no ground under x=" + std::to_string(column));
    check("fixture: ...on ground rather than at the top of the world",
          obj.placed && obj.y > 0, std::to_string(obj.y));

    // The placed-prop count stops being a line to read. The regression this catches
    // is props authored against a ground line that is true of the floor slab and
    // false of everything standing on it.
    std::string prop_error;
    const std::vector<PropDef> defs =
        load_prop_list("assets/test_props.txt", &prop_error);
    check("fixture: the shipped prop list parses", prop_error.empty(), prop_error);
    check("fixture: ...and is not empty", !defs.empty());

    std::vector<int> widths(defs.size(), 0);
    bool every_sprite_read = true;
    for (size_t i = 0; i < defs.size(); ++i) {
        bmp::Image img;
        std::string img_error;
        if (bmp::read(("assets/" + defs[i].sprite + ".bmp").c_str(), img, &img_error)) {
            widths[i] = img.width;
        } else {
            every_sprite_read = false;
        }
    }
    check("fixture: every prop sprite the list names exists and reads",
          every_sprite_read);

    const boot::PlantingReport r = boot::plant_props(run.grid, defs, widths);
    check("fixture: every prop in the shipped list finds ground",
          r.planted.size() == defs.size(),
          std::to_string(r.planted.size()) + " of " + std::to_string(defs.size()) +
              " placed; " + std::to_string(r.no_ground.size()) + " over air, " +
              std::to_string(r.no_texture.size()) + " without a sprite");
}


// --- the authored layer stacks, against the art -----------------------
//
// The launch line reports how many of a stack's layers loaded, and
// layers.empty() silently restores the generated three-layer backdrop -- so a
// total load failure looks like the old backdrop working rather than the new one
// missing. That is a number a person has to notice is wrong.
//
// This demotes that check rather than replacing it, and the gap is worth
// stating: this suite reads the BMPs through bmp::read, where the game loads
// them through SDL_LoadBMP with a colour key. What is proved here is that every
// file exists, is the size the stack is stated in, and is described consistently
// by the table -- every failure mode that comes from a missing converter run, a
// renamed asset or an edited table. What is not proved is that SDL will accept
// them.
//
// The order properties are the other half, and reordering is the cheapest thing
// to get wrong here because nothing about a wrong order fails to compile.
//
// Every check below runs on every authored set, which is the point of bg1::SETS
// existing at all: the failure a second set invites is not a new kind, it is
// these same properties silently true of the table that was tested and false of
// the one that was copied.
void test_bg1_layer_stack() {
    check("bg1: there is at least one authored backdrop set", bg1::SET_COUNT > 0);

    for (int s = 0; s < bg1::SET_COUNT; ++s) {
        const bg1::Set& set = bg1::SETS[s];
        const std::string tag = std::string(set.scene) + ": ";
        const int n = set.layer_count;
        check((tag + "the layer stack is not empty").c_str(), n > 0);
        if (n <= 0) continue;

        // Every file present, and every one the size the factors are stated in. One
        // art pixel is one world cell, so a layer of a different size is not a
        // scaling question -- it is a layer that no longer lines up with the plane
        // the other eight are painting pieces of.
        for (int i = 0; i < n; ++i) {
            const bg1::Layer& l = set.layers[i];
            const std::string path = std::string(set.dir) + l.file;
            bmp::Image img;
            std::string err;
            const bool read_ok = bmp::read(path.c_str(), img, &err);
            check((tag + "layer " + l.file + " reads").c_str(), read_ok, err);
            if (!read_ok) continue;
            check((tag + "layer " + l.file + " is the size the stack is stated in").c_str(),
                  img.width == set.native_w && img.height == set.native_h,
                  std::to_string(img.width) + "x" + std::to_string(img.height));
        }

        // Back to front, in strict numeric-descending filename order -- the claim
        // the table's own header makes, and the one a reorder breaks silently.
        //
        // The index is read after the set's own name, which is the second check
        // hiding in this one: a set's files are named for the set, so a row copied
        // between two tables and left pointing at the other set's image fails here
        // rather than at a launch nobody is watching.
        const std::string prefix = std::string(set.scene) + "_";
        bool descending = true;
        int previous = 1 << 30;
        for (int i = 0; i < n; ++i) {
            const std::string f = set.layers[i].file;
            if (f.compare(0, prefix.size(), prefix) != 0 || f.size() < prefix.size() + 2) {
                descending = false;
                break;
            }
            const int index = std::atoi(f.substr(prefix.size(), 2).c_str());
            descending = descending && index > 0 && index < previous;
            previous = index;
        }
        check((tag + "the layers are named for the set and listed back to front, "
                     "in descending filename order").c_str(), descending);

        // Exactly one banded layer, and it is the ground plane.
        // test_bg1_ground_bands checks the band numbers against that BMP; if the
        // flag were on a different row, it would be checking one image and banding
        // another.
        int banded = 0;
        std::string banded_file;
        for (int i = 0; i < n; ++i)
            if (set.layers[i].banded) { ++banded; banded_file = set.layers[i].file; }
        check((tag + "exactly one layer is banded, and it is the ground plane").c_str(),
              banded == 1 && banded_file == prefix + "08_ground.bmp",
              std::to_string(banded) + " banded, on '" + banded_file + "'");

        // The sky is the only layer painted edge to edge, and it is the backmost.
        // Anything opaque in front of it hides everything behind it.
        int opaque = 0;
        for (int i = 0; i < n; ++i) if (set.layers[i].opaque) ++opaque;
        check((tag + "exactly one layer is opaque, and it is the backmost").c_str(),
              opaque == 1 && set.layers[0].opaque, std::to_string(opaque) + " opaque");

        // One foreground layer, and it is the frontmost row. A foreground layer
        // with anything listed after it would be drawn over by a layer that is
        // meant to be behind the player.
        int foreground = 0;
        for (int i = 0; i < n; ++i) if (set.layers[i].is_foreground) ++foreground;
        check((tag + "exactly one layer is foreground, and it is the frontmost").c_str(),
              foreground == 1 && set.layers[n - 1].is_foreground,
              std::to_string(foreground) + " foreground");

        // Nearer is faster, nothing exceeds 1.0, and the banded row carries the 0.0
        // sentinel rather than a fourth copy of a number the band table already
        // states three times. The cap is draw_backdrop_layer's coverage
        // inequality, which camera_test pins from both sides: above 1.0 a
        // world-sized layer leaves the clear colour showing at the world's edge.
        bool ladder = true;
        float last = 0.0f;
        for (int i = 0; i < n; ++i) {
            const bg1::Layer& l = set.layers[i];
            if (l.banded) {
                ladder = ladder && l.parallax_x == 0.0f;
                continue;
            }
            ladder = ladder && l.parallax_x > 0.0f && l.parallax_x <= 1.0f &&
                     l.parallax_x > last;
            last = l.parallax_x;
        }
        check((tag + "the unbanded factors increase toward the viewer and cap at "
                     "1.0, and the banded row carries the sentinel").c_str(), ladder);

        // Every scene name is a set name at most once. bg1::find walks this array
        // and returns the first hit, so a duplicate is a stack that can never be
        // reached and a table that reads as if it can.
        int named = 0;
        for (int j = 0; j < bg1::SET_COUNT; ++j)
            if (std::string(bg1::SETS[j].scene) == set.scene) ++named;
        check((tag + "the scene name appears once in the set table").c_str(), named == 1,
              std::to_string(named) + " rows");
        check((tag + "the set is what bg1::find returns for its own name").c_str(),
              bg1::find(set.scene) == &set);
    }

    check("bg1: a scene with no authored stack finds no set, rather than bg1's",
          bg1::find("empty") == nullptr && bg1::find(nullptr) == nullptr &&
              bg1::find("bg1_") == nullptr);

    // An extended set is the same nine depths of the same place, which is the one
    // claim its header makes that is not about its own art. A factor that drifts
    // makes it a different landscape in the same palette, and the drift would be
    // invisible: nothing about a wrong ladder fails to load.
    check("bg1_ext: the parallax ladder is bg1's, unchanged",
          bg1::EXT_LAYER_COUNT == bg1::LAYER_COUNT);
    if (bg1::EXT_LAYER_COUNT == bg1::LAYER_COUNT) {
        bool same = true;
        for (int i = 0; i < bg1::LAYER_COUNT; ++i)
            same = same && bg1::EXT_LAYERS[i].parallax_x == bg1::LAYERS[i].parallax_x &&
                   bg1::EXT_LAYERS[i].opaque == bg1::LAYERS[i].opaque &&
                   bg1::EXT_LAYERS[i].banded == bg1::LAYERS[i].banded &&
                   bg1::EXT_LAYERS[i].is_foreground == bg1::LAYERS[i].is_foreground;
        check("bg1_ext: every row carries bg1's factor and bg1's three flags", same);
    }

    // Same colours, layer by layer. The generator names no colour -- it counts each
    // source layer's pixels and paints only with what it found there -- so this
    // holds by construction today and the check is for the day somebody edits the
    // generator. Layer by layer rather than set-wide, because a generator that
    // painted the near hills in the far hills' brown would pass a set-wide
    // comparison while inverting the aerial perspective the art carries instead of a
    // grade.
    //
    // The colour key is excluded on both sides: it is "no pixel here", not paint.
    {
        auto palette_of = [](const bmp::Image& img) {
            std::set<uint32_t> out;
            for (uint32_t p : img.pixels)
                if ((p & 0xFFFFFFu) != 0xFF00FFu) out.insert(p & 0xFFFFFFu);
            return out;
        };
        for (int i = 0; i < bg1::LAYER_COUNT && i < bg1::EXT_LAYER_COUNT; ++i) {
            bmp::Image a, b;
            const std::string pa = std::string(bg1::LAYER_DIR) + bg1::LAYERS[i].file;
            const std::string pb = std::string(bg1::EXT_LAYER_DIR) + bg1::EXT_LAYERS[i].file;
            if (!bmp::read(pa.c_str(), a, nullptr) || !bmp::read(pb.c_str(), b, nullptr))
                continue;  // the read itself is already checked above
            const std::set<uint32_t> want = palette_of(a), got = palette_of(b);
            check((std::string("bg1_ext: ") + bg1::EXT_LAYERS[i].file +
                   " is painted in exactly the colours of " + bg1::LAYERS[i].file).c_str(),
                  want == got,
                  std::to_string(got.size()) + " colours against " +
                      std::to_string(want.size()));
        }
    }

    // The extension is twice the base on both axes -- the fact the whole art
    // pipeline is stated against, and the one a re-generated set at a third size
    // would break here rather than at the seam between the band table and the BMP.
    check("bg1_ext: the world is exactly twice bg1's on both axes",
          bg1::EXT_NATIVE_W == bg1::NATIVE_W * 2 && bg1::EXT_NATIVE_H == bg1::NATIVE_H * 2,
          std::to_string(bg1::EXT_NATIVE_W) + "x" + std::to_string(bg1::EXT_NATIVE_H));

    // The bands moved down by the frame's growth and did not change shape. That is
    // the vertical rule the art was generated under -- bottom-anchored art keeps its
    // distance from the bottom -- and it is what makes the extension's standing view
    // the same rows as the base set's. A band retuned on one set and not the other
    // is two ground planes receding at two rates.
    const int shift = bg1::EXT_NATIVE_H - bg1::NATIVE_H;
    const int bands = static_cast<int>(std::size(bg1::GROUND_BANDS));
    bool shifted = static_cast<int>(std::size(bg1::EXT_GROUND_BANDS)) == bands;
    for (int i = 0; shifted && i < bands; ++i)
        shifted = bg1::EXT_GROUND_BANDS[i].parallax_x == bg1::GROUND_BANDS[i].parallax_x &&
                  bg1::EXT_GROUND_BANDS[i].row1 == bg1::GROUND_BANDS[i].row1 + shift &&
                  (i == 0 ? bg1::EXT_GROUND_BANDS[i].row0 == 0
                          : bg1::EXT_GROUND_BANDS[i].row0 ==
                                bg1::GROUND_BANDS[i].row0 + shift);
    check("bg1_ext: the ground bands are bg1's, at the same factors, moved down "
          "by the frame's growth",
          shifted);
}

// --- the ground plane's band table, against the art --------------
//
// The only enforcement the banding has, and it is worth having because the
// numbers it guards look arbitrary and are not. The ground layer scrolls as
// three bands at three different rates, and a boundary between two bands is a
// horizontal discontinuity in scroll offset -- so it is invisible only where the
// art either side of it is flat: the rows meeting at the cut must be uniform
// across every column and the same colour as each other, or a step appears in
// the shoreline and slides as the camera moves.
//
// No headless suite composes an authored frame, so nothing can check what the
// band table looks like. What can be checked is the property the boundaries were
// chosen for -- the one a later edit would break without noticing, because
// moving a boundary two rows costs nothing and shows nothing until somebody
// walks.
void test_bg1_ground_bands() {
    for (int s = 0; s < bg1::SET_COUNT; ++s) {
        const bg1::Set& set = bg1::SETS[s];
        const std::string tag = std::string(set.scene) + ": ";

        // The banded row, found through the flag rather than by filename -- the
        // same reason Layer::banded is a field. test_bg1_layer_stack has already
        // required exactly one.
        const bg1::Layer* ground = nullptr;
        for (int i = 0; i < set.layer_count; ++i)
            if (set.layers[i].banded) ground = &set.layers[i];
        check((tag + "the set has a banded layer to check the bands against").c_str(),
              ground != nullptr);
        if (!ground) continue;

        bmp::Image img;
        std::string err;
        const std::string path = std::string(set.dir) + ground->file;
        const bool read_ok = bmp::read(path.c_str(), img, &err);
        check((tag + "the ground layer's BMP reads").c_str(), read_ok, err);
        if (!read_ok) continue;

        check((tag + "the ground layer is the size the band table is stated in").c_str(),
              img.width == set.native_w && img.height == set.native_h,
              std::to_string(img.width) + "x" + std::to_string(img.height));
        if (img.width != set.native_w || img.height != set.native_h) continue;

        const int n = set.band_count;

        // Contiguous, in order, covering every row exactly once. A gap would leave
        // a strip of the sky showing through the plane; an overlap would draw one
        // range twice at two offsets.
        bool contiguous = set.bands[0].row0 == 0 && set.bands[n - 1].row1 == img.height;
        for (int i = 1; i < n; ++i)
            contiguous = contiguous && set.bands[i].row0 == set.bands[i - 1].row1;
        check((tag + "the ground bands tile the layer with no gap and no overlap").c_str(),
              contiguous);

        // Nearer is faster, and nothing exceeds 1.0 -- draw_backdrop_layer's
        // coverage inequality, which a factor above 1.0 breaks by leaving the clear
        // colour at the world's edge.
        bool ordered = true;
        for (int i = 0; i < n; ++i) {
            const float f = set.bands[i].parallax_x;
            ordered = ordered && f > 0.0f && f <= 1.0f;
            if (i > 0) ordered = ordered && f > set.bands[i - 1].parallax_x;
        }
        check((tag + "the ground bands' factors increase toward the viewer and "
                     "cap at 1.0").c_str(), ordered);

        // The one that names the defect. For every interior boundary, the last row
        // of the band above and the first row of the band below must each be one
        // colour across every column, and the same colour.
        auto uniform_colour = [&](int row, uint32_t& out) {
            out = img.pixels[static_cast<size_t>(row) * static_cast<size_t>(img.width)];
            for (int x = 1; x < img.width; ++x)
                if (img.pixels[static_cast<size_t>(row) * static_cast<size_t>(img.width) + static_cast<size_t>(x)] != out)
                    return false;
            return true;
        };

        for (int i = 1; i < n; ++i) {
            const int cut = set.bands[i].row0;
            uint32_t above = 0, below = 0;
            const bool a = uniform_colour(cut - 1, above);
            const bool b = uniform_colour(cut, below);
            const bool flat = a && b && above == below;
            check((tag + "every ground band boundary falls on flat paint, so its "
                         "scroll step cannot be seen").c_str(),
                  flat,
                  flat ? std::string()
                       : "art row " + std::to_string(cut) + ": " +
                             (!a ? "the row above the cut is not uniform"
                                 : (!b ? "the row below the cut is not uniform"
                                       : "the two rows differ in colour")));
        }
    }
}

// The resolve half of scene activation.
//
// These are the two rules that were prose at the call site: the size precedence
// and the rebuild-only-when-the-scale-changed test, each argued in a comment
// inside activate_scene and checked by nobody. Everything below fails if either
// rule is reordered.
void test_scene_resolve() {
    scene_list::SceneDef def;
    def.name = "t";
    def.scale = 4;

    // The row is silent and the scene names no art: the engine default, which is
    // the only one of the three answers that is not a fact about the scene.
    {
        const scene_activation::Resolved r =
            scene_activation::resolve(def, 0, 0, 1920, 1080, 4);
        check("resolve: a silent row with no art falls back to the engine default",
              r.world_w == 1920 && r.world_h == 1080);
    }

    // The BMP beats the default.
    {
        const scene_activation::Resolved r =
            scene_activation::resolve(def, 344, 144, 1920, 1080, 4);
        check("resolve: the material BMP's own size beats the engine default",
              r.world_w == 344 && r.world_h == 144);
    }

    // The row beats the BMP, and this is the ordering that matters most: a scene may
    // legitimately be larger than the image stamped into its corner, so an author
    // who wrote a size meant it.
    {
        scene_list::SceneDef sized = def;
        sized.custom_width = 3840;
        sized.custom_height = 1440;
        const scene_activation::Resolved r =
            scene_activation::resolve(sized, 1920, 1080, 640, 400, 4);
        check("resolve: a stated size beats the material BMP's",
              r.world_w == 3840 && r.world_h == 1440);
    }

    // The two axes resolve independently, because custom_width and custom_height are
    // 0-means-unstated separately even though the loader requires them as a pair -- a
    // future row form that states one is not allowed to silently take the other from
    // a different answer.
    {
        scene_list::SceneDef half = def;
        half.custom_width = 500;
        const scene_activation::Resolved r =
            scene_activation::resolve(half, 344, 144, 1920, 1080, 4);
        check("resolve: width and height take their answers independently",
              r.world_w == 500 && r.world_h == 144);
    }

    // `infinite` is carried through from the row and is not inferred from anything
    // else.
    {
        scene_list::SceneDef inf = def;
        inf.mode = scene_list::SceneMode::Infinite;
        check("resolve: the mode column is carried through, fixed",
              !scene_activation::resolve(def, 0, 0, 1920, 1080, 4).infinite);
        check("resolve: the mode column is carried through, infinite",
              scene_activation::resolve(inf, 0, 0, 1920, 1080, 4).infinite);
    }

    // scale_changed is false when the scale did not move, so apply_mode is not
    // re-run between two scenes that share a scale -- it destroys and rebuilds two
    // textures and a light field otherwise. And it is true in both directions, which
    // a caller that only watched for an increase would get wrong on the way back.
    {
        scene_list::SceneDef ten = def;
        ten.scale = 10;
        const scene_activation::Resolved same =
            scene_activation::resolve(def, 0, 0, 1920, 1080, 4);
        const scene_activation::Resolved up =
            scene_activation::resolve(ten, 0, 0, 1920, 1080, 4);
        const scene_activation::Resolved down =
            scene_activation::resolve(def, 0, 0, 1920, 1080, 10);
        check("resolve: an unchanged scale does not ask for a rebuild",
              !same.scale_changed && same.scale == 4);
        check("resolve: a scale that rises asks for a rebuild",
              up.scale_changed && up.scale == 10);
        check("resolve: a scale that falls asks for one too",
              down.scale_changed && down.scale == 4);
    }
}

// The same function against the rows the game actually ships, so a change to
// assets/scenes.txt that breaks the world's size fails ctest rather than being
// read off the Scene: line at launch.
void test_scene_resolve_shipped_rows() {
    std::string error;
    const std::vector<scene_list::SceneDef> scenes =
        scene_list::load_scene_list("assets/scenes.txt", &error);
    check("resolve: the shipped scene list loads", !scenes.empty(), error);
    if (scenes.empty()) return;

    for (const scene_list::SceneDef& def : scenes) {
        // No art is read here: every shipped row states its own size, which is the
        // property being asserted. A row that stopped stating one would resolve to
        // the engine default and fail below.
        const scene_activation::Resolved r = scene_activation::resolve(
            def, 0, 0, boot::GRID_WIDTH, boot::GRID_HEIGHT, def.scale);
        if (def.name == "empty") {
            check("resolve: 'empty' is a 1920x1080 infinite world at 4x",
                  r.world_w == 1920 && r.world_h == 1080 && r.infinite &&
                      r.scale == 4);
        } else if (def.name == "bg1") {
            // One art pixel to one world cell, at the scale that makes the widest
            // window show exactly the reference frame. bg1_backdrop.h asserts the art
            // is that size; this asserts the world is.
            check("resolve: 'bg1' is a 344x144 fixed world at 10x, one art "
                  "pixel to one world cell",
                  r.world_w == bg1::NATIVE_W && r.world_h == bg1::NATIVE_H &&
                      !r.infinite && r.scale == 10);
        } else if (def.name == "bg1_ext") {
            // The same mapping and the same scale, over twice the world. The scale is
            // the assertion worth having: a row that resized the world and rescaled it
            // would keep the art filling the window and quietly halve the player
            // against the scene, which is the one way an extension stops being an
            // extension.
            check("resolve: 'bg1_ext' is a 688x288 fixed world at bg1's 10x, "
                  "still one art pixel to one world cell",
                  r.world_w == bg1::EXT_NATIVE_W && r.world_h == bg1::EXT_NATIVE_H &&
                      !r.infinite && r.scale == 10);
        } else if (def.name == "bg_gemini") {
            check("resolve: 'bg_gemini' is a 688x288 fixed world at 10x",
                  r.world_w == bg1::EXT_NATIVE_W && r.world_h == bg1::EXT_NATIVE_H &&
                      !r.infinite && r.scale == 10);
        }
    }
}

}  // namespace

int main() {
    test_terrain_surface();
    test_lowest_surface_under();
    test_place_objective();
    test_plant_props();
    test_stand_player_on_ground();
    test_stand_player_on_floor();
    test_choose_display_mode();
    test_shipped_fixture();
    test_bg1_layer_stack();
    test_bg1_ground_bands();
    test_scene_resolve();
    test_scene_resolve_shipped_rows();
    return report();
}
