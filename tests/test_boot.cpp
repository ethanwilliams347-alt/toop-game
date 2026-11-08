// The startup decisions, headless.
//
// Everything asserted below used to run inside main(), print a line to stdout,
// and be checkable only by a person reading the line at launch.
//
// Two halves. The unit half covers the decisions in game/boot.h against worlds
// this file builds, and the window decisions in game/display.h -- which size
// and mode to open at, the lists the settings menu offers, settings.txt. The
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
#include <memory>
#include <string>
#include <vector>
#include "game/boot.h"
#include "game/display.h"
#include "game/run.h"
#include "game/scene_activation.h"
#include "physics/grid.h"
#include "scene/bmp.h"
#include "scene/props.h"
#include "scene/scene.h"
#include "scene/scene_list.h"
#include "scene/sprites.h"
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
        const auto run_owner = std::make_unique<Run>(64, 64);
        Run& run = *run_owner;
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
        const auto run_owner = std::make_unique<Run>(64, 64);
        Run& run = *run_owner;
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

// settings.txt is written by the menu and may be edited by hand, and it has
// outlived one format already: a file from before display= and vsync= holds
// resolution= alone (perhaps with renderer=), and has to load as the windowed
// size it always meant. A rewrite that dropped a line it did not recognise would
// lose a setting some later feature put there, by touching a different one.
void test_settings_file() {
    const char* path = "boot_test_settings.tmp";
    auto write = [&](const char* text) {
        std::FILE* f = std::fopen(path, "w");
        std::fputs(text, f);
        std::fclose(f);
    };

    write("# old header\nresolution=2560x1440\n");
    {
        const DisplaySettings s = load_settings(path);
        check("settings: a file with only resolution= still loads its size",
              s.size == DisplayMode{2560, 1440});
        check("settings: ...as a window, with VSync on and the SDL renderer",
              s.window == WindowMode::Windowed && s.vsync && s.renderer == RendererChoice::Sdl);
    }
    write("resolution=1920x1080\nrenderer=gl\n");
    check("settings: the old format's renderer= is read",
          load_settings(path).renderer == RendererChoice::Gl);

    write("resolution=1x1\nrenderer=sdl\nfuture_key=7\n");
    const DisplaySettings want{WindowMode::Borderless, {3440, 1440}, false, RendererChoice::Gl};
    check("settings: saving succeeds", save_settings(want, path));
    check("settings: what was saved loads back, every field", load_settings(path) == want);
    {
        std::string text;
        if (std::FILE* f = std::fopen(path, "r")) {
            char line[128];
            while (std::fgets(line, sizeof(line), f)) text += line;
            std::fclose(f);
        }
        check("settings: a line the game does not know survives a save",
              text.find("future_key=7\n") != std::string::npos, text);
        check("settings: a rewritten key appears once, not beside its old value",
              text.find("resolution=") == text.rfind("resolution=") &&
                  text.find("resolution=1x1") == std::string::npos,
              text);
    }

    for (WindowMode m : {WindowMode::Windowed, WindowMode::Borderless, WindowMode::Fullscreen}) {
        const std::string line = std::string("display=") + window_mode_name(m) + "\r\n";
        write(line.c_str());
        check(("settings: " + line.substr(0, line.size() - 2) + " reads back").c_str(),
              load_settings(path).window == m);
    }
    write("vsync=off\n");
    check("settings: vsync=off is read", !load_settings(path).vsync);

    // Every value wrong. Each key falls back to its default on its own, rather
    // than the whole file being refused: there is nothing a player could do about
    // an error at launch that the menu cannot do better.
    write("display=maximised\nvsync=maybe\nrenderer=vulkan\nresolution=0x900\n");
    check("settings: unreadable values are the defaults", load_settings(path) == DisplaySettings{});

    std::remove(path);
    check("settings: no file is the defaults", load_settings(path) == DisplaySettings{});

    RendererChoice c = RendererChoice::Sdl;
    check("settings: the command-line spelling parses",
          parse_renderer_choice("gl", c) && c == RendererChoice::Gl);
    check("settings: a bad value is refused and changes nothing",
          !parse_renderer_choice("g", c) && c == RendererChoice::Gl);
}

// A 3440x1440 desktop with a taskbar: 1392 usable rows, and a title bar to fit
// in them as well, so the windowed list stops at 2560x1080 -- which is why
// playing at 3440x1440 means borderless or fullscreen.
ScreenOptions ultrawide() {
    ScreenOptions o;
    o.desktop = DisplayMode{3440, 1440};
    for (int i = 0; i < COMMON_SIZE_COUNT; ++i)
        o.windowed_fits[static_cast<size_t>(i)] =
            COMMON_SIZES[i].window_w <= 3440 && COMMON_SIZES[i].window_h + 31 <= 1392;
    o.fullscreen = fullscreen_sizes({{{3440, 1440}, 100},
                                     {{3440, 1440}, 144},
                                     {{2560, 1080}, 60},
                                     {{1920, 1080}, 144},
                                     {{640, 480}, 60}});
    return o;
}

