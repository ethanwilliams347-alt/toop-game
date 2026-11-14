#include "gpu/pipeline.h"
#include <algorithm>
#include <cmath>
#include "gpu/gl.h"
#include "gpu/pass_graph.h"
#include "gpu/program.h"

namespace gpu {
namespace {

GLint loc(GLuint p, const char* name) { return gl::GetUniformLocation(p, name); }

// One full-target triangle (fullscreen.vert makes it from gl_VertexID).
void draw_fullscreen() { gl::DrawArrays(GL_TRIANGLES, 0, 3); }

void bind_target(const RenderTarget& t) {
    gl::BindFramebuffer(GL_FRAMEBUFFER, t.fbo);
    gl::Viewport(0, 0, t.w, t.h);
}

void bind_tex(int unit, GLuint tex) {
    gl::ActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + unit));
    gl::BindTexture(GL_TEXTURE_2D, tex);
}

}  // namespace

void Pipeline::release(Programs& p) {
    for (unsigned* id : {&p.gather, &p.propagate, &p.smooth, &p.sun, &p.resolve, &p.post,
                         &p.bloom_down, &p.bloom_up, &p.weather_top, &p.weather}) {
        if (*id) gl::DeleteProgram(*id);
        *id = 0;
    }
}

bool Pipeline::build(const std::string& dir, std::string& log) {
    Programs p;
    const bool ok =
        build_program(dir, "fullscreen.vert", "light_gather.frag", p.gather, log) &&
        build_program(dir, "fullscreen.vert", "light_propagate.frag", p.propagate, log) &&
        build_program(dir, "fullscreen.vert", "light_smooth.frag", p.smooth, log) &&
        build_program(dir, "fullscreen.vert", "sun.frag", p.sun, log) &&
        build_program(dir, "fullscreen.vert", "resolve.frag", p.resolve, log) &&
        build_program(dir, "fullscreen.vert", "post.frag", p.post, log) &&
        build_program(dir, "fullscreen.vert", "bloom_down.frag", p.bloom_down, log) &&
        build_program(dir, "fullscreen.vert", "bloom_up.frag", p.bloom_up, log) &&
        build_program(dir, "fullscreen.vert", "weather_top.frag", p.weather_top, log) &&
        build_program(dir, "weather.vert", "weather.frag", p.weather, log);
    if (!ok) {
        release(p);
        return false;
    }
    release(prog_);
    prog_ = p;
    if (!vao_) gl::GenVertexArrays(1, &vao_);
    return true;
}

void Pipeline::release() {
    release(prog_);
    if (vao_) gl::DeleteVertexArrays(1, &vao_);
    vao_ = 0;
    emit_.release();
    blocks_[0].release();
    blocks_[1].release();
    sun_.release();
    hdr_.release();
    for (RenderTarget& t : bloom_) t.release();
    top_.release();
    if (lut_) gl::DeleteTextures(1, &lut_);
    lut_ = 0;
    if (queries_[0][0]) gl::DeleteQueries(2 * TIMER_COUNT, &queries_[0][0]);
    for (int k = 0; k < 2; ++k)
        for (int t = 0; t < TIMER_COUNT; ++t) {
            queries_[k][t] = 0;
            issued_[k][t] = false;
        }
    open_timer_ = -1;
}

