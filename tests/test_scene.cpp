#include "scene/scene.h"
#include "scene/bmp.h"
#include "scene/legend.h"
#include "physics/grid.h"
#include "test_util.h"
#include <cstdio>
#include <string>

namespace {

// What assets/test_material.bmp currently stamps into an empty world.
//
// Pinned deliberately, and a failure here is not necessarily a defect. The
// fixture scene is the world the recorded session was played in, so this number
// is that session log's identity: if the fixture changes, the log is stale and
// the replayed benchmark row has to be re-recorded. This is the cheapest place
// to catch that, because it fails in ctest rather than in a benchmark nobody
// runs on the commit that broke it.
//
// So: if you changed the scene on purpose, update this number and re-record the
// session (README, "Recording a session"). If you did not, something has changed
// the legend or the loader underneath you.
//
// Cross-checked against tools/pixel_art.py's reader, which is an independent
// implementation of the same format. That agreement is what stands behind moving
// the loader out of main.cpp -- the number is not "what the new code happens to
// produce".
//
// When this moves, attribute the change rather than assuming it: revert each
// edit as a group and re-count. A revert has to carry a region's dependents or
// it measures two changes and reports one -- reverting a pit alone also stretches
// the bridge beam that spans it.
constexpr int FIXTURE_SCENE_CELLS = 334501;

} // namespace

