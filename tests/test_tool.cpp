// Digging tests.
//
// The tool writes to the grid, which the player never does, so these cover a
// deliberate mutation of the world from outside the simulation step. Most
// scenarios freeze the grid (no update() call) so that what is asserted is the
// dig itself rather than the settling that follows it -- except the one test
// where the settling is the point.

#include "physics/tool.h"
#include "test_util.h"
#include <string>

namespace {

// Fills a rectangle, inclusive of both corners.
void fill(Grid& g, int x0, int y0, int x1, int y1, ElementType t) {
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            g.set_element(x, y, t);
}

int count_of(const Grid& g, ElementType t) {
    int n = 0;
    for (int y = 0; y < g.get_height(); ++y)
        for (int x = 0; x < g.get_width(); ++x)
            if (g.get_element(x, y).type == t) n++;
    return n;
}

// Digs once, ignoring the swing, by using a fresh tool each time.
//
// One update() call is still one whole dig: the blow lands on the step the
// button goes down, and a fresh tool is never mid-swing. The follow-through that
// would follow is of no interest to a scenario asserting on the hole.
bool dig_once(Grid& g, int from_x, int from_y, int aim_x, int aim_y) {
    DigTool tool;
    return tool.update(g, true, from_x, from_y, aim_x, aim_y);
}

} // namespace