bool Pipeline::run(const Inputs& in, std::string& why) {
    const lit::LightConstants& c = *in.constants;
    const lit::LitFrame& f = *in.frame;
    const int block = std::max(c.block, 1);
    const int cols = std::max(1, (in.pack_w + block - 1) / block);
    const int rows = std::max(1, (in.pack_h + block - 1) / block);
    if (!emit_.ensure(cols, rows, GL_RGBA32F, false, why) ||
        !blocks_[0].ensure(cols, rows, GL_RGBA32F, true, why) ||
        !blocks_[1].ensure(cols, rows, GL_RGBA32F, true, why) ||
        !sun_.ensure(std::max(in.pack_w, 1), std::max(in.pack_h, 1), GL_R16F, true, why) ||
        !hdr_.ensure(in.target_w, in.target_h, GL_RGBA16F, false, why) ||
        !top_.ensure(std::max(in.pack_w, 1), 1, GL_R32F, false, why)) {
        gl::BindFramebuffer(GL_FRAMEBUFFER, in.frame_fbo);
        return false;
    }

    gl::Disable(GL_BLEND);
    gl::BindVertexArray(vao_);
    end_timer();  // the canvas's, if begin_canvas_timer opened one

    // Walked in pass_graph's order, so the table the asserts check is the order
    // that runs. The canvas passes (backdrop, world, ui) are the frame code's.
    for (const pass_graph::Pass& pass : pass_graph::PASSES) {
        switch (pass.id) {
            case pass_graph::Id::Backdrop:
            case pass_graph::Id::Behind:
            case pass_graph::Id::World:
            case pass_graph::Id::Ui:       break;

            case pass_graph::Id::Light: {
                begin_timer(T_LIGHT);
                // gather
                bind_target(emit_);
                gl::UseProgram(prog_.gather);
                bind_tex(0, in.pack_tex);
                gl::Uniform1i(loc(prog_.gather, "u_cells"), 0);
                gl::Uniform1i(loc(prog_.gather, "u_block"), block);
                gl::Uniform1fv(loc(prog_.gather, "u_log_transmit"), lit::MAX_TYPES, c.log_transmit);
                gl::Uniform1f(loc(prog_.gather, "u_log_solid"), c.log_solid);
                gl::Uniform1f(loc(prog_.gather, "u_glow_threshold"), c.glow_threshold);
                gl::Uniform1f(loc(prog_.gather, "u_max_emission"), c.max_emission);
                gl::Uniform1f(loc(prog_.gather, "u_coverage_floor"), c.coverage_floor);
                draw_fullscreen();

                // propagate: the first iteration reads the emission as its front,
                // as the CPU's `front = emission` does.
                gl::UseProgram(prog_.propagate);
                gl::Uniform1i(loc(prog_.propagate, "u_front"), 0);
                gl::Uniform1i(loc(prog_.propagate, "u_emit"), 1);
                gl::Uniform1f(loc(prog_.propagate, "u_min_visible"), c.min_visible);
                bind_tex(1, emit_.tex);
                GLuint front = emit_.tex;
                int out = 0;
                for (int i = 0; i < c.iterations; ++i) {
                    bind_target(blocks_[out]);
                    bind_tex(0, front);
                    draw_fullscreen();
                    front = blocks_[out].tex;
                    out ^= 1;
                }
                // smooth: horizontal then vertical, SMOOTH_PASSES times.
                gl::UseProgram(prog_.smooth);
                gl::Uniform1i(loc(prog_.smooth, "u_front"), 0);
                gl::Uniform1i(loc(prog_.smooth, "u_emit"), 1);
                gl::Uniform1f(loc(prog_.smooth, "u_min_visible"), c.min_visible);
                gl::Uniform1f(loc(prog_.smooth, "u_transmit_clear"), c.transmit_clear);
                gl::Uniform1f(loc(prog_.smooth, "u_transmit_solid"), c.transmit_solid);
                gl::Uniform1f(loc(prog_.smooth, "u_centre"), c.smooth_centre);
                const GLint step = loc(prog_.smooth, "u_step");
                for (int i = 0; i < 2 * c.smooth_passes; ++i) {
                    bind_target(blocks_[out]);
                    bind_tex(0, front);
                    gl::Uniform2i(step, i % 2 == 0 ? 1 : 0, i % 2 == 0 ? 0 : 1);
                    draw_fullscreen();
                    front = blocks_[out].tex;
                    out ^= 1;
                }
                front_ = out ^ 1;
                // No iterations and no smoothing leaves the emission as the field.
                if (front == emit_.tex) {
                    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, emit_.fbo);
                    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, blocks_[front_].fbo);
                    gl::BlitFramebuffer(0, 0, cols, rows, 0, 0, cols, rows, GL_COLOR_BUFFER_BIT,
                                        GL_NEAREST);
                }

                // the sun's visibility
                bind_target(sun_);
                gl::UseProgram(prog_.sun);
                bind_tex(0, in.pack_tex);
                gl::Uniform1i(loc(prog_.sun, "u_cells"), 0);
                gl::Uniform2f(loc(prog_.sun, "u_dir"), f.sun_dir[0], f.sun_dir[1]);
                gl::Uniform1i(loc(prog_.sun, "u_shadow"), f.sun_on && f.sun_shadow ? 1 : 0);
                gl::Uniform1i(loc(prog_.sun, "u_march"), SUN_MARCH_CELLS);
                gl::Uniform1fv(loc(prog_.sun, "u_sun_transmit"), lit::MAX_TYPES, c.sun_transmit);
                gl::Uniform1f(loc(prog_.sun, "u_solid"), std::exp(c.log_solid));
                draw_fullscreen();
                end_timer();
                break;
            }

            case pass_graph::Id::Resolve: {
                begin_timer(T_RESOLVE);
                bind_target(hdr_);
                const GLuint p = prog_.resolve;
                gl::UseProgram(p);
                bind_tex(0, in.albedo_tex);
                bind_tex(1, in.emissive_tex);
                bind_tex(2, blocks_[front_].tex);
                bind_tex(3, sun_.tex);
                gl::Uniform1i(loc(p, "u_albedo"), 0);
                gl::Uniform1i(loc(p, "u_emissive"), 1);
                gl::Uniform1i(loc(p, "u_blocks"), 2);
                gl::Uniform1i(loc(p, "u_sun"), 3);
                gl::Uniform1f(loc(p, "u_target_h"), static_cast<float>(in.target_h));
                gl::Uniform2f(loc(p, "u_origin_px"), f.screen_x, f.screen_y);
                gl::Uniform1f(loc(p, "u_scale"), static_cast<float>(std::max(f.scale, 1)));
                gl::Uniform2f(loc(p, "u_view"), static_cast<float>(f.origin_x),
                              static_cast<float>(f.origin_y));
                gl::Uniform2f(loc(p, "u_block_extent"), static_cast<float>(cols * block),
                              static_cast<float>(rows * block));
                gl::Uniform2f(loc(p, "u_pack_size"), static_cast<float>(std::max(in.pack_w, 1)),
                              static_cast<float>(std::max(in.pack_h, 1)));
                gl::Uniform3f(loc(p, "u_ambient"), f.ambient[0], f.ambient[1], f.ambient[2]);
                gl::Uniform1i(loc(p, "u_sun_on"), f.sun_on ? 1 : 0);
                gl::Uniform3f(loc(p, "u_sun_color"), f.sun[0], f.sun[1], f.sun[2]);
                gl::Uniform1f(loc(p, "u_fire_gain"), f.fire_gain);
                gl::Uniform1f(loc(p, "u_time"), f.time_s);
                set_point_lights(p, f);
                draw_fullscreen();
                end_timer();
                break;
            }

            case pass_graph::Id::Weather:
                begin_timer(T_WEATHER);
                run_weather(in, f);
                end_timer();
                break;

            case pass_graph::Id::Bloom:
                begin_timer(T_BLOOM);
                run_bloom(f);
                end_timer();
                break;

            case pass_graph::Id::Post: {
                begin_timer(T_POST);
                gl::BindFramebuffer(GL_FRAMEBUFFER, in.frame_fbo);
                const GLenum one[1] = {GL_COLOR_ATTACHMENT0};
                gl::DrawBuffers(1, one);
                gl::Viewport(0, 0, in.target_w, in.target_h);
                const GLuint p = prog_.post;
                gl::UseProgram(p);
                bind_tex(0, hdr_.tex);
                bind_tex(1, bloom_[0].tex);
                bind_tex(2, lut_);
                gl::Uniform1i(loc(p, "u_hdr"), 0);
                gl::Uniform1i(loc(p, "u_bloom"), 1);
                gl::Uniform1i(loc(p, "u_lut"), 2);
                const bool bloom = bloom_ran_;
                const bool grade = f.grade && lut_;
                gl::Uniform1i(loc(p, "u_post"),
                              bloom || grade || f.vignette > 0.0f || f.grain > 0.0f ? 1 : 0);
                gl::Uniform1f(loc(p, "u_bloom_strength"), bloom ? f.bloom_strength : 0.0f);
                gl::Uniform1i(loc(p, "u_lut_on"), grade ? 1 : 0);
                gl::Uniform1f(loc(p, "u_vignette"), f.vignette);
                gl::Uniform1f(loc(p, "u_grain"), f.grain);
                // Wrapped well inside a float's exact integers; the hash only
                // needs a different seed per step.
                gl::Uniform1f(loc(p, "u_frame"), static_cast<float>(f.step % 4096u));
                gl::Uniform2f(loc(p, "u_size"), static_cast<float>(in.target_w),
                              static_cast<float>(in.target_h));
                draw_fullscreen();
                end_timer();
                break;
            }
        }
    }
    gl::ActiveTexture(GL_TEXTURE0);
    return true;
}

