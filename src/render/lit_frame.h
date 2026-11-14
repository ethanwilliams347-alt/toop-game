#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// What the GL light passes (src/gpu/pipeline.*) are told, as plain numbers.
//
// Two records, both std-only so the three layers that touch them can share
// them without any of them including another's headers:
//   - render/light.cpp fills LightConstants from the CPU LightField's own
//     tables (LightField::gpu_constants), so the GPU port propagates with the
//     very numbers the CPU reference does -- one place to tune, and
//     gl_frame_test holds the two fields to each other;
//   - render/mood.cpp fills a LitFrame's mood half (mood::fill_lit) from a
//     parsed *_mood.txt;
//   - src/gpu/ reads both and knows nothing about Grid, ElementType or Mood.
// Indexed by ElementType's value where a table is per material; MAX_TYPES only
// has to sit above ElementType::Count (light.cpp asserts it).
namespace lit {

inline constexpr int MAX_TYPES = 16;

// The LightField propagation, as numbers. See render/light.cpp for every one.
struct LightConstants {
    int block = 4;          // LightField::BLOCK
    int iterations = 24;    // LightField::ITERATIONS
    int smooth_passes = 2;  // SMOOTH_PASSES
    // Per-cell log transmission by type (LOG_TRANSMIT), and the solid figure a
    // loose body's hidden cell is given (the pack carries the fill there, not
    // the body, and the body still casts its shadow).
    float log_transmit[MAX_TYPES] = {};
    float log_solid = 0.0f;
    float transmit_clear = 0.0f, transmit_solid = 0.0f;
    float glow_threshold = 100.0f;
    float max_emission = 0.0f;
    float coverage_floor = 0.0f;
    float min_visible = 0.0f;
    float smooth_centre = 2.0f;
    // How much of its own colour a cell emits under the multiply model, by type
    // (Fire 1, the rest 0): under a dark ambient a flame must still read as a
    // flame and not as a dim orange shape.
    float emissive[MAX_TYPES] = {};
    // Per-cell transmission toward the sun, by type, for the sun's shadow
    // march: like log_transmit but with air passing everything, since the sky
    // a sunbeam crosses is not the air light is attenuated by at night.
    float sun_transmit[MAX_TYPES] = {};
};

inline constexpr size_t MAX_POINT_LIGHTS = 64;  // mood::MAX_LIGHTS (mood.cpp asserts it)

struct PointLight {
    float x = 0.0f, y = 0.0f;  // world cells
    float r = 0.0f, g = 0.0f, b = 0.0f;
    float radius = 0.0f;  // cells
    float flicker = 0.0f;
};

// One frame of the multiply light model: the mood's numbers plus where the
// cell pack sits this frame.
struct LitFrame {
    // --- from the mood (mood::fill_lit) ---
    float ambient[3] = {1.0f, 1.0f, 1.0f};
    bool sun_on = false;
    float sun[3] = {0.0f, 0.0f, 0.0f};
    float sun_dir[2] = {0.0f, 1.0f};  // the way the light travels, +y down
    bool sun_shadow = true;
    size_t light_count = 0;
    PointLight lights[MAX_POINT_LIGHTS];
    // How much the propagated fire light (tone-mapped, as the legacy pass draws
    // it) adds to the light a surface gets. Not a mood key yet: 1.
    float fire_gain = 1.0f;

    // --- from the frame ---
    // World cell of the cell pack's texel (0, 0) -- the camera's view origin.
    int origin_x = 0, origin_y = 0;
    // Where that cell's top-left corner lands on screen, in pixels, and the
    // pixels per cell: the same rect frame::draw_cells blits the cells into.
    float screen_x = 0.0f, screen_y = 0.0f;
    int scale = 1;
    float time_s = 0.0f;  // for flicker

    // --- atmosphere (R5 post, R6 weather), from the mood (mood::fill_lit) ---
    // Fog is applied while the backdrop is drawn (GlCanvas::set_fog), since only
    // then is each layer's depth known; it is here so one record carries the
    // whole mood. Colour is linear light, as the mood files' headers say of
    // every colour they hold.
    bool fog_on = false;
    float fog[3] = {0.0f, 0.0f, 0.0f};
    float fog_near = 0.0f, fog_far = 1.0f, fog_amount = 0.0f;
    bool bloom_on = false;
    float bloom_threshold = 1.0f, bloom_strength = 0.0f;
    // The LUT strip's path (Mood::grade), or null for none. Points into the
    // Mood, which outlives the frame; GlCanvas loads and caches the strip.
    const char* grade = nullptr;
    float vignette = 0.0f, grain = 0.0f;
    // 0 none, 1 rain, 2 fog wisps, 3 dust (mood::WeatherKind's order).
    int weather = 0;
    float weather_density = 0.0f, weather_angle_deg = 0.0f, weather_speed = 0.0f;

    // --- from the frame ---
    // The simulation step count: weather and grain are functions of it, never of
    // the wall clock, so a replay or a paused game draws the same frame.
    unsigned long long step = 0;
};

// How the cells themselves look under the multiply model (R4a/R4b): the mood's
// `material` lines plus the few frame numbers the cell shader needs to anchor
// texture to the world and animate water. Unlike LitFrame it is handed to the
// canvas *before* frame::compose (GlCanvas::set_cell_look), because the cells
// are drawn during compose and the resolve comes after it. `on` false -- the
// default, and what every legacy frame keeps -- draws the cells exactly as a
// plain blit, so nothing here can reach the SDL path or a legacy mood.
struct CellLook {
    bool on = false;
    // Per material, by ElementType (mood::MaterialLook, see mood.h for each).
    float tint[MAX_TYPES][3] = {};
    float strength[MAX_TYPES] = {};
    float rim[MAX_TYPES] = {};
    float reflect[MAX_TYPES] = {};
    float spec[MAX_TYPES] = {};
    float depth[MAX_TYPES] = {};
    // The tile stem under assets/materials/ (no extension), empty for none. The
    // canvas loads each stem once and keeps it.
    std::string texture[MAX_TYPES];
    // The sun's direction, the way light travels (+y down), so a rim lights the
    // edge that faces the sun. (0, 1) -- light from straight above -- when the
    // mood has no sun.
    float sun_dir[2] = {0.0f, 1.0f};
    // World cell of the cell pack's texel (0, 0): what anchors a tile to the
    // world rather than to the screen, so a texture does not swim as the camera
    // pans.
    int origin_x = 0, origin_y = 0;
    // Simulation steps since the run began, for caustics and the reflection's
    // wobble. Steps and not the wall clock, so a paused world is a still picture
    // and a replay draws the same water on the same step.
    uint32_t step = 0;
};

// One frame's dynamic lights (an angler's lure), appended after the mood's own
// lights up to MAX_POINT_LIGHTS. Returns how many fitted.
inline size_t append_lights(LitFrame& f, const PointLight* lights, size_t n) {
    size_t added = 0;
    while (added < n && f.light_count < MAX_POINT_LIGHTS)
        f.lights[f.light_count++] = lights[added++];
    return added;
}

}  // namespace lit
