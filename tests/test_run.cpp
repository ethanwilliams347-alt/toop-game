// Run tests.
//
// Drives player, tool and grid together through one entry point rather than one
// at a time, because Run::step() is itself the thing under test here. Every
// scenario goes through run.step(input), the same call main.cpp makes once per
// fixed step.

#include "game/run.h"
#include "game/input_log.h"
#include "test_util.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

const Input NOTHING;

Input held_right() {
    Input in;
    in.right = true;
    return in;
}

// A flat Wall floor spanning the grid, top surface at `floor_y`.
void build_floor(Run& run, int floor_y) {
    for (int x = 0; x < run.grid.get_width(); ++x)
        run.grid.set_element(x, floor_y, ElementType::Wall);
}

int count_of(const Grid& g, ElementType t) {
    int n = 0;
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < g.get_width(); ++x)
            if (g.get_element(x, y).type == t) n++;
    return n;
}

void step(Run& run, const Input& input, int n) {
    for (int i = 0; i < n; ++i) run.step(input);
}

// Same shape as test_grid.cpp's worlds_match, duplicated rather than shared:
// each test file owns its own small helpers, and this is three lines.
bool worlds_match(const Grid& a, const Grid& b) {
    if (a.get_pixels() != b.get_pixels()) return false;
    for (int y = 0; y < a.get_height(); ++y) {
        for (int x = 0; x < a.get_width(); ++x) {
            const Element ea = a.get_element(x, y);
            const Element eb = b.get_element(x, y);
            if (ea.type != eb.type || ea.color != eb.color ||
                ea.updated_tag != eb.updated_tag || ea.ticks != eb.ticks)
                return false;
        }
    }
    return true;
}

} // namespace