int main() {
    // --- a dig removes cells around the impact point ---
    {
        Grid g(120, 80);
        fill(g, 60, 0, 119, 79, ElementType::Wall);
        const int before = count_of(g, ElementType::Wall);

        // Comfortably inside RANGE.
        const bool hit = dig_once(g, 45, 40, 110, 40);
        const int after = count_of(g, ElementType::Wall);

        check("digging into a slab reports a hit", hit);
        check("digging into a slab removes cells", after < before,
              "removed " + std::to_string(before - after));
        check("the hole is at the near face of the slab",
              g.get_element(60, 40).type == ElementType::Empty);
    }

    // --- range is a hard limit ---
    {
        Grid g(200, 80);
        // Slab starts well beyond RANGE from the origin.
        fill(g, 100, 0, 199, 79, ElementType::Wall);
        const int before = count_of(g, ElementType::Wall);

        const bool hit = dig_once(g, 10, 40, 150, 40);

        check("a target beyond range is not hit", !hit);
        check("a target beyond range is untouched",
              count_of(g, ElementType::Wall) == before);
    }

    // --- the ray stops at the FIRST solid it meets ---
    // The anti-tunnelling assertion. A ray that samples every Nth cell can pass
    // through a thin wall and dig the terrain behind it, which is exactly the wall
    // the player was sheltering behind.
    {
        Grid g(120, 80);
        fill(g, 30, 0, 30, 79, ElementType::Wall);  // near wall, 1 cell thick
        fill(g, 60, 0, 60, 79, ElementType::Wall);  // far wall, 1 cell thick

        dig_once(g, 10, 40, 110, 40);

        check("the near wall is cut", g.get_element(30, 40).type == ElementType::Empty);
        check("the far wall is untouched behind it",
              g.get_element(60, 40).type == ElementType::Wall);
    }

    // --- liquids and gases do not stop the ray ---
    {
        Grid g(120, 80);
        fill(g, 20, 30, 25, 50, ElementType::Water);
        fill(g, 28, 30, 30, 50, ElementType::Fire);
        fill(g, 34, 0, 34, 79, ElementType::Wall);

        dig_once(g, 10, 40, 110, 40);

        check("the ray passes through water and fire to the wall behind",
              g.get_element(34, 40).type == ElementType::Empty);
    }

    // --- digging under a settled pile makes it fall ---
    // The one test that steps the grid. It is really an assertion about
    // set_element() waking the 3x3 neighbourhood: a dig that wrote cells[] directly
    // would leave the sand hanging over the hole.
    {
        Grid g(120, 80);
        fill(g, 0, 60, 119, 79, ElementType::Wall);  // floor
        fill(g, 50, 40, 70, 59, ElementType::Sand);  // pile resting on it

        const int placed = count_of(g, ElementType::Sand);
        for (int i = 0; i < 200; ++i) g.update();  // let it settle
        const int settled = count_of(g, ElementType::Sand);

        // Guards the setup itself. If settling loses grains, every assertion below
        // it is measuring the wrong thing.
        check("settling the pile conserves sand", placed == settled,
              std::to_string(placed) + " -> " + std::to_string(settled));
        check("the pile is resting on the floor",
              g.get_element(60, 59).type == ElementType::Sand);

        // Dig horizontally into the base of the pile, from inside RANGE. Stated
        // against the pile's near face so it stays a near-range dig if RANGE moves.
        const int dug_from_x = 35;
        dig_once(g, dug_from_x, 55, 119, 55);

        const int after_dig = count_of(g, ElementType::Sand);
        check("digging opens a hole in the pile", after_dig < settled,
              "removed " + std::to_string(settled - after_dig));

        for (int i = 0; i < 200; ++i) g.update();
        const int after_collapse = count_of(g, ElementType::Sand);

        check("the collapse that follows creates and destroys nothing",
              after_dig == after_collapse,
              std::to_string(after_dig) + " -> " + std::to_string(after_collapse));

        // The point of the whole block: the grains over the hole must not still be
        // hanging where they were. They can only know to fall because set_element()
        // woke their neighbourhood.
        check("sand above the hole did not stay hanging in mid-air",
              g.get_element(50, 59).type == ElementType::Sand);
    }

    // --- the swing gates repeat digs ---
    // Asserted against the tool's own state rather than by counting digs over time:
    // a slab recedes as it is dug, so a count would be bounded by geometry running
    // out of range as much as by the swing, and would pass for the wrong reason.
    //
    // The blow lands on the step the button goes down -- a dig that arrives half a
    // second late reads as input lag on a tool used this constantly -- and the swing
    // is the follow-through behind it. So what the swing gates is the interval to
    // the second hole. SWING_STEPS is referenced rather than spelled out; the whole
    // defect it replaced was two files holding their own copy of one number.
    {
        Grid g(120, 80);
        fill(g, 40, 0, 119, 79, ElementType::Wall);

        DigTool tool;
        check("a fresh tool is ready", tool.is_ready());
        check("the first held step digs", tool.update(g, true, 30, 40, 110, 40));
        check("a tool mid-swing is not ready", !tool.is_ready());

        // Counted from the blow that just landed, with the button still held, so
        // the tool is never idle and is_ready is not the thing to watch. The
        // interval between blows is the whole of the rate.
        int steps_to_next = 0;
        for (int i = 0; i < 4 * DigTool::SWING_STEPS; ++i) {
            ++steps_to_next;
            if (tool.update(g, true, 30, 40, 110, 40)) break;
        }
        check("a held button lands the next blow one swing later",
              steps_to_next == DigTool::SWING_STEPS,
              std::to_string(steps_to_next) + " steps, swing is " +
                  std::to_string(DigTool::SWING_STEPS));
    }

    // --- releasing the button does not strand a swing ---
    // The swing advances only when update() is called, so a swing left half finished
    // would be resumed by the next press from wherever it stopped -- the player
    // pressing dig and seeing the second half of a swing.
    {
        Grid g(120, 80);
        fill(g, 40, 0, 119, 79, ElementType::Wall);

        DigTool tool;
        tool.update(g, true, 30, 40, 110, 40);  // start a swing
        for (int i = 0; i < DigTool::SWING_STEPS * 2; ++i)
            tool.update(g, false, 30, 40, 110, 40);  // button released
        check("a swing finishes even after the button comes up", tool.is_ready());
    }

    // --- holding the button does not dig every step ---
    {
        Grid g(400, 80);
        fill(g, 0, 0, 399, 79, ElementType::Wall);

        DigTool tool;
        int digs = 0;
        const int steps = 60;
        for (int i = 0; i < steps; ++i)
            if (tool.update(g, true, 200, 40, 399, 40)) digs++;

        check("holding dig is rate-limited", digs > 0 && digs < steps,
              std::to_string(digs) + " digs in " + std::to_string(steps) + " steps");
    }

    // --- missing costs no cooldown ---
    {
        Grid g(120, 80);
        DigTool tool;
        for (int i = 0; i < 30; ++i) tool.update(g, true, 20, 40, 30, 40);  // empty air
        check("digging thin air leaves the tool ready", tool.is_ready());
    }

    // --- the world border survives being dug at ---
    {
        Grid g(60, 60);
        fill(g, 0, 50, 59, 59, ElementType::Wall);
        const int before = count_of(g, ElementType::Wall);

        // Straight at the border from close range, and then past the corner.
        dig_once(g, 5, 55, -40, 55);
        dig_once(g, 5, 5, -40, -40);
        dig_once(g, 55, 55, 200, 200);

        check("digging at the border does not crash and leaves the world sealed",
              g.get_element(-1, 55).type == ElementType::Wall &&
              g.get_element(60, 55).type == ElementType::Wall);
        check("digging at the border still removes in-bounds cells",
              count_of(g, ElementType::Wall) < before);
    }

    // --- range is a real distance, measured exactly ---
    // A 3-4-5 triangle puts a cell at exactly RANGE, which is the one case a squared
    // comparison and a sqrt can disagree about: a library sqrt coming back a bit
    // under the exact value silently shortens every dig. The second half is the same
    // ray one cell longer, which must fall outside -- a diagonal that reached as far
    // as a straight one would pass the first check and fail this one.
    {
        Grid g(200, 200);
        g.set_element(56, 68, ElementType::Wall);  // exactly RANGE from the origin
        check("a solid at exactly RANGE is reached",
              dig_once(g, 20, 20, 56, 68));
    }
    {
        Grid g(200, 200);
        g.set_element(57, 69, ElementType::Wall);  // one cell further out
        check("a solid just past RANGE on the same diagonal is not reached",
              !dig_once(g, 20, 20, 57, 69));
    }

    // --- the path is symmetric about the origin ---
    // Rounding halves away from zero makes the ray up-left the mirror of the ray
    // down-right. The obvious integer replacement, (a + b/2) / b, is not: C++
    // integer division truncates toward zero, so the negative side rounds the other
    // way and digs land one cell off in two of the four quadrants. Asserted through
    // aim_point on an empty grid, where the reported cell is the end of the range
    // rather than an impact.
    {
        Grid g(400, 400);
        DigTool tool;
        int px = 0, py = 0, nx = 0, ny = 0;
        tool.aim_point(g, 200, 200, 237, 251, px, py);
        tool.aim_point(g, 200, 200, 163, 149, nx, ny);

        check("the aim ray is the same length in both directions",
              (px - 200) == -(nx - 200) && (py - 200) == -(ny - 200),
              "+(" + std::to_string(px - 200) + "," + std::to_string(py - 200) +
                  ") vs -(" + std::to_string(200 - nx) + "," +
                  std::to_string(200 - ny) + ")");
    }

    // --- a distant aim does not overflow the range arithmetic ---
    // The squared form multiplies span*span by RANGE*RANGE, which leaves 32-bit
    // range at a cursor a couple of thousand cells out -- an ordinary aim in a big
    // world. A wrapped intermediate gives a negative step count (dig does nothing)
    // or a huge one (dig reaches across the level), and both look like a range bug
    // rather than an overflow.
    {
        Grid g(300, 120);
        fill(g, 155, 0, 155, 119, ElementType::Wall);
        check("a very distant aim still digs what is inside range",
              dig_once(g, 100, 60, 100100, 61));
    }
    {
        Grid g(300, 120);
        fill(g, 170, 0, 170, 119, ElementType::Wall);
        check("a very distant aim still stops at range",
              !dig_once(g, 100, 60, 100100, 61));
    }

    // --- aiming at your own position does nothing ---
    {
        Grid g(60, 60);
        fill(g, 0, 0, 59, 59, ElementType::Wall);
        const int before = count_of(g, ElementType::Wall);
        const bool hit = dig_once(g, 30, 30, 30, 30);
        check("a zero-length aim is a no-op",
              !hit && count_of(g, ElementType::Wall) == before);
    }

    return report();
}
