#pragma once
#include <string>
#include <vector>
#include "gpu/targets.h"
#include "render/lit_frame.h"

// The multiply light model's GPU passes: light, resolve, weather, post -- the
// middle of gpu/pass_graph.h, which GlCanvas::resolve_lit runs after the frame
// code has drawn the backdrop and the world into the albedo/emissive pair.
//
// --- light ---
// A port of LightField::update (render/light.cpp), pass for pass, on the GPU and
// at the same block resolution, with the same numbers (LightField::gpu_constants):
//   gather     one texel per block from the cell pack: emission from the hottest
//              cell and the share that glows, and the block's log transmission
//              (a loose body's hidden cell counts as solid, as its real cell
//              does on the CPU);
//   propagate  ITERATIONS ping-pong passes of the same max-propagation, with
//              the orthogonal, diagonal and knight's-move steps at their own
//              costs and the emission as a floor;
//   smooth     the same openness-weighted 1-2-1 passes.
// The CPU stops early once an iteration changes nothing visible; the GPU runs
// them all, which only adds what the CPU decided was under 1/512. The CPU field
// stays the reference and the legacy model's light: gl_frame_test reads this
// field back and holds it to LightField's texels.
//
// Plus the sun's visibility, one texel per cell: a march from each cell toward
// the sun through the pack, multiplying what each cell crossed passes (air
// passes all of it). Only the window the pack covers is known, so a roof above
// the top of the screen does not shade; SUN_MARCH_CELLS caps the march.
//
// --- resolve ---
// Per screen pixel, in linear light:
//     albedo x (ambient + sun x visibility + fire light + point lights) + emissive
// with the albedo decoded from its display (sRGB) encoding first. Fire light is
// the propagated field tone-mapped exactly as the legacy texture is, so a fire
// lights its surroundings about as far and as hard as it does today -- by
// multiplying the paint instead of washing over it. Point lights are the mood's
// `light` lines plus any dynamic ones a caller appends, unshadowed, with a
// smooth (1 - d/r)^2 falloff and a flicker.
//
// --- weather (R6) ---
// The mood's rain, fog wisps or dust, instanced and stateless (weather.vert):
// each particle's position is a function of its index and the simulation time,
// so nothing is stored between frames. Added onto the HDR frame lit by the same
// ambient, sun and point lights as the world, and stopped per pixel at the
// first row in its column that it cannot fall through (weather_top.frag, one
// texel per pack column, rebuilt from the pack each frame).
//
// --- post (R5) ---
// Bloom first: the HDR frame over the mood's threshold, down a chain of
// half-resolution targets to 1/16 and back up, each level added to the one
// above. Then one final pass (post.frag): bloom added, a shoulder tonemap,
// the mood's LUT grade, vignette and grain, and the display encode. With all
// four off the final pass is the display encode alone, clamped, as before.
//
// Fog is not here: it is applied as the backdrop is drawn, the one time each
// layer's depth is known (GlCanvas::set_fog, gfx::Canvas::set_depth).
//
// A mood whose light is neutral -- ambient 1, no sun, no lights, nothing hot --
// resolves to its albedo within rounding: gl_frame_test checks that too, so
// the passes cannot quietly shift every colour.
namespace gpu {

class Pipeline {
public:
    // The cells' pass constants and the frame, plus what GlCanvas owns that the
    // passes read and write.
    struct Inputs {
        unsigned albedo_tex = 0;    // attachment 0 of frame_fbo: read, then overwritten by post
        unsigned emissive_tex = 0;  // attachment 1 of frame_fbo
        unsigned frame_fbo = 0;
        int target_w = 0, target_h = 0;
        unsigned pack_tex = 0;
        int pack_w = 0, pack_h = 0;
        const lit::LightConstants* constants = nullptr;
        const lit::LitFrame* frame = nullptr;
    };