// The point lights as resolve.frag and weather.frag both declare them.
void Pipeline::set_point_lights(unsigned p, const lit::LitFrame& f) {
    const int n = static_cast<int>(std::min(f.light_count, lit::MAX_POINT_LIGHTS));
    gl::Uniform1i(loc(p, "u_light_count"), n);
    if (n <= 0) return;
    float pos[4 * lit::MAX_POINT_LIGHTS];
    float col[3 * lit::MAX_POINT_LIGHTS];
    for (int i = 0; i < n; ++i) {
        const lit::PointLight& l = f.lights[i];
        pos[4 * i + 0] = l.x;
        pos[4 * i + 1] = l.y;
        pos[4 * i + 2] = std::max(l.radius, 1e-3f);
        pos[4 * i + 3] = l.flicker;
        col[3 * i + 0] = l.r;
        col[3 * i + 1] = l.g;
        col[3 * i + 2] = l.b;
    }
    gl::Uniform4fv(loc(p, "u_light_pos"), n, pos);
    gl::Uniform3fv(loc(p, "u_light_color"), n, col);
}

// Down the chain from the HDR frame to 1/16, then back up, each level added
// onto the one above, so bloom_[0] holds every width of glow at once. A target
// that cannot be made leaves bloom off for the frame rather than failing it.
void Pipeline::run_bloom(const lit::LitFrame& f) {
    bloom_ran_ = false;
    if (!f.bloom_on || f.bloom_strength <= 0.0f) return;
    std::string why;
    int w = hdr_.w, h = hdr_.h;
    for (RenderTarget& t : bloom_) {
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
        if (!t.ensure(w, h, GL_RGBA16F, true, why)) return;
    }
    // hdr_ is sampled nearest; its four taps at +-1 texel still cover a 4x4 box
    // at half resolution, and the levels below are linear and get the full box.
    gl::UseProgram(prog_.bloom_down);
    gl::Uniform1i(loc(prog_.bloom_down, "u_src"), 0);
    gl::Uniform1f(loc(prog_.bloom_down, "u_threshold"), f.bloom_threshold);
    const RenderTarget* src = &hdr_;
    for (int i = 0; i < BLOOM_LEVELS; ++i) {
        bind_target(bloom_[i]);
        bind_tex(0, src->tex);
        gl::Uniform1i(loc(prog_.bloom_down, "u_first"), i == 0 ? 1 : 0);
        gl::Uniform2f(loc(prog_.bloom_down, "u_src_texel"), 1.0f / static_cast<float>(src->w),
                      1.0f / static_cast<float>(src->h));
        gl::Uniform2f(loc(prog_.bloom_down, "u_dst_size"), static_cast<float>(bloom_[i].w),
                      static_cast<float>(bloom_[i].h));
        draw_fullscreen();
        src = &bloom_[i];
    }
    gl::UseProgram(prog_.bloom_up);
    gl::Uniform1i(loc(prog_.bloom_up, "u_src"), 0);
    gl::Enable(GL_BLEND);
    gl::BlendFunc(GL_ONE, GL_ONE);
    for (int i = BLOOM_LEVELS - 1; i > 0; --i) {
        bind_target(bloom_[i - 1]);
        bind_tex(0, bloom_[i].tex);
        gl::Uniform2f(loc(prog_.bloom_up, "u_src_texel"), 1.0f / static_cast<float>(bloom_[i].w),
                      1.0f / static_cast<float>(bloom_[i].h));
        gl::Uniform2f(loc(prog_.bloom_up, "u_dst_size"), static_cast<float>(bloom_[i - 1].w),
                      static_cast<float>(bloom_[i - 1].h));
        draw_fullscreen();
    }
    gl::Disable(GL_BLEND);
    bloom_ran_ = true;
}

