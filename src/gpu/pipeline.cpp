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
    for (unsigned* id : {&p.gather, &p.propagate, &p.smooth, &p.sun, &p.resolve, &p.post}) {
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
        build_program(dir, "fullscreen.vert", "post.frag", p.post, log);
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
        !hdr_.ensure(in.target_w, in.target_h, GL_RGBA16F, false, why)) {
        gl::BindFramebuffer(GL_FRAMEBUFFER, in.frame_fbo);
        return false;
    }

    gl::Disable(GL_BLEND);
    gl::BindVertexArray(vao_);

    // Walked in pass_graph's order, so the table the asserts check is the order
    // that runs. The canvas passes (backdrop, world, ui) are the frame code's.
    for (const pass_graph::Pass& pass : pass_graph::PASSES) {
        switch (pass.id) {
            case pass_graph::Id::Backdrop:
            case pass_graph::Id::World:
            case pass_graph::Id::Ui:       break;

            case pass_graph::Id::Light: {
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
                break;
            }

            case pass_graph::Id::Resolve: {
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
                const int n = static_cast<int>(std::min(f.light_count, lit::MAX_POINT_LIGHTS));
                gl::Uniform1i(loc(p, "u_light_count"), n);
                if (n > 0) {
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
                draw_fullscreen();
                break;
            }

            case pass_graph::Id::Weather: break;  // R6

            case pass_graph::Id::Post: {
                gl::BindFramebuffer(GL_FRAMEBUFFER, in.frame_fbo);
                const GLenum one[1] = {GL_COLOR_ATTACHMENT0};
                gl::DrawBuffers(1, one);
                gl::Viewport(0, 0, in.target_w, in.target_h);
                gl::UseProgram(prog_.post);
                bind_tex(0, hdr_.tex);
                gl::Uniform1i(loc(prog_.post, "u_hdr"), 0);
                draw_fullscreen();
                break;
            }
        }
    }
    gl::ActiveTexture(GL_TEXTURE0);
    return true;
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