int main() {
    // --- a scripted sequence: land, walk, jump, dig ---
    // Nothing here freezes the grid or calls a subsystem directly -- every effect
    // happens by handing run.step() an Input, the same as a real session would.
    {
        // Wide enough that a second of holding right never reaches the far wall.
        // The dig target below is placed relative to wherever the player ends up
        // and needs real headroom on both sides, or it silently lands outside the
        // grid, where get_element() reads the border itself as Wall regardless of
        // what was actually dug -- so the "dig target is actually solid" guard
        // would go on passing against the border while the dig assertion under it
        // failed.
        Run run(400, 60, 7);
        build_floor(run, 50);

        // The player spawns mid-air; let it land before asserting anything about
        // walking.
        step(run, NOTHING, 60);
        check("the player lands on the floor", run.player.is_on_ground(),
              "cell_y=" + std::to_string(run.player.cell_y()));

        const int start_x = run.player.cell_x();
        step(run, held_right(), 60);
        check("holding right through run.step() walks the player right",
              run.player.cell_x() > start_x,
              "start=" + std::to_string(start_x) + " now=" + std::to_string(run.player.cell_x()));

        const int rest_y = run.player.cell_y();
        Input jump;
        jump.jump = true;
        run.step(jump);
        // Not asserting is_on_ground() here as well: leaving a floor takes one
        // whole cell of upward movement, and whether a single step's rem_y carries
        // that far is a detail of Player's own integration, covered by its own
        // tests. The velocity flip is the part that belongs to this step's input
        // handling.
        check("one jump step applies an upward impulse", run.player.velocity_y() < 0,
              "vel_y=" + std::to_string(run.player.velocity_y()));

        int highest = run.player.cell_y();
        for (int i = 0; i < 120; ++i) {
            run.step(NOTHING);
            if (run.player.cell_y() < highest) highest = run.player.cell_y();
        }
        check("the jump clears the ground and the player lands again",
              rest_y - highest >= 8 && run.player.is_on_ground(),
              "peak=" + std::to_string(rest_y - highest) + " cells");

        // A Wall block near wherever the player ended up, close enough to sit well
        // inside DigTool::RANGE without depending on an exact landing spot. Clear
        // of the far side of the body, not just of its origin: a target placed
        // inside the player would be dug from a centre that is already touching
        // it.
        const int wx = run.player.cell_x() + Player::WIDTH + 6;
        const int wy = run.player.center_y();
        for (int y = wy - 3; y <= wy + 3; ++y)
            for (int x = wx; x <= wx + 4; ++x)
                run.grid.set_element(x, y, ElementType::Wall);
        check("the dig target is actually solid",
              run.grid.get_element(wx + 2, wy).type == ElementType::Wall);

        Input dig;
        dig.dig = true;
        dig.cursor_x = wx + 2;
        dig.cursor_y = wy;
        run.step(dig);
        check("holding dig through run.step() removes solid terrain",
              run.grid.get_element(wx + 2, wy).type != ElementType::Wall);
    }

    // --- the brush is no longer a special case ---
    // Painted through run.step() rather than applied outside the loop: nothing about
    // placing a cell happens anywhere but inside a fixed step.
    {
        Run run(40, 40, 11);
        Input paint;
        paint.brush_active = true;
        paint.brush_type = ElementType::Sand;
        paint.brush_size = 2;
        paint.cursor_x = 20;
        paint.cursor_y = 20;
        run.step(paint);

        check("the brush reaches the grid through run.step()",
              run.grid.get_element(20, 20).type == ElementType::Sand);
        check("the brush paints a circle, not a single cell",
              run.grid.get_element(21, 20).type == ElementType::Sand &&
              run.grid.get_element(20, 21).type == ElementType::Sand);
    }

    // --- the brush displaces what it lands on instead of destroying it ---
    // The symptom is material spawned into water capping the water on top and then
    // bursting outward on release; the cause is the brush deleting every water cell
    // in its disc. So the assertion is about volume and not about where any
    // particular cell ended up: count the water, drag the sand brush through it,
    // count again. A cell of water may be anywhere afterwards -- that is the pool
    // finding its level -- but there must still be as much of it.
    {
        Run run(60, 60, 77);
        for (int x = 0; x < 60; ++x) run.grid.set_element(x, 50, ElementType::Wall);
        for (int y = 30; y < 50; ++y)
            for (int x = 5; x < 55; ++x) run.grid.set_element(x, y, ElementType::Water);

        const int before = count_of(run.grid, ElementType::Water);

        Input paint;
        paint.brush_active = true;
        paint.brush_type = ElementType::Sand;
        paint.brush_size = 4;
        paint.cursor_y = 40;
        // Dragged, not stamped. A single stamp passes against the broken code by
        // accident: one step deletes few enough cells to look like rounding. The
        // defect is in a stroke.
        for (int x = 10; x < 50; ++x) {
            paint.cursor_x = x;
            run.step(paint);
        }

        const int after = count_of(run.grid, ElementType::Water);
        check("dragging a brush through water does not destroy it",
              after == before,
              std::to_string(before) + " cells before, " + std::to_string(after) + " after");
    }

    // The same claim for the brush that cannot be walked through on the way out, and
    // the reason Run::step paints its disc bottom row first. A Wall disc painted
    // top-down seals the rows beneath it before they have moved, and every one of
    // them is then deleted for want of anywhere to go -- invisible to the Sand case
    // above, because sand is movable and a displacement climbs straight through it.
    {
        Run run(60, 60, 78);
        for (int x = 0; x < 60; ++x) run.grid.set_element(x, 50, ElementType::Wall);
        for (int y = 30; y < 50; ++y)
            for (int x = 5; x < 55; ++x) run.grid.set_element(x, y, ElementType::Water);

        const int before = count_of(run.grid, ElementType::Water);

        Input paint;
        paint.brush_active = true;
        paint.brush_type = ElementType::Wall;
        paint.brush_size = 4;
        paint.cursor_x = 30;
        paint.cursor_y = 40;
        run.step(paint);

        const int after = count_of(run.grid, ElementType::Water);
        check("a Wall disc dropped into water lifts the water rather than sealing it in",
              after == before,
              std::to_string(before) + " cells before, " + std::to_string(after) + " after");
    }

    // The limit of the promise, asserted so it is a decision and not a surprise.
    // Water with a lid on it has nowhere to go, make_room_above says so, and the
    // brush overwrites rather than refusing the stroke. This test exists to fail
    // loudly if someone later makes displacement unconditional: a brush that will
    // not paint where the player clicked is a worse defect than a lost cell of
    // water, and this is the line between the two.
    {
        Run run(40, 40, 79);
        for (int x = 10; x < 30; ++x) {
            run.grid.set_element(x, 20, ElementType::Wall);  // lid
            run.grid.set_element(x, 26, ElementType::Wall);  // floor
        }
        for (int x = 10; x < 30; ++x) {
            run.grid.set_element(x, 21, ElementType::Wall);
            run.grid.set_element(x, 25, ElementType::Wall);
        }
        for (int y = 22; y < 25; ++y)
            for (int x = 11; x < 29; ++x) run.grid.set_element(x, y, ElementType::Water);

        Input paint;
        paint.brush_active = true;
        paint.brush_type = ElementType::Sand;
        paint.brush_size = 1;
        paint.cursor_x = 20;
        paint.cursor_y = 23;
        run.step(paint);

        check("a sealed pool has no room, and the brush still paints",
              run.grid.get_element(20, 23).type == ElementType::Sand);
    }

    // --- the same seed plus the same recorded Input sequence replays
    //     byte-identically, regardless of how the caller paces the calls ---
    // A generator or a per-rendered-frame sample could not make this promise: the
    // same physical session played at two frame rates would batch a held key into a
    // different number of steps per sample, and diverge. Here there is no sampling
    // left to diverge -- the sequence below is the input, independent of any frame
    // it was ever rendered on.
    {
        std::vector<Input> sequence;
        for (int i = 0; i < 40; ++i) sequence.push_back(NOTHING);  // let the player land
        for (int i = 0; i < 50; ++i) sequence.push_back(held_right());
        {
            Input jump; jump.jump = true;
            sequence.push_back(jump);
        }
        for (int i = 0; i < 30; ++i) sequence.push_back(NOTHING);
        for (int i = 0; i < 20; ++i) {
            Input paint;
            paint.brush_active = true;
            paint.brush_type = ElementType::Water;
            paint.brush_size = 3;
            paint.cursor_x = 60;
            paint.cursor_y = 10;
            sequence.push_back(paint);
        }
        for (int i = 0; i < 25; ++i) {
            Input dig;
            dig.dig = true;
            dig.cursor_x = 60;
            dig.cursor_y = 20;
            sequence.push_back(dig);
        }

        Run a(90, 50, 4040);
        build_floor(a, 45);
        for (const Input& in : sequence) a.step(in);

        // Same sequence, same seed, but consumed in batches with a render-style
        // read (get_pixels(), active_chunk_count()) between them -- exactly what
        // main.cpp does every rendered frame between groups of fixed steps. If
        // pacing or an intervening read could perturb the result, this is the run
        // that shows it.
        Run b(90, 50, 4040);
        build_floor(b, 45);
        size_t i = 0;
        while (i < sequence.size()) {
            const size_t batch = std::min<size_t>(7, sequence.size() - i);
            for (size_t j = 0; j < batch; ++j) b.step(sequence[i + j]);
            i += batch;
            (void)b.grid.get_pixels();
            (void)b.grid.active_chunk_count();
        }

        check("a replayed Input sequence reproduces the same world regardless of batching",
              worlds_match(a.grid, b.grid));
        check("...and the same player position",
              a.player.cell_x() == b.player.cell_x() && a.player.cell_y() == b.player.cell_y());

        // --- the same sequence, through a file ---
        //
        // The suite above proves a sequence held in memory replays identically. The
        // benchmark row depends on a stronger claim -- that a sequence written to disk
        // and read back is the same sequence -- and that is about a byte format rather
        // than about the simulation. Checked here rather than in its own suite
        // because the thing it has to agree with is run.step().
        {
            input_log::Log out;
            out.header.grid_w = 90;
            out.header.grid_h = 50;
            out.header.seed = 4040;
            out.header.scene = "bg_tarn";
            out.header.scene_cells = 0;
            out.header.start_fingerprint = 12345;
            out.header.end_fingerprint = input_log::fingerprint(a.grid);
            out.header.end_player_x = a.player.cell_x();
            out.header.end_player_y = a.player.cell_y();
            out.steps = sequence;

            const char* path = "test_run_log.rec";
            std::string error;
            check("a session log writes", input_log::write(path, out, &error), error);

            input_log::Log in;
            check("...and reads back", input_log::read(path, in, &error), error);
            check("...with every header field intact",
                  in.header.grid_w == out.header.grid_w && in.header.grid_h == out.header.grid_h &&
                  in.header.seed == out.header.seed &&
                  in.header.scene == out.header.scene &&
                  in.header.scene_cells == out.header.scene_cells &&
                  in.header.start_fingerprint == out.header.start_fingerprint &&
                  in.header.end_fingerprint == out.header.end_fingerprint &&
                  in.header.end_player_x == out.header.end_player_x &&
                  in.header.end_player_y == out.header.end_player_y);
            check("...and the same number of steps", in.steps.size() == sequence.size(),
                  std::to_string(in.steps.size()) + " vs " + std::to_string(sequence.size()));

            bool inputs_identical = in.steps.size() == sequence.size();
            for (size_t k = 0; k < in.steps.size() && inputs_identical; ++k) {
                const Input& x = in.steps[k];
                const Input& y = sequence[k];
                inputs_identical = x.left == y.left && x.right == y.right && x.jump == y.jump &&
                                   x.dig == y.dig && x.cursor_x == y.cursor_x &&
                                   x.cursor_y == y.cursor_y && x.brush_active == y.brush_active &&
                                   x.brush_type == y.brush_type && x.brush_size == y.brush_size;
            }
            check("...and every field of every Input", inputs_identical);

            // The assertion the row actually rests on: a log is only worth
            // benchmarking if replaying it lands in the same world. Asserted on the
            // fingerprint rather than by comparing grids, because the fingerprint is
            // what grid_bench checks and a test that checked something else would
            // leave the thing being trusted untested.
            Run c(90, 50, 4040);
            build_floor(c, 45);
            for (const Input& in_step : in.steps) c.step(in_step);
            check("a log read from disk replays into the recorded world",
                  input_log::fingerprint(c.grid) == in.header.end_fingerprint);
            check("...and leaves the player where the recording did",
                  c.player.cell_x() == in.header.end_player_x &&
                  c.player.cell_y() == in.header.end_player_y);

            // A fingerprint that cannot tell two worlds apart would make every check
            // above vacuous -- it would pass on a log replayed into the wrong world.
            c.grid.set_element(1, 1, ElementType::Wall);
            check("...and the fingerprint notices a single changed cell",
                  input_log::fingerprint(c.grid) != in.header.end_fingerprint);

            // Truncation is refused rather than read as a shorter session. A log cut
            // off by a crash mid-write is the realistic way this happens, and a short
            // replay would quietly measure a fraction of the session it names.
            {
                std::FILE* f = std::fopen(path, "rb");
                std::fseek(f, 0, SEEK_END);
                const long size = std::ftell(f);
                std::fseek(f, 0, SEEK_SET);
                std::vector<unsigned char> bytes(static_cast<size_t>(size));
                (void)std::fread(bytes.data(), 1, bytes.size(), f);
                std::fclose(f);

                const char* cut_path = "test_run_log_cut.rec";
                std::FILE* g = std::fopen(cut_path, "wb");
                std::fwrite(bytes.data(), 1, bytes.size() - 7, g);  // mid-record
                std::fclose(g);

                input_log::Log cut;
                check("a truncated log is refused, not read short",
                      !input_log::read(cut_path, cut, &error));
                std::remove(cut_path);
            }

            std::remove(path);
        }
    }

    // ========================================================================
    // The run can be won and it can be lost.
    //
    // Driven entirely through run.step(), like everything above -- a run that ends
    // because a test called a setter is not evidence that a played one ever would.
    // ========================================================================

    // --- a fresh run is Playing, and stays Playing with nowhere to go ---
    // The negative case first, because it is what makes the two below mean anything:
    // an outcome that latched to Won on construction would pass a "reaching the
    // objective wins" test perfectly.
    {
        Run run(200, 120, 5150);
        build_floor(run, 100);
        check("S0: a fresh run is Playing", run.outcome() == Run::Outcome::Playing);
        check("S0: ...and has no objective until one is placed", !run.has_objective());

        step(run, NOTHING, 200);
        check("S0: a run with no objective and no hazard stays Playing",
              run.outcome() == Run::Outcome::Playing && run.player.is_alive(),
              "hp=" + std::to_string(run.player.health()));
    }

    // --- an objective out of reach does not end the run; walking to it does ---
    // One scenario rather than two, for the same reason the fall-damage tests in
    // test_player.cpp are one: the claim is about the boundary, and a rule that
    // fired everywhere would pass the second half alone.
    {
        Run run(400, 120, 5151);
        build_floor(run, 100);
        step(run, NOTHING, 120);  // land first
        check("the player lands before the objective is placed", run.player.is_on_ground());

        const int start_x = run.player.cell_x();
        // Well beyond OBJECTIVE_REACH, and beyond the body's own width so the box
        // cannot be overlapping it at placement time.
        const int goal = start_x + Player::WIDTH + 4 * Run::OBJECTIVE_REACH + 40;
        run.set_objective(goal, run.player.center_y());
        check("S0: an objective can be placed", run.has_objective() &&
              run.objective_x() == goal);

        run.step(NOTHING);
        check("S0: an objective out of reach does not end the run",
              run.outcome() == Run::Outcome::Playing,
              "player x=" + std::to_string(run.player.cell_x()) +
                  " goal x=" + std::to_string(goal));

        // Walk to it. Bounded rather than "until it wins", so a rule that never
        // fires fails the check instead of hanging the suite.
        for (int i = 0; i < 300 && run.outcome() == Run::Outcome::Playing; ++i)
            run.step(held_right());

        check("S0: walking onto the objective wins the run",
              run.outcome() == Run::Outcome::Won,
              "player x=" + std::to_string(run.player.cell_x()) +
                  " goal x=" + std::to_string(goal));
    }

    // --- dying loses the run ---
    // Killed by fire painted through the brush, which is a path a player has: the
    // point is that the loss arrives through the simulation rather than through a
    // test reaching into the body.
    {
        Run run(200, 120, 5152);
        build_floor(run, 100);
        step(run, NOTHING, 120);

        Input burn;
        burn.brush_active = true;
        burn.brush_type = ElementType::Fire;
        burn.brush_size = Player::HEIGHT / 2;

        for (int i = 0; i < 4000 && run.outcome() == Run::Outcome::Playing; ++i) {
            burn.cursor_x = run.player.center_x();
            burn.cursor_y = run.player.center_y();
            run.step(burn);
        }

        check("S0: a body burnt to zero health loses the run",
              run.outcome() == Run::Outcome::Lost && !run.player.is_alive(),
              "hp=" + std::to_string(run.player.health()));

        // The outcome latches. main.cpp freezes the world by not accumulating time,
        // so nothing in the SDL shell would notice this -- but a headless caller
        // drives straight past the ending, and a run that reported Playing again
        // once the flames went out would be worse than useless.
        step(run, NOTHING, 300);
        check("S0: ...and the outcome does not un-latch when the hazard clears",
              run.outcome() == Run::Outcome::Lost);
    }

    // --- reset restarts the run and keeps the objective ---
    // The objective surviving reset() is the second documented exception to "reset
    // gives you a fresh Run" and it is asserted rather than written down, for the
    // same reason test_grid.cpp asserts vent_radius survives: a hand-written wipe
    // can forget a field, and this one silently produces an unwinnable run when it
    // does.
    {
        Run run(200, 120, 5153);
        build_floor(run, 100);
        step(run, NOTHING, 120);
        run.set_objective(run.player.center_x(), run.player.center_y());
        run.step(NOTHING);
        check("S0: the run is Won before the reset", run.outcome() == Run::Outcome::Won);

        run.reset(5153);
        check("S0: reset puts the run back to Playing",
              run.outcome() == Run::Outcome::Playing);
        check("S0: ...with the body back at full health",
              run.player.health() == Player::MAX_HEALTH);
        check("S0: ...and the objective still placed",
              run.has_objective(), "an objective cleared by reset leaves an "
                                   "unwinnable run that says nothing");

        // And the one thing that may clear it, which is a level change. The
        // exception above is right while the caller re-stamps the same scene;
        // switching to one with no terrain would otherwise leave this objective
        // hanging in empty space, claiming a run is winnable when nothing can be
        // reached.
        run.clear_objective();
        check("S0: clear_objective removes it, for a caller changing the level",
              !run.has_objective());
        run.reset(5153);
        check("S0: ...and it stays cleared through a reset", !run.has_objective());
    }

    // --- the recorder's guarantee survives a restart ---
    //
    // A session log replays by rebuilding the world from the seed and the scene and
    // then feeding the recorded inputs back in, so a reset in the middle of a
    // recording would replay into the wrong world and the bench could not tell.
    // main.cpp starts a new recording on restart instead, and the reason that is
    // sound is asserted here: reset with the same seed, re-stamped with the same
    // terrain, is the same world the first recording started in.
    {
        Run a(120, 80, 5154);
        build_floor(a, 60);
        const uint64_t before = input_log::fingerprint(a.grid);

        step(a, held_right(), 200);
        Input paint;
        paint.brush_active = true;
        paint.brush_type = ElementType::Sand;
        paint.brush_size = 3;
        paint.cursor_x = 40;
        paint.cursor_y = 20;
        step(a, paint, 40);
        check("the world actually moved before the reset",
              input_log::fingerprint(a.grid) != before);

        a.reset(5154);
        build_floor(a, 60);  // what main.cpp's re-stamp of the scene stands in for
        check("S0: reset plus a re-stamp reproduces the world a recording starts from",
              input_log::fingerprint(a.grid) == before,
              "a mismatch here means a restarted session's log replays into a "
              "different world than it was recorded in");
    }

    // --- a reset may change the world's size ----------------------------
    //
    // The check that matters is the absence of a change, not the presence of one.
    // reset(seed) is what every existing caller and every recorded session goes
    // through, and the size arguments default to 0 meaning keep it; a defaulted
    // resize would silently reallocate the grid on every restart and drop
    // Grid::reset's documented vent_radius exception with it.
    {
        Run r(120, 80, 909);
        r.reset(909);
        check("a reset with no size stated keeps the size",
              r.grid.get_width() == 120 && r.grid.get_height() == 80);

        r.reset(909, 120, 80);
        check("and so does a reset to the size it already has",
              r.grid.get_width() == 120 && r.grid.get_height() == 80);

        r.reset(909, 300, 150);
        check("a stated size resizes the grid",
              r.grid.get_width() == 300 && r.grid.get_height() == 150,
              std::to_string(r.grid.get_width()) + "x" +
                  std::to_string(r.grid.get_height()));
        check("and the resized world is a fresh one, not a stretched one",
              input_log::fingerprint(r.grid) ==
                  input_log::fingerprint(Grid(300, 150, 909)));
        // The body is re-placed against the new world rather than left at a
        // coordinate the old one happened to have. Left alone, a scene switch to a
        // smaller world spawns the player outside it.
        check("the body is re-placed inside the new world",
              r.player.center_x() >= 0 && r.player.center_x() < 300 &&
                  r.player.center_y() >= 0 && r.player.center_y() < 150);

        // Half a size is not a size, at this boundary as well as at the parser's.
        r.reset(909, 0, 400);
        check("a height with no width changes nothing",
              r.grid.get_width() == 300 && r.grid.get_height() == 150);
    }

    return report();
}
