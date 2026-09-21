// The debug tooling.
//
// The point of this suite is that these decisions are not in the SDL event
// switch. Pause, single-step, the free camera's clamp and the inspector's text
// all have a wrong answer -- a pause that banks time, a step key that runs eight
// steps, a pan that stops responding at a world edge -- and every one of them
// would be unreachable by any test had it been written where the key is bound.
//
// The chunk-awake query is here rather than in grid_test because it exists for
// the inspector and its whole contract is the sentence the inspector prints.

#include "game/debug_view.h"
#include "physics/grid.h"
#include "test_util.h"
#include <string>

namespace {

void fill(Grid& g, int x0, int y0, int x1, int y1, ElementType t) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            g.set_element(x, y, t);
}

// Steps until nothing is left awake, or gives up. Returns whether it settled --
// a test that means "asleep" must not pass because it ran out of patience.
bool settle(Grid& g, int max_steps = 400) {
    for (int i = 0; i < max_steps; ++i) {
        if (g.active_chunk_count() == 0) return true;
        g.update();
    }
    return g.active_chunk_count() == 0;
}

} // namespace

int main() {
    // --- pause and single-step ---------------------------------------------
    {
        DebugView v;
        check("a new view is running, not paused", !v.paused);
        check("a running view queues no steps", !v.consume_single_step());

        // A step request while running is ignored. If it queued, the step would
        // run on top of the steps the frame already runs, which is a stutter
        // rather than a single-step and leaves no way to know how far the world
        // moved.
        v.request_single_step();
        check("a step asked for while running is dropped", !v.consume_single_step());

        v.toggle_pause();
        check("P pauses", v.paused);
        check("pausing alone queues nothing", !v.consume_single_step());

        v.request_single_step();
        check("a step asked for while paused is queued", v.consume_single_step());
        check("and is consumed exactly once", !v.consume_single_step());
    }

    // A burst of key-repeat inside one long frame must not turn into a run.
    {
        DebugView v;
        v.toggle_pause();
        for (int i = 0; i < DebugView::MAX_QUEUED_STEPS * 4; ++i) v.request_single_step();

        int ran = 0;
        while (v.consume_single_step()) ran++;
        check("queued single-steps are capped", ran == DebugView::MAX_QUEUED_STEPS,
              std::to_string(ran) + " queued from " +
                  std::to_string(DebugView::MAX_QUEUED_STEPS * 4) + " presses");
    }

    // Resuming must not spend steps asked for during the pause. This is the same
    // lurch the accumulator rule avoids, arriving by the other door.
    {
        DebugView v;
        v.toggle_pause();
        v.request_single_step();
        v.request_single_step();
        v.toggle_pause();
        check("resuming discards queued steps", !v.consume_single_step());
        check("and is running again", !v.paused);
    }

    // --- the free camera ---------------------------------------------------
    {
        DebugView v;
        check("a new view follows the player", !v.free_camera);

        v.detach_camera(900.0f, 500.0f, 481, 271, 1920, 1080);
        check("detaching starts where the view already was",
              v.free_camera && v.cam_x == 900.0f && v.cam_y == 500.0f);

        v.pan(10.0f, -20.0f, 481, 271, 1920, 1080);
        check("panning moves the centre", v.cam_x == 910.0f && v.cam_y == 480.0f,
              std::to_string(v.cam_x) + " " + std::to_string(v.cam_y));

        v.attach_camera();
        check("F returns to the player", !v.free_camera);
    }

    // The clamp is at the centre, and its bound is the view's range rather than the
    // world's. Panning hard into a corner and back has to move the view on the very
    // next frame: a centre allowed to wander into the half a viewport that
    // Camera::follow clamps away would leave the key dead on the way out -- the
    // stuck-key failure this clamp prevents, sat exactly where a free camera is
    // used, at the world's corners.
    {
        DebugView v;

        // Detaching next to a world edge, where the body spends much of its time
        // and where follow() is already clamping.
        v.detach_camera(20.0f, 20.0f, 481, 271, 1920, 1080);
        check("detaching near an edge lands where panning answers",
              v.cam_x == 240.5f && v.cam_y == 135.5f,
              std::to_string(v.cam_x) + " " + std::to_string(v.cam_y));

        v.pan(-5000.0f, -5000.0f, 481, 271, 1920, 1080);
        check("panning stops where the view does, top-left",
              v.cam_x == 240.5f && v.cam_y == 135.5f,
              std::to_string(v.cam_x) + " " + std::to_string(v.cam_y));

        v.pan(1.0f, 1.0f, 481, 271, 1920, 1080);
        check("and the very next press moves it",
              v.cam_x == 241.5f && v.cam_y == 136.5f,
              std::to_string(v.cam_x) + " " + std::to_string(v.cam_y));

        v.pan(5000.0f, 5000.0f, 481, 271, 1920, 1080);
        check("panning stops where the view does, bottom-right",
              v.cam_x == 1679.5f && v.cam_y == 944.5f,
              std::to_string(v.cam_x) + " " + std::to_string(v.cam_y));
    }

    // An axis the view cannot scroll on at all. The camera is pinned there, so
    // panning must be a no-op rather than something that accumulates silently.
    {
        DebugView v;
        v.detach_camera(0.0f, 0.0f, 481, 271, 200, 1080);
        v.pan(500.0f, 0.0f, 481, 271, 200, 1080);
        check("an axis smaller than the viewport pins the centre", v.cam_x == 100.0f,
              std::to_string(v.cam_x));
        v.pan(-500.0f, 0.0f, 481, 271, 200, 1080);
        check("and pins it from the other side too", v.cam_x == 100.0f,
              std::to_string(v.cam_x));
    }

    // --- the chunk-awake query ---------------------------------------------
    {
        Grid g(128, 128);  // 2x2 chunks

        check("nothing is awake in a fresh grid", !g.chunk_awake_at(10, 10));

        g.set_element(10, 10, ElementType::Wall);
        check("a write wakes the chunk it landed in", g.chunk_awake_at(10, 10));
        check("and not a chunk on the other side of the world",
              !g.chunk_awake_at(100, 100));

        // The 3x3 wake crossing a chunk border is what chunked sleep rests on, and
        // this is the query saying the same thing the invariant does.
        g.set_element(63, 100, ElementType::Wall);
        check("a write on a chunk border wakes the chunk next to it",
              g.chunk_awake_at(64, 100));

        check("settling puts the chunk back to sleep", settle(g) && !g.chunk_awake_at(10, 10));

        check("outside the world is never awake",
              !g.chunk_awake_at(-1, 10) && !g.chunk_awake_at(10, -1) &&
                  !g.chunk_awake_at(128, 10) && !g.chunk_awake_at(10, 128));
    }

    // --- the cell inspector -------------------------------------------------
    {
        Grid g(64, 64);
        fill(g, 0, 60, 63, 63, ElementType::Wall);
        settle(g);

        check("a settled cell reads asleep",
              describe_cell(g, 10, 61) == "X:10 Y:61 Wall T:20 CHUNK:ASLEEP",
              describe_cell(g, 10, 61));

        check("empty space reads as Empty rather than as nothing",
              describe_cell(g, 10, 10) == "X:10 Y:10 Empty T:20 CHUNK:ASLEEP",
              describe_cell(g, 10, 10));

        // Out of the world is reachable by ordinary mouse movement, because the
        // free camera can sit at a world edge with half the window past it.
        check("outside the world says so",
              describe_cell(g, -1, 10) == "X:-1 Y:10 OUTSIDE THE WORLD",
              describe_cell(g, -1, 10));
        check("and so does past the far edge",
              describe_cell(g, 10, 64) == "X:10 Y:64 OUTSIDE THE WORLD",
              describe_cell(g, 10, 64));
    }

    // A gas carries a countdown in `ticks` and a placed Fire cell carries its spawn
    // temperature, so both thermal fields are on this one line.
    {
        Grid g(64, 64);
        g.set_element(10, 10, ElementType::Fire);
        check("a lifetime cell shows its countdown and its heat",
              describe_cell(g, 10, 10) == "X:10 Y:10 Fire T:250 CHUNK:AWAKE LIFE:0",
              describe_cell(g, 10, 10));
    }

    // The other role the byte has. A structural cell in free fall carries the fall
    // clock its speed and its fracture threshold are read from, and it has never
    // been readable from the window while the collapse was happening.
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);  // floor
        fill(g, 20, 30, 30, 34, ElementType::Wall);  // slab, propped
        fill(g, 25, 35, 25, 49, ElementType::Wall);  // the prop
        g.update();
        fill(g, 25, 35, 25, 49, ElementType::Empty);  // cut it free
        g.update();
        g.update();

        // Two steps down from where it was authored.
        const std::string s = describe_cell(g, 20, 32);
        check("a falling structural cell shows its fall clock",
              s.find("Wall") != std::string::npos && s.find(" FALL:") != std::string::npos, s);

        // The finding this suite was written by, and it is about the engine rather
        // than about the inspector. A falling structural piece is carried by
        // pending_support, and resolve_support() runs before update() swaps the
        // chunk rects -- so the marks it leaves land in the set that is about to
        // become this step's work, and the sweep after it adds none of its own,
        // because a Static cell does nothing in the sweep. The piece therefore
        // falls with no chunk awake anywhere in the world.
        //
        // Nothing is broken: the piece keeps falling and update() is exactly as
        // correct as it ever was. What is false is "zero awake chunks means the
        // world has come completely to rest" -- that is the claim this pins, and
        // has_pending_support_checks() is the other half of the question.
        check("a slab in mid-fall leaves no chunk awake at all",
              g.active_chunk_count() == 0,
              std::to_string(g.active_chunk_count()) + " chunks awake");
        check("and the support queue is what says the world is not at rest",
              g.has_pending_support_checks());
        check("so the inspector says CHUNK, and does not claim the cell is asleep",
              s.find("CHUNK:ASLEEP") != std::string::npos, s);
    }

    // --- the status line ----------------------------------------------------
    //
    // A paused world that says nothing is indistinguishable from a hung one.
    {
        DebugView v;
        check("nothing is said while nothing is on", debug_status(v).empty());

        v.toggle_pause();
        check("a paused view says so", debug_status(v).find("PAUSED") != std::string::npos,
              debug_status(v));

        v.detach_camera(0.0f, 0.0f, 481, 271, 1920, 1080);
        const std::string both = debug_status(v);
        check("both modes at once say both",
              both.find("PAUSED") != std::string::npos &&
                  both.find("FREE CAMERA") != std::string::npos,
              both);
    }

    return report();
}