int main() {
    // --- the material-map legend ---
    //
    // The legend used to be the render palette, so retuning the colours made every
    // pixel of the shipped scene match nothing -- and an unmatched pixel and an
    // empty one were the same answer. The game booted to a blank world and every
    // test still passed, because the lookup lived in main.cpp where nothing links
    // it.
    {
        ElementType t = ElementType::Count;

        check("legend resolves an authored material", element_from_legend(0x888888, t));
        check("...to the right type", t == ElementType::Wall);

        // Alpha is not part of a legend colour: BMPs arrive opaque, and a loader
        // that compared 32 bits would miss on every one of them.
        t = ElementType::Count;
        check("legend ignores the alpha byte", element_from_legend(0xFF4444FF, t) && t == ElementType::Water);

        // The whole point. An unrecognised colour must be distinguishable from a
        // deliberate Empty, or a broken scene file is indistinguishable from an
        // empty one.
        t = ElementType::Count;
        check("an unknown colour does not resolve", !element_from_legend(0x123456, t));
        t = ElementType::Count;
        check("Empty is a legend colour in its own right",
              element_from_legend(0x000000, t) && t == ElementType::Empty);

        // Every material has to be authorable, and no two may collide. Both are
        // static_asserts in legend.h; asserted again here so a failure names itself
        // rather than only stopping the build.
        bool all_reachable = true;
        for (int i = 0; i < static_cast<int>(ElementType::Count); ++i) {
            const ElementType want = static_cast<ElementType>(i);
            ElementType got = ElementType::Count;
            bool found = false;
            for (int k = 0; k < LEGEND_SIZE; ++k)
                if (SCENE_LEGEND[k].type == want &&
                    element_from_legend(SCENE_LEGEND[k].rgb, got) && got == want) found = true;
            if (!found) all_reachable = false;
        }
        check("every material has a legend colour that resolves back to it", all_reachable);

        // The legend must not drift back into being the render palette. If these
        // ever coincide again it is because someone tidied up by pointing one at the
        // other, which is the original bug returning.
        check("the legend is not the render palette",
              (material_of(ElementType::Water).color & 0xFFFFFF) != 0x4444FF);
    }

    {
        Grid g(10, 10, 123);
        Scene s;
        s.width = 2;
        s.height = 2;
        s.materials = {
            ElementType::Wall, ElementType::Sand,
            ElementType::Wood, ElementType::Water
        };
        s.albedo = {
            0xFF111111, 0xFF222222,
            0xFF333333, 0xFF444444
        };

        const int placed = load_scene(g, s, 2, 2);
        check("load_scene reports how many cells it placed", placed == 4,
              "placed=" + std::to_string(placed));

        check("scene Wall loaded correctly", g.get_element(2, 2).type == ElementType::Wall);
        check("scene Wall color loaded correctly", g.get_element(2, 2).color == 0xFF111111);
        
        check("scene Sand loaded correctly", g.get_element(3, 2).type == ElementType::Sand);
        check("scene Sand color loaded correctly", g.get_element(3, 2).color == 0xFF222222);

        check("scene Wood loaded correctly", g.get_element(2, 3).type == ElementType::Wood);
        check("scene Wood color loaded correctly", g.get_element(2, 3).color == 0xFF333333);

        check("scene Water loaded correctly", g.get_element(3, 3).type == ElementType::Water);
        check("scene Water color loaded correctly", g.get_element(3, 3).color == 0xFF444444);

        check("outside scene remains empty", g.get_element(1, 1).type == ElementType::Empty);
    }

    {
        // Empty is a background colour someone painted in an editor, not a material
        // with a legend entry -- loading it must not stamp its albedo pixel over
        // whatever the grid already had there.
        Grid g(10, 10, 123);
        g.set_element(5, 5, ElementType::Wall);
        const uint32_t before = g.get_element(5, 5).color;

        Scene s;
        s.width = 1;
        s.height = 1;
        s.materials = { ElementType::Empty };
        s.albedo = { 0xFF999999 };

        const int placed = load_scene(g, s, 5, 5);

        check("scene Empty leaves existing type untouched", g.get_element(5, 5).type == ElementType::Wall);
        check("scene Empty leaves existing color untouched", g.get_element(5, 5).color == before);
        check("an all-Empty scene reports placing nothing", placed == 0,
              "placed=" + std::to_string(placed));
    }

    // --- a scene that names no material is reported as such ---
    //
    // The signal that was missing. "Parsed without error" and "put something in the
    // world" were the same answer to every caller, so a scene whose every pixel
    // failed to resolve looked exactly like a scene that loaded fine.
    {
        Grid g(10, 10, 123);
        Scene s;
        s.width = 3;
        s.height = 3;
        s.materials.assign(9, ElementType::Empty);
        s.albedo.assign(9, 0xFF808080);
        check("a scene naming no material places nothing", load_scene(g, s) == 0);

        // A malformed scene is zero too, rather than a partial stamp.
        Scene bad;
        bad.width = 4;
        bad.height = 4;
        bad.materials.assign(9, ElementType::Wall);  // wrong size on purpose
        bad.albedo.assign(16, 0xFF808080);
        check("a malformed scene places nothing", load_scene(g, bad) == 0);
        check("...and leaves the grid alone", g.get_element(0, 0).type == ElementType::Empty);
    }

    // --- the shipped fixture, loaded the way the game loads it ---
    //
    // The first test that reads the actual scene files. Everything above builds a
    // Scene by hand, which is the right shape for testing the stamping rules and is
    // blind to the thing that has gone wrong here twice: the world the game boots
    // into being empty or different from what everyone believes.
    {
        std::string error, warning;
        const Scene s = bmp::load("assets/test_material.bmp", "assets/test_albedo.bmp",
                                  &error, &warning);
        check("the shipped fixture scene loads", error.empty(), error);
        check("...at the size main.cpp simulates", s.width == 1920 && s.height == 1080,
              std::to_string(s.width) + "x" + std::to_string(s.height));
        // A warning here means an authored pixel matched no legend entry, which is
        // the failure that emptied the whole world once and said nothing.
        check("...with every authored pixel in the legend", warning.empty(), warning);

        Grid g(1920, 1080, 1);
        const int placed = load_scene(g, s, 0, 0);
        check("...and stamps the cell count the session log was recorded against",
              placed == FIXTURE_SCENE_CELLS,
              std::to_string(placed) + " now, " + std::to_string(FIXTURE_SCENE_CELLS) +
                  " pinned - see the note on FIXTURE_SCENE_CELLS");
    }

    return report();
}
