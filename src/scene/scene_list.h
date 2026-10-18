#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Which scenes exist, and what each one is made of.
//
// A list rather than literals in main.cpp, because an empty scene is a scene
// with no image, not an image of nothing: two grid-sized BMPs of pure black
// would be megabytes carrying no information. A scene names its files, and a
// scene that names none is empty by construction.
//
// Format, one record per line, `#` to end-of-line is a comment:
//
//     # name    material           albedo            props           spawn    named fields
//     empty     -                  -                 -               floor    mode=unclamped size=1920x1080
//     bg_valley bg_valley/bg_valley_material.bmp  bg_valley/bg_valley_albedo.bmp  -  terrain  size=344x144 scale=10 backdrop=bg_valley
//
// Five positional fields, all required, `-` meaning none for the three
// filenames. Blank lines are fine. Anything else is an error with a line number,
// never a silently skipped row.
//
// Everything after the five is a named field, `key=value`, in any order and each
// at most once. An omitted field is not an ignored one -- it is a row that made
// no claim, and the default it falls back to is stated here:
//
//     mode=<mode>      `fixed` (the default) is a bounded world with solid
//                      borders and non-repeating backdrops; `unclamped` lets the
//                      camera scroll past the world's left and right edges, over
//                      tiling backdrops. Unclamped is not infinite: the grid is
//                      still the stated size and its borders are still solid, so
//                      the body stops at them -- only the view goes further.
//     size=<W>x<H>     the world in cells. One field, so it is a pair or absent;
//                      absent means derive it from the material BMP's own
//                      dimensions, or the engine default when the scene names no
//                      BMP.
//     scale=<N>        screen pixels per cell, 1 to 64; Camera::DEFAULT_SCALE when
//                      absent.
//     level=<file>     the level file (scene/level_list.h), resolved under assets/
//     backdrop=<dir>   the backdrop drawn behind it: assets/<dir>/backdrop.txt
//                      (render/backdrop_set.h). Without one, the clear colour.
//
// Mode, size and scale were positional until they were named, and a row still
// written that way (`... floor unclamped 1920 1080`) is refused with the named
// spelling in the message rather than read.
//
// Names and filenames are text_record::name_ok (util/text_record.h): letters,
// digits, `_` and `-`, plus one dot before a filename's extension. A filename may
// sit in one folder, `<dir>/<file>`, and only in the scene's own backdrop= folder.
// Nothing else, so no second separator, no backslash and no `..`, and a scene
// list can never reach outside assets/ or into assets/wip/.
//
// Every field is read, which is why there is no `seed` column and no x/y spawn
// column: a number the loader ignores is one an author eventually spends an
// afternoon tuning. `spawn` is a word rather than a coordinate for the same
// reason the prop format has no y -- the ground is not one number, so the spawn
// row is scanned rather than authored, and the file only says which scan to run.
namespace scene_list {

// Where the body starts, once the scene is stamped.
enum class Spawn {
    // Stand on the terrain under the spawn column. A scene with no terrain
    // under that column leaves the body where it was and the caller warns.
    Terrain,
    // Put the body on the world's bottom border. This is not a fallback for
    // Terrain and must not be made into one: it exists for a scene that is
    // meant to be empty, and using it when a terrain scan merely failed would
    // turn a broken scene into a playable-looking one.
    Floor,
};

// Which of the two world paradigms a scene is. A property of the authored
// location rather than a camera setting, which is why it lives here: it decides
// the world's borders, how the backdrop is laid out, and whether travelling in
// one direction ever ends. A camera that could be switched between them
// independently of the scene would be able to pan off the edge of a bounded
// world's art.
enum class SceneMode : uint8_t {
    // A bounded world of a stated size with solid borders. The camera clamps to
    // the world rect and each backdrop layer is panned across its own width
    // exactly once, so authored art never repeats and never runs out.
    Fixed,
    // The camera does not clamp horizontally, so the view can scroll past the
    // world's side edges, and the backdrop layers tile through
    // backdrop_wrap::wrap_axis. The world itself is the same finite grid with
    // solid borders -- this is about the view, which is why it is not called
    // infinite. Vertical is still clamped, because the world has a floor and a
    // ceiling either way.
    Unclamped,
};

struct SceneDef {
    std::string name;  // the scene's own name; also what the HUD shows
    std::string material;  // asset stem or empty; resolves to assets/<stem>
    std::string albedo;     // asset stem or empty
    std::string props;      // asset stem or empty
    // `level=<file>`: what the level puts in its world -- player column,
    // objective, enemies (scene/level_list.h). Empty means every default.
    std::string level;
    // `backdrop=<dir>`: the backdrop set in assets/<dir>/backdrop.txt. Empty is
    // no backdrop. Named here rather than matched against the scene's own name
    // in code, so two scenes can share a set and a new set is an asset drop.
    std::string backdrop;
    Spawn spawn = Spawn::Terrain;
    SceneMode mode = SceneMode::Fixed;
    // 0 means "not stated" and not "zero cells", which is why these are not
    // pre-filled with the engine default: the loader cannot know a scene's BMP
    // dimensions and the caller can, so the caller resolves 0 and this file never
    // invents a size it did not read.
    int custom_width = 0;
    int custom_height = 0;

    // Screen pixels per world cell for this scene, Camera::DEFAULT_SCALE when the
    // row does not say.
    //
    // Unlike the size, this is pre-filled with the default. A size the loader
    // invented would be a fact about the world that no file states; a scale is a
    // rendering choice with a project-wide default that this file can name without
    // knowing anything about the scene. The literal rather than
    // Camera::DEFAULT_SCALE because src/scene/ does not include src/game/;
    // scene_list_test pins the two to each other so the duplication cannot drift.
    int scale = 4;
    int line = 0;  // 1-based source line, for diagnostics

    // A scene that names no material map places no cells. Stated as a question about
    // the declaration rather than about the result, because "declared empty" and
    // "stamped nothing" have to stay distinguishable -- a legend that matched no
    // colour also places no cells, and that is a defect.
    bool declared_empty() const { return material.empty() && albedo.empty(); }

    bool is_unclamped() const { return mode == SceneMode::Unclamped; }
};

// Parses a scene list. Returns the records; on any malformed line the list comes
// back empty and `error` (if non-null) holds a message naming the line.
//
// All-or-nothing, for load_prop_list's reason: a list that drops the row it
// could not read produces a world that loads, loads wrong, and says nothing.
//
// A missing file is not an error and yields an empty list. The caller falls back
// to its own built-in default, so shipping this file is additive and deleting it
// cannot stop the game booting.
//
// Two conditions beyond a malformed line are also errors, because both are
// otherwise silent: a duplicate name, which makes "switch to X" ambiguous, and
// an empty list from a file that existed but held only comments.
std::vector<SceneDef> load_scene_list(const std::string& path, std::string* error = nullptr);

// The list main.cpp uses when assets/scenes.txt is absent. Here rather than in
// main.cpp so the fallback is testable, and so that "the file is missing" and
// "the file lists one scene" produce provably the same world.
std::vector<SceneDef> default_scene_list();

} // namespace scene_list
