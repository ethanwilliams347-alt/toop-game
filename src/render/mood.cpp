#include "render/mood.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>
#include "util/text_record.h"

namespace mood {

// Every number is read whole, by text_record::decimal and text_record::integer,
// and then range-checked, so "0.5x" and "1e3" are "not a number" and "-0.2" is
// "out of range" -- two different mistakes, named differently.

namespace {

bool in_range(float v, float lo, float hi) { return v >= lo && v <= hi; }

// "r,g,b" -- one field, commas between, each component lo..hi.
bool parse_rgb_commas(std::string_view raw, float lo, float hi, Rgb& out) {
    const size_t c1 = raw.find(',');
    if (c1 == std::string_view::npos) return false;
    const size_t c2 = raw.find(',', c1 + 1);
    if (c2 == std::string_view::npos) return false;
    Rgb c;
    if (!text_record::decimal(raw.substr(0, c1), c.r) ||
        !text_record::decimal(raw.substr(c1 + 1, c2 - c1 - 1), c.g) ||
        !text_record::decimal(raw.substr(c2 + 1), c.b))
        return false;
    if (!in_range(c.r, lo, hi) || !in_range(c.g, lo, hi) || !in_range(c.b, lo, hi)) return false;
    out = c;
    return true;
}

// Three positional words, each lo..hi.
bool parse_rgb_words(std::istringstream& fields, float lo, float hi, Rgb& out) {
    std::string r, g, b;
    if (!(fields >> r >> g >> b)) return false;
    Rgb c;
    if (!text_record::decimal(r, c.r) || !text_record::decimal(g, c.g) ||
        !text_record::decimal(b, c.b))
        return false;
    if (!in_range(c.r, lo, hi) || !in_range(c.g, lo, hi) || !in_range(c.b, lo, hi)) return false;
    out = c;
    return true;
}

// The `key=value` fields after a record's positional ones, each key at most
// once. Read into a small list first so a record can check which it got before
// interpreting any, and so a repeated key is caught whatever its value.
struct KeyValue {
    std::string key, value;
};

// Fills `out`; on a token with no `=`, an empty key or a repeated key returns
// false and says which token in `bad`.
bool read_keys(std::istringstream& fields, std::vector<KeyValue>& out, std::string& bad) {
    std::string tok;
    while (fields >> tok) {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos || eq == 0) {
            bad = "'" + tok + "' is not a key=value field";
            return false;
        }
        KeyValue kv{tok.substr(0, eq), tok.substr(eq + 1)};
        for (const KeyValue& seen : out)
            if (seen.key == kv.key) {
                bad = "`" + kv.key + "=` is given twice";
                return false;
            }
        out.push_back(std::move(kv));
    }
    return true;
}

// The ElementType a `material` line names, by its MATERIALS name exactly.
// Empty is refused: it is the absence of a material, drawn as whatever is
// behind it, and a look for it would be a look for nothing.
bool element_named(const std::string& name, ElementType& out) {
    for (size_t i = 1; i < MATERIAL_COUNT; ++i) {
        if (name == MATERIALS[i].name) {
            out = static_cast<ElementType>(i);
            return true;
        }
    }
    return false;
}

std::string material_names() {
    std::string s;
    for (size_t i = 1; i < MATERIAL_COUNT; ++i) {
        if (!s.empty()) s += ", ";
        s += MATERIALS[i].name;
    }
    return s;
}

} // namespace

Mood legacy() { return Mood{}; }

