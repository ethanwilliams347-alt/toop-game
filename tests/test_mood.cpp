// The mood file format (render/mood.h).
//
// Most of this asserts that a malformed file is rejected whole: a suite that
// only checked the good lines survived would pass on the exact bug the
// all-or-nothing rule exists to catch. Then the shipped files: every
// *_mood.txt under assets/ is found by name and parsed, and every scene's
// mood= with it, so a mood can be authored before any scene uses it and still
// be held to the format.

#include "render/mood.h"
#include "scene/scene_list.h"
#include "test_util.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

const char* TMP = "test_mood.tmp_mood.txt";

struct Result {
    mood::Mood m;
    bool ok = false;
    std::string error;
};

Result parse(const std::string& body) {
    {
        std::ofstream out(TMP, std::ios::binary);
        out << body;
    }
    Result r;
    r.ok = mood::load(TMP, r.m, &r.error);
    return r;
}

bool approx(float a, float b) { return a - b < 1e-4f && b - a < 1e-4f; }

// Rejected, with a message naming `line`, and `out` back at legacy().
void rejects(const char* name, const std::string& body, int line) {
    const Result r = parse(body);
    const std::string at =
        line > 0 ? std::string(TMP) + ":" + std::to_string(line) + ":" : std::string(TMP) + ":";
    check(name, !r.ok && r.error.compare(0, at.size(), at) == 0 && r.m == mood::legacy(), r.error);
}

constexpr const char* FULL = "# a night mood\n"
                             "light_model multiply\n"
                             "ambient 0.18 0.22 0.34\n"
                             "sun 0.10 0.12 0.20 dir=0,1 shadow=off   # moonlight\n"
                             "fog 0.10 0.12 0.18 near=0.1 far=0.9 amount=0.7\n"
                             "bloom threshold=0.9 strength=0.6\n"
                             "grade night_grade.bmp\n"
                             "vignette 0.35\n"
                             "grain 0.04\n"
                             "weather rain density=0.7 angle=12 speed=60\n"
                             "\n"
                             "light x=210 y=150 color=1,0.62,0.25 radius=48 flicker=0.1\n"
                             "light x=420 y=140 color=0.2,0.9,1.4 radius=30\n"
                             "material Water tint=0.7,0.85,0.9 reflect=0.6 spec=0.4 depth=0.5\n"
                             "material Wall texture=wall_rock strength=0.35 rim=0.5\n";

} // namespace

