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
//     # name    material           albedo            props           spawn    mode      w     h    scale
//     fixture    test_material.bmp  test_albedo.bmp   test_props.txt  terrain
//     empty      -                  -                 -               floor    infinite  1920  1080
//     bg1        bg1_material.bmp   bg1_albedo.bmp    -               terrain  fixed      344   144  10
//
// Five fields required, `-` meaning none for the three filenames. Blank lines
// are fine. Anything else is an error with a line number, never a silently
// skipped row.
//
// Three optional fields follow: `mode`, then `width` and `height` together. An
// omitted field is not an ignored one -- it is a row that made no claim, and the
// default it falls back to is stated at the field. `mode` is `fixed` (a bounded
// world with solid borders and non-repeating backdrops) or `infinite`
// (unbounded horizontal travel with tiling backdrops), defaulting to `fixed`.
// `width height` must appear as a pair, since half a size is not a size; omitted
// they mean derive it from the material BMP's own dimensions, or the engine
// default when the scene names no BMP.
//
// Named fields, `key=value`, may follow anywhere after the five required ones:
//
//     level=<file>     the level file (scene/level_list.h), resolved under assets/
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
    // Unbounded horizontal travel. The camera does not clamp horizontally and
    // the backdrop layers tile through backdrop_wrap::wrap_axis. Vertical is
    // still clamped, because the world has a floor and a ceiling either way.
    Infinite,
};

struct SceneDef {
    std::string name;  // the scene's own name; also what the HUD shows
    std::string material;  // asset stem or empty; resolves to assets/<stem>
    std::string albedo;     // asset stem or empty
    std::string props;      // asset stem or empty
    // `level=<file>`: what the level puts in its world -- player column,
    // objective, enemies (scene/level_list.h). Empty means every default.
    std::string level;
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

    bool is_infinite() const { return mode == SceneMode::Infinite; }
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

// True if `name` is something this loader will turn into a path or a scene name.
// Rejects empty names, path separators and `..`, so a scene list can never reach
// outside assets/. Exposed for the test, which is the only reason it is not
// static.
bool scene_name_ok(const std::string& name);

// The list main.cpp uses when assets/scenes.txt is absent. Here rather than in
// main.cpp so the fallback is testable, and so that "the file is missing" and
// "the file lists one scene" produce provably the same world.
std::vector<SceneDef> default_scene_list();

} // namespace scene_list