void test_fullscreen_sizes() {
    const std::vector<FullscreenMode> out = fullscreen_sizes({{{1920, 1080}, 60},
                                                              {{640, 480}, 60},
                                                              {{1920, 1080}, 144},
                                                              {{3440, 1440}, 100},
                                                              {{1280, 1024}, 75},
                                                              {{1920, 1080}, 120},
                                                              {{1280, 720}, 60}});
    check("fullscreen_sizes: one entry per size, and nothing under 1280x720", out.size() == 4,
          std::to_string(out.size()) + " entries");
    if (out.size() == 4) {
        check("fullscreen_sizes: smallest first, by width then height",
              out[0].size == DisplayMode{1280, 720} && out[1].size == DisplayMode{1280, 1024} &&
                  out[2].size == DisplayMode{1920, 1080} && out[3].size == DisplayMode{3440, 1440});
        check("fullscreen_sizes: each size at its highest refresh rate",
              out[2].refresh_hz == 144 && out[1].refresh_hz == 75);
    }
}

void test_resolution_lists() {
    ScreenOptions o;
    o.desktop = DisplayMode{1920, 1080};
    for (int i = 0; i < COMMON_SIZE_COUNT; ++i)
        o.windowed_fits[static_cast<size_t>(i)] = COMMON_SIZES[i].window_h <= 900;
    o.fullscreen = {{{1280, 720}, 60}, {{1920, 1080}, 144}};

    const std::vector<DisplayMode> windowed = resolutions(WindowMode::Windowed, o);
    check("resolutions: windowed is the common sizes that fit",
          windowed.size() == 3 && windowed.back() == DisplayMode{1600, 900});
    check("resolutions: borderless is the desktop alone",
          resolutions(WindowMode::Borderless, o) == std::vector<DisplayMode>{{1920, 1080}});
    check("resolutions: fullscreen is the display's modes",
          resolutions(WindowMode::Fullscreen, o).size() == 2);

    check("fit_size: a size the mode offers is kept",
          fit_size(WindowMode::Windowed, {1366, 768}, o) == DisplayMode{1366, 768});
    // Borderless -> windowed on a desktop the window cannot fill: the largest
    // window that fits, not the desktop's size carried into a window.
    check("fit_size: a size the mode lacks becomes the largest no larger",
          fit_size(WindowMode::Windowed, {1920, 1080}, o) == DisplayMode{1600, 900});
    check("fit_size: ...and a fullscreen size the display lacks likewise",
          fit_size(WindowMode::Fullscreen, {1600, 900}, o) == DisplayMode{1280, 720});
    check("fit_size: smaller than everything offered is the smallest",
          fit_size(WindowMode::Fullscreen, {1000, 600}, o) == DisplayMode{1280, 720});
    check("fit_size: borderless is the desktop whatever was asked",
          fit_size(WindowMode::Borderless, {1280, 720}, o) == o.desktop);

    // Neither list is ever empty: a row with nothing on it is a menu that cannot
    // say what the window is.
    ScreenOptions bare;
    bare.desktop = DisplayMode{1024, 600};
    check("resolutions: no fullscreen modes reported is the desktop alone",
          resolutions(WindowMode::Fullscreen, bare) == std::vector<DisplayMode>{{1024, 600}});
    check("resolutions: no window fitting is the smallest window anyway",
          resolutions(WindowMode::Windowed, bare) == std::vector<DisplayMode>{COMMON_SIZES[0]});

    // A window the system shrank to nothing still draws its text.
    check("ui_scale: never below 1", DisplayMode{640, 200}.ui_scale() == 1);
    check("ui_scale: 5 at 1440 rows, as the 21:9 overlay case is drawn",
          WIDEST_VIEW.ui_scale() == 5);
}