    Pipeline() = default;
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    ~Pipeline() = default;  // GL objects go in release(), under a current context

    // Compiles every pass's program from `shader_dir`. On failure keeps what it
    // had (a hot reload keeps drawing) and puts the log in `log`.
    bool build(const std::string& shader_dir, std::string& log);
    void release();

    // Runs light, resolve, weather and post. Leaves frame_fbo bound with only
    // attachment 0 drawn to, blending off. False, with `why` set, if a target
    // could not be made; the frame then holds the unlit albedo.
    bool run(const Inputs& in, std::string& why);

    // The last run's propagated light, one RGB triple per block, row-major, top
    // row first, linear (not tone-mapped). For gl_frame_test.
    void read_blocks(std::vector<float>& rgb, int& cols, int& rows) const;
    // The last run's sun visibility, one value per cell, top row first.
    void read_sun(std::vector<float>& vis, int& w, int& h) const;

    // A LUT strip for the grade (tools/make_lut.py's layout: 1024 x 32, RGBA
    // bytes, top row first), or null to drop it. The frame grades only while
    // one is set and the frame names a grade.
    void set_lut(const unsigned char* rgba, int w, int h);
    bool has_lut() const { return lut_ != 0; }

    // --- GPU timing ---
    // GL_TIME_ELAPSED per pass, double-buffered: frame N reads the queries of
    // frame N-2, which the GPU finished long ago, so the timers never stall the
    // CPU. `canvas` is the frame code's backdrop and world draws (from
    // GlCanvas::begin_lit_frame to the resolve); the UI is not timed.
    enum Timer : int { T_CANVAS, T_LIGHT, T_RESOLVE, T_WEATHER, T_BLOOM, T_POST, TIMER_COUNT };
    static const char* timer_name(int t);
    void begin_canvas_timer();
    // Milliseconds per Timer, the latest results that are in. With `wait`, the
    // frame just drawn is waited for and read (preview_frame's single frame);
    // otherwise only what is already available, never stalling.
    void gpu_ms(float out[TIMER_COUNT], bool wait);

    // How far the sun's shadow march looks, in cells. A cell with no occluder
    // within this many cells toward the sun is in full sun.
    static constexpr int SUN_MARCH_CELLS = 96;

private:
    struct Programs {
        unsigned gather = 0, propagate = 0, smooth = 0, sun = 0, resolve = 0, post = 0;
        unsigned bloom_down = 0, bloom_up = 0, weather_top = 0, weather = 0;
    };
    static void set_point_lights(unsigned program, const lit::LitFrame& f);
    void run_bloom(const lit::LitFrame& f);
    void run_weather(const Inputs& in, const lit::LitFrame& f);
    void begin_timer(int t);
    void end_timer();
    void collect(int set, bool wait);
    static void release(Programs& p);

    Programs prog_;
    unsigned vao_ = 0;  // core profile draws need one bound; the passes have no attributes
    RenderTarget emit_;        // gather: emission rgb, log transmission a
    RenderTarget blocks_[2];   // propagate / smooth ping-pong
    int front_ = 0;            // which of blocks_ holds the result
    RenderTarget sun_;
    RenderTarget hdr_;
    // Bloom: 1/2, 1/4, 1/8 and 1/16 of the target, linear filtered.
    static constexpr int BLOOM_LEVELS = 4;
    RenderTarget bloom_[BLOOM_LEVELS];
    RenderTarget top_;  // weather's stop row per pack column
    unsigned lut_ = 0;  // the grade strip, RGBA8, linear
    bool bloom_ran_ = false;  // this frame's bloom chain holds this frame's glow

    unsigned queries_[2][TIMER_COUNT] = {};
    bool issued_[2][TIMER_COUNT] = {};
    float ms_[TIMER_COUNT] = {};
    int set_ = 0;  // which half of queries_ this frame writes
    int open_timer_ = -1;  // the query running now, or -1
};

}  // namespace gpu