int main() {
    // --- legacy ---------------------------------------------------------------
    {
        const mood::Mood l = mood::legacy();
        check("legacy: additive light", l.light_model == mood::LightModel::Legacy);
        check("legacy: ambient is 1 (no darkening)",
              l.ambient.r == 1.0f && l.ambient.g == 1.0f && l.ambient.b == 1.0f);
        check("legacy: no sun, fog, bloom or grade",
              !l.sun.on && !l.fog.on && !l.bloom.on && l.grade.empty());
        check("legacy: no vignette, grain or weather",
              l.vignette == 0.0f && l.grain == 0.0f && l.weather.kind == mood::WeatherKind::None);
        check("legacy: no lights", l.lights.empty());
        bool looks = true;
        for (const mood::MaterialLook& m : l.materials)
            looks = looks && !m.set && m == mood::MaterialLook{};
        check("legacy: no material looks", looks);
        check("legacy: is a default Mood", l == mood::Mood{});

        const Result r = parse("light_model legacy\n");
        check("legacy: the one-line file loads equal to legacy()", r.ok && r.m == l, r.error);

        std::string err = "untouched";
        const mood::Mood s = mood::load_for_scene("", "assets/", &err);
        check("legacy: a scene with no mood= gets legacy() and no error",
              s == l && err == "untouched");
        const mood::Mood bad = mood::load_for_scene("no_such_mood.txt", "nowhere/", &err);
        check("legacy: a scene whose mood fails gets legacy() and the error",
              bad == l && err.find("nowhere/no_such_mood.txt") == 0, err);
    }

    // --- a full file ------------------------------------------------------------
    {
        const Result r = parse(FULL);
        check("full: parses", r.ok && r.error.empty(), r.error);
        const mood::Mood& m = r.m;
        check("full: light model", m.light_model == mood::LightModel::Multiply);
        check("full: ambient", approx(m.ambient.r, 0.18f) && approx(m.ambient.g, 0.22f) &&
                                   approx(m.ambient.b, 0.34f));
        check("full: sun", m.sun.on && approx(m.sun.color.b, 0.20f) && !m.sun.shadow &&
                               approx(m.sun.dir_x, 0.0f) && approx(m.sun.dir_y, 1.0f));
        check("full: fog", m.fog.on && approx(m.fog.near_depth, 0.1f) &&
                               approx(m.fog.far_depth, 0.9f) && approx(m.fog.amount, 0.7f));
        check("full: bloom",
              m.bloom.on && approx(m.bloom.threshold, 0.9f) && approx(m.bloom.strength, 0.6f));
        check("full: grade resolves in the mood file's folder (here, the cwd)",
              m.grade == "night_grade.bmp", m.grade);
        check("full: vignette and grain", approx(m.vignette, 0.35f) && approx(m.grain, 0.04f));
        check("full: weather",
              m.weather.kind == mood::WeatherKind::Rain && approx(m.weather.density, 0.7f) &&
                  approx(m.weather.angle, 12.0f) && approx(m.weather.speed, 60.0f));
        check("full: two lights, in file order, with their lines",
              m.lights.size() == 2 && m.lights[0].x == 210 && m.lights[0].y == 150 &&
                  m.lights[0].line == 12 && m.lights[1].x == 420 && m.lights[1].line == 13);
        if (m.lights.size() == 2) {
            check("full: light colour and radius", approx(m.lights[0].color.g, 0.62f) &&
                                                       approx(m.lights[0].radius, 48.0f) &&
                                                       approx(m.lights[0].flicker, 0.1f));
            check("full: flicker defaults to 0, colour may be over-bright",
                  m.lights[1].flicker == 0.0f && approx(m.lights[1].color.b, 1.4f));
        }
        const mood::MaterialLook& water = m.look(ElementType::Water);
        check("full: water look", water.set && approx(water.tint.g, 0.85f) &&
                                      approx(water.reflect, 0.6f) && approx(water.spec, 0.4f) &&
                                      approx(water.depth, 0.5f) && water.texture.empty() &&
                                      water.rim == 0.0f && water.line == 14);
        const mood::MaterialLook& wall = m.look(ElementType::Wall);
        check("full: wall look, untouched fields at their defaults",
              wall.set && wall.texture == "wall_rock" && approx(wall.strength, 0.35f) &&
                  approx(wall.rim, 0.5f) && wall.tint == (mood::Rgb{1.0f, 1.0f, 1.0f}));
        check("full: a material with no line has no look",
              !m.look(ElementType::Sand).set && m.look(ElementType::Sand) == mood::MaterialLook{});
    }

    // Order is free, and the sun's direction is normalised.
    {
        const Result r = parse("sun 1 1 1 dir=1,1\nambient 0 0 0\nlight_model multiply\n");
        check("order: records in any order", r.ok, r.error);
        check("order: dir is stored normalised", approx(r.m.sun.dir_x, 0.70710678f) &&
                                                     approx(r.m.sun.dir_y, 0.70710678f) &&
                                                     r.m.sun.shadow);
    }
    {
        const Result r = parse("light_model legacy\nweather none\n"
                               "light x=0 y=0 color=1,1,1 radius=1\n");
        check("legacy model takes point lights and an explicit `weather none`", r.ok, r.error);
    }
    {
        const Result r = parse("light_model legacy\nmaterial Charred tint=2,0,0.5\n"
                               "material Sand texture=sand_grain\n");
        check("material: every non-Empty material name works, tint up to 2",
              r.ok && r.m.look(ElementType::Charred).set &&
                  r.m.look(ElementType::Sand).texture == "sand_grain",
              r.error);
    }
    {
        std::string body = "light_model multiply\n";
        for (size_t i = 0; i < mood::MAX_LIGHTS; ++i)
            body += "light x=1 y=1 color=1,1,1 radius=4\n";
        const Result r = parse(body);
        check("lights: MAX_LIGHTS of them load", r.ok && r.m.lights.size() == mood::MAX_LIGHTS,
              r.error);
        rejects("lights: one more is refused at its line",
                body + "light x=1 y=1 color=1,1,1 radius=4\n",
                static_cast<int>(mood::MAX_LIGHTS) + 2);
    }

    // --- rejections -------------------------------------------------------------
    //
    // Each names the offending line; the whole file is lost, and `out` is legacy.
    const std::string M = "light_model multiply\n";  // line 1 of most cases below
    rejects("reject: an empty file", "", 0);
    {
        mood::Mood m;
        m.vignette = 0.5f;
        std::string err;
        check("reject: a file that cannot be opened is an error",
              !mood::load("no_such_file_mood.txt", m, &err) && !err.empty() && m == mood::legacy(),
              err);
    }
    rejects("reject: no light_model", "vignette 0.2\n", 0);
    rejects("reject: comments only", "# nothing\n\n", 0);
    rejects("reject: unknown record", M + "sparkle 1\n", 2);
    rejects("reject: bad light_model", "light_model add\n", 1);
    rejects("reject: light_model with an extra field", "light_model legacy multiply\n", 1);
    rejects("reject: light_model twice", M + "light_model multiply\n", 2);
    rejects("reject: ambient twice", M + "ambient 1 1 1\nambient 1 1 1\n", 3);
    rejects("reject: ambient with two numbers", M + "ambient 1 1\n", 2);
    rejects("reject: ambient with four", M + "ambient 1 1 1 1\n", 2);
    rejects("reject: ambient not a number", M + "ambient 1 1 x\n", 2);
    rejects("reject: ambient with an exponent", M + "ambient 1e0 1 1\n", 2);
    rejects("reject: ambient with a leading +", M + "ambient +1 1 1\n", 2);
    rejects("reject: ambient with a unit", M + "ambient 1 1 1f\n", 2);
    rejects("reject: ambient negative", M + "ambient -0.1 1 1\n", 2);
    rejects("reject: ambient above 8", M + "ambient 1 8.5 1\n", 2);
    rejects("reject: ambient under light_model legacy", "ambient 1 1 1\nlight_model legacy\n", 1);
    rejects("reject: sun under light_model legacy", "light_model legacy\nsun 1 1 1 dir=0,1\n", 2);
    rejects("reject: sun without dir", M + "sun 1 1 1\n", 2);
    rejects("reject: sun dir of zero", M + "sun 1 1 1 dir=0,0\n", 2);
    rejects("reject: sun dir out of range", M + "sun 1 1 1 dir=0,2\n", 2);
    rejects("reject: sun dir with one component", M + "sun 1 1 1 dir=1\n", 2);
    rejects("reject: sun shadow not on/off", M + "sun 1 1 1 dir=0,1 shadow=yes\n", 2);
    rejects("reject: sun unknown key", M + "sun 1 1 1 dir=0,1 glow=1\n", 2);
    rejects("reject: sun repeated key", M + "sun 1 1 1 dir=0,1 dir=1,0\n", 2);
    rejects("reject: sun bare word", M + "sun 1 1 1 dir=0,1 bright\n", 2);
    rejects("reject: sun twice", M + "sun 1 1 1 dir=0,1\nsun 1 1 1 dir=0,1\n", 3);
    rejects("reject: fog colour above 1", M + "fog 1.5 1 1 near=0 far=1 amount=1\n", 2);
    rejects("reject: fog without amount", M + "fog 1 1 1 near=0 far=1\n", 2);
    rejects("reject: fog near >= far", M + "fog 1 1 1 near=0.5 far=0.5 amount=1\n", 2);
    rejects("reject: fog far above 1", M + "fog 1 1 1 near=0 far=1.5 amount=1\n", 2);
    rejects("reject: fog unknown key", M + "fog 1 1 1 near=0 far=1 amount=1 thick=1\n", 2);
    rejects("reject: bloom without strength", M + "bloom threshold=1\n", 2);
    rejects("reject: bloom strength above 4", M + "bloom threshold=1 strength=5\n", 2);
    rejects("reject: bloom threshold negative", M + "bloom threshold=-1 strength=1\n", 2);
    rejects("reject: bloom positional", M + "bloom 1 1\n", 2);
    rejects("reject: grade not a bmp", M + "grade night.png\n", 2);
    rejects("reject: grade in another folder", M + "grade ../night.bmp\n", 2);
    rejects("reject: grade with a separator", M + "grade sub/night.bmp\n", 2);
    rejects("reject: grade with no file", M + "grade\n", 2);
    rejects("reject: grade with two files", M + "grade a.bmp b.bmp\n", 2);
    rejects("reject: grade twice", M + "grade a.bmp\ngrade a.bmp\n", 3);
    rejects("reject: vignette above 1", M + "vignette 1.5\n", 2);
    rejects("reject: vignette missing", M + "vignette\n", 2);
    rejects("reject: vignette twice", M + "vignette 0.1\nvignette 0.2\n", 3);
    rejects("reject: grain negative", M + "grain -0.1\n", 2);
    rejects("reject: grain two numbers", M + "grain 0.1 0.2\n", 2);
    rejects("reject: weather unknown kind", M + "weather snow density=1 angle=0 speed=1\n", 2);
    rejects("reject: weather none with fields", M + "weather none density=1\n", 2);
    rejects("reject: weather rain without speed", M + "weather rain density=1 angle=0\n", 2);
    rejects("reject: weather angle out of range", M + "weather rain density=1 angle=91 speed=1\n",
            2);
    rejects("reject: weather density above 1", M + "weather dust density=2 angle=0 speed=1\n", 2);
    rejects("reject: weather twice", M + "weather none\nweather fog density=1 angle=0 speed=1\n",
            3);
    rejects("reject: light without radius", M + "light x=1 y=1 color=1,1,1\n", 2);
    rejects("reject: light negative x", M + "light x=-1 y=1 color=1,1,1 radius=1\n", 2);
    rejects("reject: light fractional y", M + "light x=1 y=1.5 color=1,1,1 radius=1\n", 2);
    rejects("reject: light colour as three words", M + "light x=1 y=1 color=1 1 1 radius=1\n", 2);
    rejects("reject: light colour with two parts", M + "light x=1 y=1 color=1,1 radius=1\n", 2);
    rejects("reject: light colour above 8", M + "light x=1 y=1 color=1,9,1 radius=1\n", 2);
    rejects("reject: light radius 0", M + "light x=1 y=1 color=1,1,1 radius=0\n", 2);
    rejects("reject: light flicker above 1", M + "light x=1 y=1 color=1,1,1 radius=1 flicker=2\n",
            2);
    rejects("reject: light repeated key", M + "light x=1 x=2 y=1 color=1,1,1 radius=1\n", 2);
    rejects("reject: light unknown key", M + "light x=1 y=1 z=1 color=1,1,1 radius=1\n", 2);
    rejects("reject: material unknown name", M + "material Lava tint=1,1,1\n", 2);
    rejects("reject: material name in the wrong case", M + "material water tint=1,1,1\n", 2);
    rejects("reject: material Empty", M + "material Empty tint=1,1,1\n", 2);
    rejects("reject: material with no name", M + "material\n", 2);
    rejects("reject: material with no field", M + "material Sand\n", 2);
    rejects("reject: material named twice", M + "material Sand rim=1\nmaterial Sand spec=1\n", 3);
    rejects("reject: material tint above 2", M + "material Sand tint=1,2.5,1\n", 2);
    rejects("reject: material texture with an extension", M + "material Sand texture=a.bmp\n", 2);
    rejects("reject: material texture with a folder", M + "material Sand texture=a/b\n", 2);
    rejects("reject: material strength above 1", M + "material Sand strength=1.1\n", 2);
    rejects("reject: material repeated key", M + "material Sand rim=0.1 rim=0.2\n", 2);
    rejects("reject: material unknown key", M + "material Sand gloss=1\n", 2);
    rejects("reject: a bad line after good ones loses the whole file",
            M + "ambient 0.5 0.5 0.5\nlight x=1 y=1 color=1,1,1 radius=1\nvignette 2\n", 4);
    std::remove(TMP);

    // --- the shipped files --------------------------------------------------------
    //
    // Every *_mood.txt under assets/ (outside wip/, which never ships), found by
    // name rather than listed, so a mood written before any scene uses it is still
    // held to the format. Its grade and its textures must exist too: a mood whose
    // LUT is missing loads and then draws ungraded, which is the silent failure
    // the parser refuses everywhere else.
    namespace fs = std::filesystem;
    int found = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it("assets", ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path& p = it->path();
        if (it->is_directory() && p.filename() == "wip") {
            it.disable_recursion_pending();
            continue;
        }
        const std::string name = p.filename().string();
        const std::string suffix = "_mood.txt";
        if (!it->is_regular_file() || name.size() <= suffix.size() ||
            name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
            continue;
        ++found;
        const std::string path = p.generic_string();
        // assets/README.md: every file in a set's folder repeats the set's name.
        const std::string folder = p.parent_path().filename().string();
        if (folder != "assets")
            check(("shipped: " + path + " is prefixed with its folder's name").c_str(),
                  name.compare(0, folder.size() + 1, folder + "_") == 0);
        mood::Mood m;
        std::string err;
        const bool ok = mood::load(path, m, &err);
        check(("shipped: " + path + " parses").c_str(), ok, err);
        if (!ok) continue;
        if (!m.grade.empty())
            check(("shipped: " + path + "'s grade exists").c_str(), fs::is_regular_file(m.grade),
                  m.grade);
        for (const mood::MaterialLook& look : m.materials)
            if (!look.texture.empty()) {
                const std::string tex = "assets/materials/" + look.texture + ".bmp";
                check(("shipped: " + path + "'s texture " + look.texture + " exists").c_str(),
                      fs::is_regular_file(tex), tex);
            }
    }
    check("shipped: assets/ could be searched", !ec, ec.message());
    std::printf("       (%d mood file%s under assets/)\n", found, found == 1 ? "" : "s");

    // And every scene that names a mood names one that is there and loads.
    {
        std::string err;
        const std::vector<scene_list::SceneDef> scenes =
            scene_list::load_scene_list("assets/scenes.txt", &err);
        check("shipped: the scene list loads", !scenes.empty(), err);
        for (const scene_list::SceneDef& def : scenes) {
            if (def.mood.empty()) continue;
            err.clear();
            mood::load_for_scene(def.mood, "assets/", &err);
            check(("shipped: scene '" + def.name + "''s mood loads").c_str(), err.empty(), err);
        }
    }

    return report();
}