void test_choose_start() {
    const ScreenOptions o = ultrawide();
    auto start = [&](WindowMode m, DisplayMode size) {
        DisplaySettings s;
        s.window = m;
        s.size = size;
        return choose_start(s, o);
    };
    {
        const StartChoice c = start(WindowMode::Windowed, {0, 0});
        check("choose_start: nothing stored opens at the largest window that fits",
              c.settings.size == DisplayMode{2560, 1080} && c.why == StartChoice::Why::Largest);
    }
    {
        const StartChoice c = start(WindowMode::Windowed, {1920, 1080});
        check("choose_start: a stored window size that fits is kept",
              c.settings.size == DisplayMode{1920, 1080} && c.why == StartChoice::Why::Stored);
    }
    {
        // The settings.txt of somebody who chose 4K on another monitor.
        const StartChoice c = start(WindowMode::Windowed, {3840, 2160});
        check("choose_start: a stored window too big for this desktop falls to the largest",
              c.settings.window == WindowMode::Windowed &&
                  c.settings.size == DisplayMode{2560, 1080} &&
                  c.why == StartChoice::Why::StoredTooBig);
    }
    {
        // The pre-display= file of somebody who chose 3440x1440 when the old menu
        // offered it as a window -- on this very monitor, to fill it.
        const StartChoice c = start(WindowMode::Windowed, {3440, 1440});
        check("choose_start: a stored window the desktop's exact size opens borderless",
              c.settings.window == WindowMode::Borderless && c.settings.size == WIDEST_VIEW &&
                  c.why == StartChoice::Why::DesktopWindow);
    }
    {
        const StartChoice c = start(WindowMode::Borderless, {1280, 720});
        check("choose_start: borderless is the desktop's size",
              c.settings.size == WIDEST_VIEW && c.why == StartChoice::Why::Stored);
    }
    {
        const StartChoice c = start(WindowMode::Fullscreen, {2560, 1080});
        check("choose_start: a stored fullscreen mode the display has is kept",
              c.settings.size == DisplayMode{2560, 1080} && c.why == StartChoice::Why::Stored);
    }
    {
        // A different monitor than last time.
        const StartChoice c = start(WindowMode::Fullscreen, {1600, 900});
        check("choose_start: a fullscreen size the display lacks opens at the desktop's",
              c.settings.window == WindowMode::Fullscreen && c.settings.size == WIDEST_VIEW &&
                  c.why == StartChoice::Why::NotAMode);
    }
    {
        const StartChoice c = start(WindowMode::Fullscreen, {0, 0});
        check("choose_start: fullscreen with no size stored is the desktop's, unremarked",
              c.settings.size == WIDEST_VIEW && c.why == StartChoice::Why::Stored);
    }
    {
        ScreenOptions tiny;
        tiny.desktop = DisplayMode{1024, 600};
        DisplaySettings s;
        s.size = DisplayMode{1920, 1080};
        const StartChoice c = choose_start(s, tiny);
        check("choose_start: nothing fitting still opens, at the smallest window",
              c.settings.size == COMMON_SIZES[0] && c.why == StartChoice::Why::NothingFits);
    }
    {
        DisplaySettings s;
        s.window = WindowMode::Borderless;
        s.vsync = false;
        s.renderer = RendererChoice::Gl;
        const StartChoice c = choose_start(s, o);
        check("choose_start: VSync and the renderer pass through untouched",
              !c.settings.vsync && c.settings.renderer == RendererChoice::Gl);
    }
}

// The empty scene's spawn. A world with no terrain at all is a legitimate scene
// -- assets/scenes.txt ships one so the backdrop can be looked at with nothing
// standing in front of it -- and the body has to end up somewhere real in it.
void test_stand_player_on_floor() {
    // No terrain stamped: this world is entirely Empty.
    const auto run_owner = std::make_unique<Run>(40, 40);
    Run& run = *run_owner;
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
        const auto run_owner = std::make_unique<Run>(40, 40);
        Run& run = *run_owner;
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
        const auto run_owner = std::make_unique<Run>(40, 40);
        Run& run = *run_owner;
        const int before = run.player.cell_y();
        const boot::Standing s = boot::stand_player_on_ground(run);
        check("stand_player_on_ground: no ground means no placement", !s.placed);
        check("stand_player_on_ground: ...and the body is not moved to a guess",
              run.player.cell_y() == before);
    }
}

// The free first landing has to survive being stood on the ground. A placed
// body that does not know it is grounded counts its first step as a landing and
// spends the free one there, so the first real fall -- the one it was kept for --
// costs health.
//
// Two drops, each well past the safe landing speed (90 and 140 cells, against a
// safe speed reached after about 58): the first must be free and the second must
// not, which proves the rule is live rather than switched off.
void test_free_first_landing() {
    // Wide, because the body keeps walking while it falls: a 90-cell drop at the
    // walk speed carries it about 70 cells sideways, and the 140-cell one further.
    const auto run_owner = std::make_unique<Run>(320, 300);
    Run& run = *run_owner;
    for (int x = 0; x < 20; ++x) run.grid.set_element(x, 60, ElementType::Wall);    // the ledge
    for (int x = 20; x < 130; ++x)
        run.grid.set_element(x, 150, ElementType::Wall); // a shelf below
    for (int x = 0; x < 320; ++x) run.grid.set_element(x, 290, ElementType::Wall);  // the floor
    run.player = Player(4, 0);
    const boot::Standing s = boot::stand_player_on_ground(run);
    check("free landing: the body stands on the ledge", s.placed && s.surface == 60,
          std::to_string(s.surface));

    Input right;
    right.right = true;
    int on_shelf = -1, on_floor = -1;
    for (int i = 0; i < 600 && on_floor < 0; ++i) {
        run.step(right);
        const int feet = run.player.cell_y() + Player::HEIGHT;
        if (on_shelf < 0 && run.player.is_on_ground() && feet == 150)
            on_shelf = run.player.health();
        if (run.player.is_on_ground() && feet == 290) on_floor = run.player.health();
    }
    check("free landing: the first hard landing after a placed spawn is free",
          on_shelf == Player::MAX_HEALTH, std::to_string(on_shelf));
    check("free landing: ...and the second one is not", on_floor >= 0 && on_floor < on_shelf,
          std::to_string(on_floor));
}