bool load(const std::string& path, Mood& out, std::string* error) {
    out = legacy();
    Mood m;

    int line_no = 0;
    auto fail = [&](const std::string& why) {
        if (error) *error = text_record::at(path, line_no, why);
        out = legacy();
        return false;
    };

    std::ifstream in(path);
    if (!in) return fail("cannot be opened");

    // The folder the grade LUT is resolved in: the mood file's own.
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);

    // The line each once-only record was first seen on; 0 is not yet.
    int model_line = 0, ambient_line = 0, sun_line = 0, fog_line = 0, bloom_line = 0;
    int grade_line = 0, vignette_line = 0, grain_line = 0, weather_line = 0;

    std::string line;
    while (std::getline(in, line)) {
        ++line_no;
        text_record::strip_comment(line);

        std::istringstream fields(line);
        std::string kind;
        if (!(fields >> kind)) continue;

        const auto twice = [&](int first) {
            return fail("a second `" + kind + "` line (the first is line " + std::to_string(first) +
                        ")");
        };
        // A once-only record with nothing after its positional fields.
        const auto no_more = [&]() -> bool {
            std::string extra;
            return !(fields >> extra);
        };

        if (kind == "light_model") {
            if (model_line) return twice(model_line);
            model_line = line_no;
            std::string v;
            fields >> v;
            if (v == "legacy")
                m.light_model = LightModel::Legacy;
            else if (v == "multiply")
                m.light_model = LightModel::Multiply;
            else
                return fail("`light_model` is `legacy` or `multiply`, not '" + v + "'");
            if (!no_more()) return fail("unexpected field after `light_model`");
        } else if (kind == "ambient") {
            if (ambient_line) return twice(ambient_line);
            ambient_line = line_no;
            if (!parse_rgb_words(fields, 0.0f, 8.0f, m.ambient) || !no_more())
                return fail("`ambient` needs three numbers r g b, each 0 to 8");
        } else if (kind == "sun") {
            if (sun_line) return twice(sun_line);
            sun_line = line_no;
            if (!parse_rgb_words(fields, 0.0f, 8.0f, m.sun.color))
                return fail("`sun` starts with three numbers r g b, each 0 to 8");
            std::vector<KeyValue> kvs;
            std::string bad;
            if (!read_keys(fields, kvs, bad)) return fail("sun: " + bad);
            bool has_dir = false;
            for (const KeyValue& kv : kvs) {
                if (kv.key == "dir") {
                    const size_t c = kv.value.find(',');
                    float x = 0.0f, y = 0.0f;
                    if (c == std::string::npos ||
                        !text_record::decimal(std::string_view(kv.value).substr(0, c), x) ||
                        !text_record::decimal(std::string_view(kv.value).substr(c + 1), y) ||
                        !in_range(x, -1.0f, 1.0f) || !in_range(y, -1.0f, 1.0f) ||
                        (x == 0.0f && y == 0.0f))
                        return fail("dir='" + kv.value +
                                    "' is not a direction (x,y, each -1 to 1, not both 0)");
                    const float len = std::sqrt(x * x + y * y);
                    m.sun.dir_x = x / len;
                    m.sun.dir_y = y / len;
                    has_dir = true;
                } else if (kv.key == "shadow") {
                    if (kv.value == "on")
                        m.sun.shadow = true;
                    else if (kv.value == "off")
                        m.sun.shadow = false;
                    else
                        return fail("shadow='" + kv.value + "' is `on` or `off`");
                } else {
                    return fail("'" + kv.key + "=' is not a sun field (dir=, shadow=)");
                }
            }
            if (!has_dir) return fail("`sun` needs dir=x,y");
            m.sun.on = true;
        } else if (kind == "fog") {
            if (fog_line) return twice(fog_line);
            fog_line = line_no;
            if (!parse_rgb_words(fields, 0.0f, 1.0f, m.fog.color))
                return fail("`fog` starts with three numbers r g b, each 0 to 1");
            std::vector<KeyValue> kvs;
            std::string bad;
            if (!read_keys(fields, kvs, bad)) return fail("fog: " + bad);
            bool has_near = false, has_far = false, has_amount = false;
            for (const KeyValue& kv : kvs) {
                float* target = nullptr;
                if (kv.key == "near") {
                    target = &m.fog.near_depth;
                    has_near = true;
                } else if (kv.key == "far") {
                    target = &m.fog.far_depth;
                    has_far = true;
                } else if (kv.key == "amount") {
                    target = &m.fog.amount;
                    has_amount = true;
                } else {
                    return fail("'" + kv.key + "=' is not a fog field (near=, far=, amount=)");
                }
                if (!text_record::decimal(kv.value, *target) || !in_range(*target, 0.0f, 1.0f))
                    return fail(kv.key + "='" + kv.value + "' is not a number from 0 to 1");
            }
            if (!has_near || !has_far || !has_amount)
                return fail("`fog` needs near=, far= and amount=");
            if (m.fog.near_depth >= m.fog.far_depth) return fail("`fog` needs near < far");
            m.fog.on = true;
        } else if (kind == "bloom") {
            if (bloom_line) return twice(bloom_line);
            bloom_line = line_no;
            std::vector<KeyValue> kvs;
            std::string bad;
            if (!read_keys(fields, kvs, bad)) return fail("bloom: " + bad);
            bool has_threshold = false, has_strength = false;
            for (const KeyValue& kv : kvs) {
                if (kv.key == "threshold") {
                    if (!text_record::decimal(kv.value, m.bloom.threshold) ||
                        !in_range(m.bloom.threshold, 0.0f, 16.0f))
                        return fail("threshold='" + kv.value + "' is not a number from 0 to 16");
                    has_threshold = true;
                } else if (kv.key == "strength") {
                    if (!text_record::decimal(kv.value, m.bloom.strength) ||
                        !in_range(m.bloom.strength, 0.0f, 4.0f))
                        return fail("strength='" + kv.value + "' is not a number from 0 to 4");
                    has_strength = true;
                } else {
                    return fail("'" + kv.key + "=' is not a bloom field (threshold=, strength=)");
                }
            }
            if (!has_threshold || !has_strength)
                return fail("`bloom` needs threshold= and strength=");
            m.bloom.on = true;
        } else if (kind == "grade") {
            if (grade_line) return twice(grade_line);
            grade_line = line_no;
            // A plain .bmp name in this folder, for backdrop_set's layer reason:
            // no separator, so a mood cannot reach outside its own folder.
            std::string f;
            if (!(fields >> f) || f.size() <= 4 || f.compare(f.size() - 4, 4, ".bmp") != 0 ||
                !text_record::name_ok(f, true) || !no_more())
                return fail("`grade` needs one .bmp file name in the mood file's own folder");
            m.grade = dir + f;
        } else if (kind == "vignette" || kind == "grain") {
            int& seen = kind == "vignette" ? vignette_line : grain_line;
            if (seen) return twice(seen);
            seen = line_no;
            float& v = kind == "vignette" ? m.vignette : m.grain;
            std::string raw;
            if (!(fields >> raw) || !text_record::decimal(raw, v) || !in_range(v, 0.0f, 1.0f) ||
                !no_more())
                return fail("`" + kind + "` needs one number from 0 to 1");
        } else if (kind == "weather") {
            if (weather_line) return twice(weather_line);
            weather_line = line_no;
            std::string k;
            fields >> k;
            if (k == "none")
                m.weather.kind = WeatherKind::None;
            else if (k == "rain")
                m.weather.kind = WeatherKind::Rain;
            else if (k == "fog")
                m.weather.kind = WeatherKind::Fog;
            else if (k == "dust")
                m.weather.kind = WeatherKind::Dust;
            else
                return fail("`weather` is none, rain, fog or dust, not '" + k + "'");
            std::vector<KeyValue> kvs;
            std::string bad;
            if (!read_keys(fields, kvs, bad)) return fail("weather: " + bad);
            if (m.weather.kind == WeatherKind::None) {
                if (!kvs.empty()) return fail("`weather none` takes no fields");
            } else {
                bool has_density = false, has_angle = false, has_speed = false;
                for (const KeyValue& kv : kvs) {
                    if (kv.key == "density") {
                        if (!text_record::decimal(kv.value, m.weather.density) ||
                            !in_range(m.weather.density, 0.0f, 1.0f))
                            return fail("density='" + kv.value + "' is not a number from 0 to 1");
                        has_density = true;
                    } else if (kv.key == "angle") {
                        if (!text_record::decimal(kv.value, m.weather.angle) ||
                            !in_range(m.weather.angle, -90.0f, 90.0f))
                            return fail("angle='" + kv.value +
                                        "' is not a number of degrees from -90 to 90");
                        has_angle = true;
                    } else if (kv.key == "speed") {
                        if (!text_record::decimal(kv.value, m.weather.speed) ||
                            !in_range(m.weather.speed, 0.0f, 2000.0f))
                            return fail("speed='" + kv.value +
                                        "' is not a number of cells per second from 0 to 2000");
                        has_speed = true;
                    } else {
                        return fail("'" + kv.key +
                                    "=' is not a weather field (density=, angle=, speed=)");
                    }
                }
                if (!has_density || !has_angle || !has_speed)
                    return fail("`weather " + k + "` needs density=, angle= and speed=");
            }
        } else if (kind == "light") {
            if (m.lights.size() >= MAX_LIGHTS)
                return fail("more than " + std::to_string(MAX_LIGHTS) + " `light` lines");
            PointLight l;
            l.line = line_no;
            std::vector<KeyValue> kvs;
            std::string bad;
            if (!read_keys(fields, kvs, bad)) return fail("light: " + bad);
            bool has_x = false, has_y = false, has_color = false, has_radius = false;
            for (const KeyValue& kv : kvs) {
                if (kv.key == "x" || kv.key == "y") {
                    int& v = kv.key == "x" ? l.x : l.y;
                    if (!text_record::integer(kv.value, v) || v < 0 || v > 999999)
                        return fail(kv.key + "='" + kv.value +
                                    "' is not a world cell (a whole number, 0 to 999999)");
                    (kv.key == "x" ? has_x : has_y) = true;
                } else if (kv.key == "color") {
                    if (!parse_rgb_commas(kv.value, 0.0f, 8.0f, l.color))
                        return fail("color='" + kv.value + "' is not r,g,b, each 0 to 8");
                    has_color = true;
                } else if (kv.key == "radius") {
                    if (!text_record::decimal(kv.value, l.radius) || l.radius <= 0.0f ||
                        l.radius > 1024.0f)
                        return fail("radius='" + kv.value +
                                    "' is not a number of cells above 0, at most 1024");
                    has_radius = true;
                } else if (kv.key == "flicker") {
                    if (!text_record::decimal(kv.value, l.flicker) ||
                        !in_range(l.flicker, 0.0f, 1.0f))
                        return fail("flicker='" + kv.value + "' is not a number from 0 to 1");
                } else {
                    return fail("'" + kv.key +
                                "=' is not a light field (x=, y=, color=, radius=, flicker=)");
                }
            }
            if (!has_x || !has_y || !has_color || !has_radius)
                return fail("`light` needs x=, y=, color= and radius=");
            m.lights.push_back(l);
        } else if (kind == "material") {
            std::string name;
            ElementType t = ElementType::Empty;
            if (!(fields >> name) || !element_named(name, t))
                return fail("`material` needs a material name first (" + material_names() +
                            "), not '" + name + "'");
            MaterialLook& look = m.materials[static_cast<size_t>(t)];
            if (look.set)
                return fail("material " + name + " already has a look on line " +
                            std::to_string(look.line));
            look.set = true;
            look.line = line_no;
            std::vector<KeyValue> kvs;
            std::string bad;
            if (!read_keys(fields, kvs, bad)) {
                std::string msg = "material ";
                msg += name;
                msg += ": ";
                msg += bad;
                return fail(msg);
            }
            if (kvs.empty())
                return fail("material " + name +
                            " names no field (tint=, texture=, strength=, rim=, reflect=, "
                            "spec=, depth=)");
            for (const KeyValue& kv : kvs) {
                float* unit = nullptr;
                if (kv.key == "tint") {
                    if (!parse_rgb_commas(kv.value, 0.0f, 2.0f, look.tint))
                        return fail("tint='" + kv.value + "' is not r,g,b, each 0 to 2");
                } else if (kv.key == "texture") {
                    // A bare stem: the folder (assets/materials/) and the
                    // extension are the format's, so neither can be mistyped.
                    if (!text_record::name_ok(kv.value))
                        return fail("texture='" + kv.value +
                                    "' is not a tile name (a bare stem under assets/materials/, "
                                    "no extension)");
                    look.texture = kv.value;
                } else if (kv.key == "strength") {
                    unit = &look.strength;
                } else if (kv.key == "rim") {
                    unit = &look.rim;
                } else if (kv.key == "reflect") {
                    unit = &look.reflect;
                } else if (kv.key == "spec") {
                    unit = &look.spec;
                } else if (kv.key == "depth") {
                    unit = &look.depth;
                } else {
                    return fail("'" + kv.key +
                                "=' is not a material field (tint=, texture=, strength=, rim=, "
                                "reflect=, spec=, depth=)");
                }
                if (unit &&
                    (!text_record::decimal(kv.value, *unit) || !in_range(*unit, 0.0f, 1.0f)))
                    return fail(kv.key + "='" + kv.value + "' is not a number from 0 to 1");
            }
        } else {
            return fail("'" + kind +
                        "' is not a mood record (light_model, ambient, sun, fog, bloom, grade, "
                        "vignette, grain, weather, light, material)");
        }
    }

    if (!model_line) {
        line_no = 0;
        return fail("has no `light_model` line; every mood states legacy or multiply");
    }
    // Checked at the end because records come in any order: `ambient` may be
    // written above the `light_model` line that makes it meaningless.
    if (m.light_model == LightModel::Legacy) {
        for (const int seen : {ambient_line, sun_line}) {
            if (!seen) continue;
            line_no = seen;
            return fail(std::string("`") + (seen == ambient_line ? "ambient" : "sun") +
                        "` needs `light_model multiply`; legacy light only adds, so there is "
                        "nothing for it to light");
        }
    }

    out = std::move(m);
    return true;
}

Mood load_for_scene(const std::string& field, const std::string& assets_dir, std::string* error) {
    if (field.empty()) return legacy();
    Mood m;
    load(assets_dir + field, m, error);  // legacy() on failure, with *error set
    return m;
}

}  // namespace mood