void Pipeline::run_weather(const Inputs& in, const lit::LitFrame& f) {
    if (f.weather <= 0 || f.weather > 3 || f.weather_density <= 0.0f) return;
    // The stop row per column, from the pack.
    bind_target(top_);
    gl::UseProgram(prog_.weather_top);
    bind_tex(0, in.pack_tex);
    gl::Uniform1i(loc(prog_.weather_top, "u_cells"), 0);
    gl::Uniform1f(loc(prog_.weather_top, "u_none"), 1.0e6f);
    draw_fullscreen();

    bind_target(hdr_);
    const GLuint p = prog_.weather;
    gl::UseProgram(p);
    bind_tex(0, top_.tex);
    gl::Uniform1i(loc(p, "u_top"), 0);
    const float scale = static_cast<float>(std::max(f.scale, 1));
    gl::Uniform1i(loc(p, "u_pack_w"), in.pack_w);
    gl::Uniform1f(loc(p, "u_target_h"), static_cast<float>(in.target_h));
    gl::Uniform2f(loc(p, "u_target"), static_cast<float>(in.target_w),
                  static_cast<float>(in.target_h));
    gl::Uniform2f(loc(p, "u_origin_px"), f.screen_x, f.screen_y);
    gl::Uniform1f(loc(p, "u_scale"), scale);
    gl::Uniform2f(loc(p, "u_view"), static_cast<float>(f.origin_x), static_cast<float>(f.origin_y));
    // The camera in pixels, so the weather is in the world and slides past as
    // the view pans. Wrapped to 2^20 pixels -- hundreds of screens of walking --
    // so the float keeps whole pixels; the wrap itself is one jump that far out.
    const float cam_x = std::fmod(static_cast<float>(f.origin_x) * scale - f.screen_x, 1048576.0f);
    const float cam_y = std::fmod(static_cast<float>(f.origin_y) * scale - f.screen_y, 1048576.0f);
    gl::Uniform2f(loc(p, "u_cam"), cam_x, cam_y);
    // Simulation time, never the wall clock, wrapped hourly for the float.
    gl::Uniform1f(loc(p, "u_t"), static_cast<float>(f.step % (60u * 3600u)) / 60.0f);
    gl::Uniform1i(loc(p, "u_kind"), f.weather);
    const float a = f.weather_angle_deg * 3.14159265f / 180.0f;
    gl::Uniform2f(loc(p, "u_dir"), std::sin(a), std::cos(a));
    gl::Uniform1f(loc(p, "u_speed"), f.weather_speed * scale);
    gl::Uniform3f(loc(p, "u_ambient"), f.ambient[0], f.ambient[1], f.ambient[2]);
    gl::Uniform3f(loc(p, "u_sun_color"), f.sun_on ? f.sun[0] : 0.0f, f.sun_on ? f.sun[1] : 0.0f,
                  f.sun_on ? f.sun[2] : 0.0f);
    set_point_lights(p, f);

    // How many per screen cell at full density: rain fills the air, wisps are a
    // few banks across the screen, dust a sprinkle. Per cell rather than per
    // pixel, so a scene at scale 8 and one at 10 rain alike.
    const float cells =
        static_cast<float>(in.target_w) * static_cast<float>(in.target_h) / (scale * scale);
    float per_cell = 0.0f, alpha = 0.0f;
    float tint[3] = {1.0f, 1.0f, 1.0f};
    switch (f.weather) {
        case 1:
            per_cell = 0.03f;
            alpha = 0.45f;
            tint[0] = 0.70f;
            tint[1] = 0.80f;
            break;
        case 2:
            per_cell = 1.0f / 900.0f;
            alpha = 0.10f;
            tint[0] = 0.85f;
            tint[1] = 0.92f;
            break;
        default:
            per_cell = 0.004f;
            alpha = 0.35f;
            tint[2] = 0.75f;
            break;
    }
    gl::Uniform1f(loc(p, "u_alpha"), alpha);
    gl::Uniform3f(loc(p, "u_tint"), tint[0], tint[1], tint[2]);
    const int count = std::clamp(static_cast<int>(cells * per_cell * f.weather_density), 0, 40000);
    gl::Enable(GL_BLEND);
    gl::BlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);  // light added, alpha kept
    gl::DrawArraysInstanced(GL_TRIANGLES, 0, 6, count);
    gl::Disable(GL_BLEND);
}

