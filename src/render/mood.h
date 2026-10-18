#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "physics/material.h"

// A scene's mood: how its light, air and materials look, Rain World style. A
// daylight scene stays daylight; a noir scene gets night, rain, fog and neon.
// Read from `assets/<set>/<set>[_<variant>]_mood.txt`, which a scene names with
// `mood=<set>/<file>_mood.txt` in assets/scenes.txt (scene_list.h refuses any
// other ending, and any folder but the scene's own backdrop= one). A scene that
// names none gets mood::legacy(), which is exactly today's look. mood_test
// parses every *_mood.txt under assets/, used by a scene or not, and checks
// that its grade and textures exist. assets/README.md has this format as a
// table; this comment is the authority.
//
// --- why a file ------------------------------------------------------------------
//
// Every number here is a look, tuned by eye against one scene's art. In code they
// would be a rebuild per tweak and one table for every scene; as a file they sit
// in the backdrop folder beside the art they were tuned against, and a second
// mood for the same place (bg_tarn at night) is another file, not a branch.
//
// Render data only, so floats are fine here, and nothing in it may ever reach
// the simulation: a mood changes what a cell looks like, never what it is.
// Element::color stays the albedo and stays hashed into replays; everything a
// mood does is applied on top of it at draw time.
//
// SDL-free and GL-free, like backdrop_set: this is the record, and the GL
// passes that consume it (the plan's R3-R6) read it const.
//
// Like every parser here it is all-or-nothing: the first malformed record
// rejects the whole file and names its line (`path:line: why`). A mood missing
// the fog line it could not read is a noir scene in flat daylight that says
// nothing about why. The caller logs the error and draws legacy().
//
// --- the format ------------------------------------------------------------------
//
// One record per line; `#` to end-of-line is a comment; blank lines are fine.
// The first word names the record, and records may come in any order. Fields
// are separated by whitespace, so a colour inside one `key=` field is written
// with commas, `color=1,0.6,0.25`, and a colour that is its record's own
// positional fields is three words, `ambient 0.2 0.25 0.4`. Numbers are plain
// decimals (text_record::decimal: "0.5", ".5", "-1", "2"; never "1e3", "+1",
// "0.5f"); x= and y= are whole numbers. Every record except `light` and
// `material` may appear at most once; within a record every `key=` at most once.
// Every field shown is required unless marked optional. Anything else -- an
// unknown record or key, a missing or extra field, a number out of its range --
// is an error.
//
//   light_model legacy | multiply             REQUIRED, once.
//       legacy:   today's light -- emitters add light on top of the albedo; it
//                 can brighten and never darken. `ambient` and `sun` are
//                 refused under it (a number nothing reads is one an author
//                 tunes for nothing).
//       multiply: albedo x (ambient + sun + lights) + emissive, so a scene can
//                 be darker than its paint.
//
//   ambient <r> <g> <b>                       multiply only; default 1 1 1.
//       Light reaching everything, linear, each 0..8 (above 1 is over-bright).
//
//   sun <r> <g> <b> dir=<x>,<y> [shadow=on|off]   multiply only; default none.
//       Colour 0..8 each. dir is the way the light travels, in cells, +x right
//       and +y DOWN (grid rows): dir=0.3,1 is a high sun from the upper left.
//       Each component -1..1, not both 0; stored normalised. shadow (optional,
//       default on) is whether solids between a cell and the sun shade it.
//
//   fog <r> <g> <b> near=<d> far=<d> amount=<a>   default none.
//       Aerial perspective over the parallax layers. Colour 0..1 each. Depth d
//       is 1 - the layer's parallax factor, clamped to 0..1: 0 is the world
//       plane, 1 the sky. Fog is 0 at `near`, rising to `amount` at `far`;
//       0 <= near < far <= 1, amount 0..1.
//
//   bloom threshold=<t> strength=<s>          default none.
//       Linear luminance above which a pixel blooms (0..16) and how much
//       (0..4).
//
//   grade <file>.bmp                          default none.
//       A LUT strip (tools/make_lut.py) applied last. A plain file name in the
//       mood file's own folder -- which, by the rule in scene_list.h, is the
//       scene's backdrop folder. Mood::grade holds it resolved against that
//       folder ("assets/bg_tarn/bg_tarn_noir_grade.bmp").
//
//   vignette <v>                              0..1, default 0 (none).
//   grain <g>                                 0..1, default 0 (none).
//
//   weather none | rain | fog | dust density=<d> angle=<deg> speed=<cells/s>
//       Default none. `none` takes no fields; the others need all three.
//       density 0..1; angle -90..90 degrees from straight down, positive
//       leaning toward +x; speed 0..2000 cells per second.
//
//   light x=<cell> y=<cell> color=<r>,<g>,<b> radius=<cells> [flicker=<f>]
//       REPEATABLE, up to MAX_LIGHTS. A point light at a world cell (x, y each
//       0..999999, the scene list's size cap). Colour 0..8 each; radius
//       0 < r <= 1024 cells; flicker (optional, default 0) 0..1, how much its
//       brightness wavers. Allowed under either light model.
//
//   material <Name> [tint=<r>,<g>,<b>] [texture=<stem>] [strength=<s>]
//            [rim=<s>] [reflect=<s>] [spec=<s>] [depth=<s>]
//       REPEATABLE, once per material. Name is a MATERIALS name exactly as
//       physics/material.h spells it (Sand, Water, Wall, Wood, Oil, Steam,
//       Fire, Charred; not Empty). At least one field, all optional:
//         tint      multiplies the albedo, 0..2 each; default 1,1,1
//         texture   tile in assets/materials/<stem>.bmp (a bare stem, no
//                   extension, no folder); default none
//         strength  how far the texture moves luminance, 0..1; default 0
//         rim       edge highlight from the solid-mask gradient, 0..1
//         reflect   liquid surface reflection, 0..1
//         spec      specular sheen, 0..1
//         depth     liquid depth tint, 0..1
//       Each of the last five defaults to 0.
//
// A full example (a night mood for the tarn):
//
//     light_model multiply
//     ambient 0.18 0.22 0.34
//     sun 0.10 0.12 0.20 dir=0.3,1 shadow=off       # moonlight
//     fog 0.10 0.12 0.18 near=0.1 far=0.9 amount=0.7
//     bloom threshold=0.9 strength=0.6
//     grade bg_tarn_noir_grade.bmp
//     vignette 0.35
//     grain 0.04
//     weather rain density=0.7 angle=12 speed=60
//     light x=210 y=150 color=1,0.62,0.25 radius=48 flicker=0.1   # sodium lamp
//     light x=420 y=140 color=0.2,0.9,1.4 radius=30 flicker=0.3   # neon
//     material Water tint=0.7,0.85,0.9 reflect=0.6 spec=0.4 depth=0.5
//     material Wall texture=wall_rock strength=0.35 rim=0.5
//
// The smallest file is one line, `light_model legacy`, and loads equal to
// mood::legacy().
namespace mood {

enum class LightModel : uint8_t { Legacy, Multiply };
enum class WeatherKind : uint8_t { None, Rain, Fog, Dust };

// Linear colour. Light colours may exceed 1 (over-bright); surface colours
// (fog, tint) have their own ranges in the format above.
struct Rgb {
    float r = 0.0f, g = 0.0f, b = 0.0f;
    bool operator==(const Rgb&) const = default;
};

struct Sun {
    bool on = false;
    Rgb color;
    float dir_x = 0.0f, dir_y = 1.0f;  // normalised; +y is down
    bool shadow = true;
    bool operator==(const Sun&) const = default;
};

struct Fog {
    bool on = false;
    Rgb color;
    float near_depth = 0.0f, far_depth = 1.0f, amount = 0.0f;
    bool operator==(const Fog&) const = default;
};

struct Bloom {
    bool on = false;
    float threshold = 1.0f, strength = 0.0f;
    bool operator==(const Bloom&) const = default;
};

struct Weather {
    WeatherKind kind = WeatherKind::None;
    float density = 0.0f, angle = 0.0f, speed = 0.0f;  // angle in degrees
    bool operator==(const Weather&) const = default;
};

struct PointLight {
    int x = 0, y = 0;  // world cells
    Rgb color;
    float radius = 0.0f;  // cells
    float flicker = 0.0f;
    int line = 0;  // in the mood file, for diagnostics
    bool operator==(const PointLight&) const = default;
};

// One material's look. The defaults are "no change": tint 1, no texture, every
// effect 0. `set` says a `material` line named it.
struct MaterialLook {
    bool set = false;
    Rgb tint{1.0f, 1.0f, 1.0f};
    std::string texture;  // stem under assets/materials/, or empty
    float strength = 0.0f, rim = 0.0f, reflect = 0.0f, spec = 0.0f, depth = 0.0f;
    int line = 0;
    bool operator==(const MaterialLook&) const = default;
};

// A cap rather than unbounded because the GL pass uploads lights as one fixed
// uniform array, and a mood with more than it holds should fail here, by line,
// rather than drop lights silently on the GPU.
inline constexpr size_t MAX_LIGHTS = 64;

inline constexpr size_t MATERIAL_COUNT = static_cast<size_t>(ElementType::Count);

struct Mood {
    LightModel light_model = LightModel::Legacy;
    Rgb ambient{1.0f, 1.0f, 1.0f};
    Sun sun;
    Fog fog;
    Bloom bloom;
    std::string grade;  // resolved path of the LUT strip, or empty for none
    float vignette = 0.0f;
    float grain = 0.0f;
    Weather weather;
    std::vector<PointLight> lights;
    // Indexed by ElementType, so a draw looks a cell's material up without a
    // search or an allocation.
    std::array<MaterialLook, MATERIAL_COUNT> materials{};

    const MaterialLook& look(ElementType t) const { return materials[static_cast<size_t>(t)]; }
    bool operator==(const Mood&) const = default;
};

// Today's look: additive light, no ambient darkening, no sun, fog, bloom,
// grade, vignette, grain, weather, point lights or material looks. The value a
// default-constructed Mood already has; a function so call sites say what they
// mean.
Mood legacy();

// Parses `path`. On success fills `out` and returns true. On the first
// malformed record returns false, sets `out` to legacy() and `*error` (if
// non-null) to "path:line: why". A missing file is an error too, for
// backdrop_set::load's reason: a scene that names a mood and gets none is a
// scene drawn in the wrong light.
bool load(const std::string& path, Mood& out, std::string* error);

// The mood a scene row asks for: legacy() when `field` (SceneDef::mood) is
// empty, else load(assets_dir + field). A failure returns legacy() with
// `*error` set, so the caller only has to print it.
Mood load_for_scene(const std::string& field, const std::string& assets_dir, std::string* error);

}  // namespace mood