// --- the fixture half: the launch check, as assertions ---

void test_shipped_fixture() {
    std::string error;
    Scene scene = bmp::load("tests/fixtures/test_material.bmp", "tests/fixtures/test_albedo.bmp", &error);
    if (!error.empty()) {
        check("fixture: the shipped scene loads", false, error);
        return;
    }

    const auto run_owner = std::make_unique<Run>(boot::GRID_WIDTH, boot::GRID_HEIGHT);
    Run& run = *run_owner;
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
    check("fixture: the objective plants on the shipped scene", obj.placed && obj.x == column,
          "no ground under x=" + std::to_string(column));
    check("fixture: ...on ground rather than at the top of the world",
          obj.placed && obj.y > 0, std::to_string(obj.y));

    // The placed-prop count stops being a line to read. The regression this catches
    // is props authored against a ground line that is true of the floor slab and
    // false of everything standing on it.
    std::string prop_error;
    const std::vector<PropDef> defs =
        load_prop_list("tests/fixtures/test_props.txt", &prop_error);
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

    // The player sheet's fallback, which is what the game draws when sprites.txt
    // is deleted or ignored. A fallback naming a file that is not shipped turns the
    // player into a white rectangle with nothing failing but a stderr line.
    {
        bmp::Image img;
        std::string img_error;
        const std::string sheet = std::string("assets/") + PLAYER_SHEET_FILE;
        check("fixture: the player sheet's fallback file is shipped and reads",
              bmp::read(sheet.c_str(), img, &img_error), img_error);
    }

    const boot::PlantingReport r = boot::plant_props(run.grid, defs, widths);
    check("fixture: every prop in the shipped list finds ground",
          r.planted.size() == defs.size(),
          std::to_string(r.planted.size()) + " of " + std::to_string(defs.size()) +
              " placed; " + std::to_string(r.no_ground.size()) + " over air, " +
              std::to_string(r.no_texture.size()) + " without a sprite");
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

    // `unclamped` is carried through from the row and is not inferred from anything
    // else.
    {
        scene_list::SceneDef inf = def;
        inf.mode = scene_list::SceneMode::Unclamped;
        check("resolve: the mode column is carried through, fixed",
              !scene_activation::resolve(def, 0, 0, 1920, 1080, 4).unclamped);
        check("resolve: the mode field is carried through, unclamped",
              scene_activation::resolve(inf, 0, 0, 1920, 1080, 4).unclamped);
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
            check("resolve: 'empty' is a 1920x1080 unclamped world at 4x",
                  r.world_w == 1920 && r.world_h == 1080 && r.unclamped && r.scale == 4);
        } else if (def.name == "bg_valley") {
            // One art pixel to one world cell, at the scale that makes the widest
            // window show exactly the reference frame. backdrop_set_test asserts the
            // art is that size; this asserts the world is.
            check("resolve: 'bg_valley' is a 344x144 fixed world at 10x, one art "
                  "pixel to one world cell",
                  r.world_w == 344 && r.world_h == 144 && !r.unclamped && r.scale == 10);
        } else if (def.name == "bg_valley_wide") {
            // The same mapping and the same scale, over twice the world. The scale is
            // the assertion worth having: a row that resized the world and rescaled it
            // would keep the art filling the window and quietly halve the player
            // against the scene, which is the one way an extension stops being an
            // extension.
            check("resolve: 'bg_valley_wide' is a 688x288 fixed world at bg_valley's 10x, "
                  "still one art pixel to one world cell",
                  r.world_w == 688 && r.world_h == 288 && !r.unclamped && r.scale == 10);
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
    test_free_first_landing();
    test_choose_display_mode();
    test_settings_file();
    test_fullscreen_sizes();
    test_resolution_lists();
    test_choose_start();
    test_shipped_fixture();
    test_scene_resolve();
    test_scene_resolve_shipped_rows();
    return report();
}