void Pipeline::set_lut(const unsigned char* rgba, int w, int h) {
    if (lut_) gl::DeleteTextures(1, &lut_);
    lut_ = 0;
    if (!rgba || w <= 0 || h <= 0) return;
    gl::GenTextures(1, &lut_);
    gl::BindTexture(GL_TEXTURE_2D, lut_);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

const char* Pipeline::timer_name(int t) {
    static const char* const NAMES[TIMER_COUNT] = {"canvas",  "light", "resolve",
                                                   "weather", "bloom", "post"};
    return t >= 0 && t < TIMER_COUNT ? NAMES[t] : "?";
}

void Pipeline::begin_timer(int t) {
    end_timer();
    if (!queries_[0][0]) gl::GenQueries(2 * TIMER_COUNT, &queries_[0][0]);
    gl::BeginQuery(GL_TIME_ELAPSED, queries_[set_][t]);
    issued_[set_][t] = true;
    open_timer_ = t;
}

void Pipeline::end_timer() {
    if (open_timer_ < 0) return;
    gl::EndQuery(GL_TIME_ELAPSED);
    open_timer_ = -1;
}

// A new frame: flip to the other half of the queries, reading what the GPU left
// in it two frames ago first (long finished), then start the canvas's.
void Pipeline::begin_canvas_timer() {
    end_timer();
    set_ ^= 1;
    collect(set_, false);
    begin_timer(T_CANVAS);
}

void Pipeline::collect(int set, bool wait) {
    for (int t = 0; t < TIMER_COUNT; ++t) {
        if (!issued_[set][t]) continue;
        GLint ready = 0;
        gl::GetQueryObjectiv(queries_[set][t], GL_QUERY_RESULT_AVAILABLE, &ready);
        if (!ready && !wait) continue;
        GLuint64 ns = 0;
        gl::GetQueryObjectui64v(queries_[set][t], GL_QUERY_RESULT, &ns);
        ms_[t] = static_cast<float>(static_cast<double>(ns) / 1.0e6);
        issued_[set][t] = false;
    }
}

void Pipeline::gpu_ms(float out[TIMER_COUNT], bool wait) {
    if (wait) {
        end_timer();
        collect(set_, true);
    }
    for (int t = 0; t < TIMER_COUNT; ++t) out[t] = ms_[t];
}

void Pipeline::read_blocks(std::vector<float>& rgb, int& cols, int& rows) const {
    const RenderTarget& t = blocks_[front_];
    cols = t.w;
    rows = t.h;
    std::vector<float> rgba(static_cast<size_t>(t.w) * t.h * 4);
    rgb.assign(static_cast<size_t>(t.w) * t.h * 3, 0.0f);
    if (!t.fbo) return;
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
    gl::PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl::ReadPixels(0, 0, t.w, t.h, GL_RGBA, GL_FLOAT, rgba.data());
    // Texel row y is block row y here (the passes never flip), so no reversal.
    for (size_t i = 0; i < static_cast<size_t>(t.w) * t.h; ++i)
        for (size_t k = 0; k < 3; ++k) rgb[3 * i + k] = rgba[4 * i + k];
}

void Pipeline::read_sun(std::vector<float>& vis, int& w, int& h) const {
    w = sun_.w;
    h = sun_.h;
    vis.assign(static_cast<size_t>(w) * h, 0.0f);
    if (!sun_.fbo) return;
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, sun_.fbo);
    gl::PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl::ReadPixels(0, 0, w, h, GL_RED, GL_FLOAT, vis.data());
}

}  // namespace gpu
