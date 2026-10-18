#include "gpu/gl_canvas.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "gpu/gl.h"

namespace gpu {
namespace {

// What a gfx::Texture's handle points at in this backend. The blend mode lives
// with the texture because the Canvas contract says a blit uses the texture's
// own mode, fixed at creation (the SDL rule).
struct GlTexture {
    GLuint id = 0;
    gfx::Blend blend = gfx::Blend::Alpha;
};

// glVertexAttribPointer takes a byte offset into the bound buffer disguised as a
// pointer -- GL's API from before buffers existed. The int-to-pointer cast is the
// API's, not a pointer anyone dereferences.
const void* buffer_offset(size_t bytes) {
    // NOLINTNEXTLINE(*-reinterpret-cast,performance-no-int-to-ptr)
    return reinterpret_cast<const void*>(bytes);
}

GlTexture* tex_of(const gfx::Texture& t) { return static_cast<GlTexture*>(t.handle); }

// SDL's three blend modes as GL blend functions, alpha channel included, because
// the golden frame's surface keeps alpha and so does the offscreen target here.
//   Alpha: rgb = src*sa + dst*(1-sa),  a = sa + da*(1-sa)   (SDL_BLENDMODE_BLEND)
//   Add:   rgb = src*sa + dst,         a = da               (SDL_BLENDMODE_ADD)
//   None:  overwrite                                        (SDL_BLENDMODE_NONE)
void apply_blend(gfx::Blend mode) {
    switch (mode) {
        case gfx::Blend::Alpha:
            gl::Enable(GL_BLEND);
            gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE,
                                  GL_ONE_MINUS_SRC_ALPHA);
            return;
        case gfx::Blend::Add:
            gl::Enable(GL_BLEND);
            gl::BlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
            return;
        case gfx::Blend::None: break;
    }
    gl::Disable(GL_BLEND);
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

// Compiles one stage. On failure the shader is deleted and the log says why.
GLuint compile(GLenum type, const std::string& source, const std::string& name, std::string& log) {
    const GLuint s = gl::CreateShader(type);
    const GLchar* text = source.c_str();
    gl::ShaderSource(s, 1, &text, nullptr);
    gl::CompileShader(s);
    GLint ok = 0;
    gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048] = {};
        gl::GetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        log = name + ": " + buf;
        gl::DeleteShader(s);
        return 0;
    }
    return s;
}

// SDL's software renderer truncates each field of a float destination rect on
// its own (SDL_render_sw.c, SW_QueueCopy). Not floor: a rect at x = -2.5 starts
// at -2, which is what the golden frame was drawn with.
gfx::Rect snap(const gfx::FRect& r) {
    return gfx::Rect{static_cast<int>(r.x), static_cast<int>(r.y), static_cast<int>(r.w),
                     static_cast<int>(r.h)};
}

} // namespace

void GlCanvas::set_window_attributes() {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    // Nothing is depth-tested or stencilled; the frame is painter's order.
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

std::unique_ptr<GlCanvas> GlCanvas::create(SDL_Window* window, const char* shader_dir, bool vsync,
                                           std::string& why) {
    SDL_GLContext ctx = SDL_GL_CreateContext(window);
    if (!ctx) {
        why = std::string("no GL 3.3 core context: ") + SDL_GetError();
        return nullptr;
    }
    std::unique_ptr<GlCanvas> c(new GlCanvas(window, ctx, shader_dir));
    if (!c->init(why)) return nullptr;  // the destructor releases what init made
    // A refused swap interval is not a failure: the frame still draws, only
    // unpaced, which is what the SDL renderer does on a driver without vsync too.
    if (vsync) SDL_GL_SetSwapInterval(1);
    return c;
}

bool GlCanvas::init(std::string& why) {
    if (SDL_GL_MakeCurrent(window_, context_) != 0) {
        why = std::string("could not make the GL context current: ") + SDL_GetError();
        return false;
    }
    std::string missing;
    if (!gl::load(missing)) {
        why = "the driver lacks " + missing;
        return false;
    }

    std::string log;
    if (!build_programs(program_, cells_program_, log)) {
        why = log;
        return false;
    }
    bind_cell_uniforms();

    gl::GenVertexArrays(1, &vao_);
    gl::BindVertexArray(vao_);
    gl::GenBuffers(1, &vbo_);
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    // Locations fixed by layout() in sprite.vert.
    gl::EnableVertexAttribArray(0);
    gl::VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                            buffer_offset(offsetof(Vertex, x)));
    gl::EnableVertexAttribArray(1);
    gl::VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                            buffer_offset(offsetof(Vertex, u)));
    gl::EnableVertexAttribArray(2);
    gl::VertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Vertex),
                            buffer_offset(offsetof(Vertex, r)));

    gl::GenTextures(1, &white_);
    gl::BindTexture(GL_TEXTURE_2D, white_);
    const uint32_t white = 0xFFFFFFFFu;
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                   &white);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    // The material pack's texture, a single pad texel until the first upload.
    // Integer textures cannot be filtered: anything but NEAREST leaves one
    // incomplete, and an incomplete texture samples as zero.
    gl::GenTextures(1, &cell_tex_);
    upload_cell_pack(nullptr, 0, 0);

    gl::ActiveTexture(GL_TEXTURE0);
    verts_.reserve(6 * 4096);

    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    if (!resize_target(w, h)) {
        why = error_;
        return false;
    }
    if (gl::GetError() != GL_NO_ERROR) {
        why = "GL reported an error while setting up";
        return false;
    }
    return true;
}

GlCanvas::~GlCanvas() {
    if (context_) {
        SDL_GL_MakeCurrent(window_, context_);
        // gl::DeleteProgram is null if the loader never ran; everything below was
        // only made after it did.
        if (gl::DeleteProgram) {
            if (program_) gl::DeleteProgram(program_);
            if (cells_program_) gl::DeleteProgram(cells_program_);
            if (cell_tex_) gl::DeleteTextures(1, &cell_tex_);
            if (vbo_) gl::DeleteBuffers(1, &vbo_);
            if (vao_) gl::DeleteVertexArrays(1, &vao_);
            if (white_) gl::DeleteTextures(1, &white_);
            if (fbo_) gl::DeleteFramebuffers(1, &fbo_);
            if (fbo_tex_) gl::DeleteTextures(1, &fbo_tex_);
        }
        SDL_GL_DeleteContext(context_);
    }
}

bool GlCanvas::build_program(const char* vert, const char* frag, unsigned& out,
                             std::string& log) const {
    const std::string vpath = shader_dir_ + "/" + vert;
    const std::string fpath = shader_dir_ + "/" + frag;
    std::string vsrc, fsrc;
    if (!read_file(vpath, vsrc)) {
        log = "cannot read " + vpath;
        return false;
    }
    if (!read_file(fpath, fsrc)) {
        log = "cannot read " + fpath;
        return false;
    }
    const GLuint vs = compile(GL_VERTEX_SHADER, vsrc, vpath, log);
    if (!vs) return false;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc, fpath, log);
    if (!fs) {
        gl::DeleteShader(vs);
        return false;
    }
    const GLuint p = gl::CreateProgram();
    gl::AttachShader(p, vs);
    gl::AttachShader(p, fs);
    gl::LinkProgram(p);
    gl::DeleteShader(vs);  // flagged; freed with the program
    gl::DeleteShader(fs);
    GLint ok = 0;
    gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[2048] = {};
        gl::GetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        log = fpath + " program: " + buf;
        gl::DeleteProgram(p);
        return false;
    }
    out = p;
    return true;
}

// Both or neither: the sprite program and the cells program share sprite.vert,
// so a half-taken reload could leave the two drawing from different versions of
// it.
bool GlCanvas::build_programs(unsigned& sprite, unsigned& cells, std::string& log) const {
    if (!build_program("sprite.vert", "sprite.frag", sprite, log)) return false;
    if (!build_program("sprite.vert", "cells.frag", cells, log)) {
        gl::DeleteProgram(sprite);
        sprite = 0;
        return false;
    }
    return true;
}

// The cells program's sampler units are fixed (colour on 0, pack on 1), so they
// are set once per program rather than per draw.
void GlCanvas::bind_cell_uniforms() {
    gl::UseProgram(cells_program_);
    gl::Uniform1i(gl::GetUniformLocation(cells_program_, "u_tex"), 0);
    gl::Uniform1i(gl::GetUniformLocation(cells_program_, "u_cells"), 1);
    cu_scale_ = gl::GetUniformLocation(cells_program_, "u_scale");
    cu_mode_ = gl::GetUniformLocation(cells_program_, "u_mode");
    gl::UseProgram(program_);
}

bool GlCanvas::reload_shaders() {
    flush();
    unsigned sprite = 0, cells = 0;
    if (!build_programs(sprite, cells, error_)) return false;
    gl::DeleteProgram(program_);
    gl::DeleteProgram(cells_program_);
    program_ = sprite;
    cells_program_ = cells;
    u_scale_ = -1;  // looked up again against the new program
    bind_cell_uniforms();
    // A reload between frames is the usual case; one mid-frame (a test) needs the
    // new program's scale now, which begin_frame only sets when it opens a frame.
    if (frame_open_) {
        u_scale_ = gl::GetUniformLocation(program_, "u_scale");
        gl::Uniform1i(gl::GetUniformLocation(program_, "u_tex"), 0);
        gl::Uniform2f(u_scale_, 2.0f / static_cast<float>(target_w_),
                      2.0f / static_cast<float>(target_h_));
    }
    return true;
}

bool GlCanvas::resize_target(int w, int h) {
    if (w <= 0 || h <= 0) {
        error_ = "the window has no area";
        return false;
    }
    GLuint tex = 0, fbo = 0;
    gl::GenTextures(1, &tex);
    gl::BindTexture(GL_TEXTURE_2D, tex);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                   nullptr);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl::GenFramebuffers(1, &fbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
        gl::DeleteFramebuffers(1, &fbo);
        gl::DeleteTextures(1, &tex);
        error_ = "the offscreen framebuffer is incomplete";
        return false;
    }
    if (fbo_) gl::DeleteFramebuffers(1, &fbo_);
    if (fbo_tex_) gl::DeleteTextures(1, &fbo_tex_);
    fbo_ = fbo;
    fbo_tex_ = tex;
    target_w_ = w;
    target_h_ = h;
    gl::Viewport(0, 0, w, h);
    return true;
}

// The first draw of a frame. The window's size is read here rather than pushed
// in, so shell::apply_mode resizing the window is all a mode change needs: the
// next frame finds the new size and rebuilds the target.
void GlCanvas::begin_frame() {
    if (frame_open_) return;
    frame_open_ = true;
    int w = 0, h = 0;
    SDL_GetWindowSize(window_, &w, &h);
    // A failed resize keeps the old target; the frame draws clipped or with a
    // margin, which is a bad frame rather than a crash.
    if (w != target_w_ || h != target_h_) resize_target(w, h);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo_);
    gl::Viewport(0, 0, target_w_, target_h_);
    gl::BindVertexArray(vao_);
    gl::UseProgram(program_);
    if (u_scale_ < 0) {
        u_scale_ = gl::GetUniformLocation(program_, "u_scale");
        gl::Uniform1i(gl::GetUniformLocation(program_, "u_tex"), 0);
    }
    gl::Uniform2f(u_scale_, 2.0f / static_cast<float>(target_w_),
                  2.0f / static_cast<float>(target_h_));
    blend_known_ = false;
}

void GlCanvas::flush() {
    if (verts_.empty()) return;
    gl::BindBuffer(GL_ARRAY_BUFFER, vbo_);
    // Orphan and refill: a fresh store each flush, so the driver never waits on
    // the previous draw still reading the old one.
    gl::BufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts_.size() * sizeof(Vertex)),
                   verts_.data(), GL_STREAM_DRAW);
    if (!blend_known_ || gl_blend_ != batch_blend_) {
        apply_blend(batch_blend_);
        gl_blend_ = batch_blend_;
        blend_known_ = true;
    }
    gl::BindTexture(GL_TEXTURE_2D, batch_tex_);
    gl::DrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size()));
    verts_.clear();
}

void GlCanvas::quad(unsigned texture, gfx::Blend blend, const float c[8], float u0, float v0,
                    float u1, float v1, const uint8_t rgba[4]) {
    begin_frame();
    if (!verts_.empty() && (texture != batch_tex_ || blend != batch_blend_)) flush();
    batch_tex_ = texture;
    batch_blend_ = blend;
    const Vertex tl{c[0], c[1], u0, v0, rgba[0], rgba[1], rgba[2], rgba[3]};
    const Vertex tr{c[2], c[3], u1, v0, rgba[0], rgba[1], rgba[2], rgba[3]};
    const Vertex br{c[4], c[5], u1, v1, rgba[0], rgba[1], rgba[2], rgba[3]};
    const Vertex bl{c[6], c[7], u0, v1, rgba[0], rgba[1], rgba[2], rgba[3]};
    verts_.push_back(tl);
    verts_.push_back(tr);
    verts_.push_back(br);
    verts_.push_back(tl);
    verts_.push_back(br);
    verts_.push_back(bl);
}

void GlCanvas::set_draw_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    color_[0] = r;
    color_[1] = g;
    color_[2] = b;
    color_[3] = a;
}

void GlCanvas::set_blend(gfx::Blend mode) { draw_blend_ = mode; }

void GlCanvas::clear() {
    begin_frame();
    flush();
    gl::ClearColor(color_[0] / 255.0f, color_[1] / 255.0f, color_[2] / 255.0f, color_[3] / 255.0f);
    gl::Clear(GL_COLOR_BUFFER_BIT);
}

void GlCanvas::fill_rect(const gfx::Rect& r) {
    // SDL_RenderFillRect goes through the float path, so an int rect gets the
    // float path's one-pixel minimum too.
    fill_rect(gfx::FRect{static_cast<float>(r.x), static_cast<float>(r.y), static_cast<float>(r.w),
                         static_cast<float>(r.h)});
}

void GlCanvas::fill_rect(const gfx::FRect& f) {
    gfx::Rect r = snap(f);
    // SW_QueueFillRects: a fill is never thinner than one pixel.
    r.w = std::max(r.w, 1);
    r.h = std::max(r.h, 1);
    const float x0 = static_cast<float>(r.x), y0 = static_cast<float>(r.y);
    const float x1 = static_cast<float>(r.x + r.w), y1 = static_cast<float>(r.y + r.h);
    const float c[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
    quad(white_, draw_blend_, c, 0.0f, 0.0f, 1.0f, 1.0f, color_);
}

void GlCanvas::blit(const gfx::Texture& tex, const gfx::Rect* src, const gfx::FRect& dst) {
    textured(tex, src, dst, 0.0, nullptr, gfx::Flip::None);
}

void GlCanvas::blit_ex(const gfx::Texture& tex, const gfx::Rect* src, const gfx::FRect& dst,
                       double angle_deg, const gfx::FPoint* pivot, gfx::Flip flip) {
    textured(tex, src, dst, angle_deg, pivot, flip);
}

void GlCanvas::textured(const gfx::Texture& tex, const gfx::Rect* src, const gfx::FRect& dst,
                        double angle_deg, const gfx::FPoint* pivot, gfx::Flip flip) {
    const GlTexture* t = tex_of(tex);
    if (!t) return;
    // SDL_RenderCopy clips the source to the texture and skips an empty one; the
    // destination is left as asked (so a clipped source stretches).
    int sx0 = 0, sy0 = 0, sx1 = tex.w, sy1 = tex.h;
    if (src) {
        sx0 = std::max(src->x, 0);
        sy0 = std::max(src->y, 0);
        sx1 = std::min(src->x + src->w, tex.w);
        sy1 = std::min(src->y + src->h, tex.h);
        if (sx1 <= sx0 || sy1 <= sy0) return;
    }
    const gfx::Rect d = snap(dst);
    if (d.w <= 0 || d.h <= 0) return;

    float u0 = static_cast<float>(sx0) / static_cast<float>(tex.w);
    float u1 = static_cast<float>(sx1) / static_cast<float>(tex.w);
    const float v0 = static_cast<float>(sy0) / static_cast<float>(tex.h);
    const float v1 = static_cast<float>(sy1) / static_cast<float>(tex.h);
    if (flip == gfx::Flip::Horizontal) std::swap(u0, u1);

    const float x0 = static_cast<float>(d.x), y0 = static_cast<float>(d.y);
    const float w = static_cast<float>(d.w), h = static_cast<float>(d.h);
    float c[8] = {x0, y0, x0 + w, y0, x0 + w, y0 + h, x0, y0 + h};
    if (angle_deg != 0.0) {
        // Clockwise on screen, about the pivot relative to the snapped rect's
        // top-left -- with y down, the ordinary rotation matrix turns clockwise.
        const float px = x0 + (pivot ? pivot->x : w * 0.5f);
        const float py = y0 + (pivot ? pivot->y : h * 0.5f);
        const double rad = angle_deg * 3.14159265358979323846 / 180.0;
        const float cs = static_cast<float>(std::cos(rad)), sn = static_cast<float>(std::sin(rad));
        for (int i = 0; i < 4; ++i) {
            const float dx = c[2 * i] - px, dy = c[2 * i + 1] - py;
            c[2 * i] = px + dx * cs - dy * sn;
            c[2 * i + 1] = py + dx * sn + dy * cs;
        }
    }
    // Textures are not tinted by the draw colour (no colour mod in gfx::Canvas).
    static constexpr uint8_t OPAQUE_WHITE[4] = {255, 255, 255, 255};
    quad(t->id, t->blend, c, u0, v0, u1, v1, OPAQUE_WHITE);
}

void GlCanvas::blit_cells(const gfx::Texture& colour, const gfx::FRect& dst) {
    const GlTexture* t = tex_of(colour);
    if (!t) return;
    const gfx::Rect d = snap(dst);
    if (d.w <= 0 || d.h <= 0) return;
    begin_frame();
    flush();  // whatever is batched was drawn with the sprite program

    gl::UseProgram(cells_program_);
    gl::Uniform2f(cu_scale_, 2.0f / static_cast<float>(target_w_),
                  2.0f / static_cast<float>(target_h_));
    gl::Uniform1i(cu_mode_, static_cast<int>(cell_view_));
    gl::ActiveTexture(GL_TEXTURE1);
    gl::BindTexture(GL_TEXTURE_2D, cell_tex_);
    gl::ActiveTexture(GL_TEXTURE0);

    // One quad, drawn now through the ordinary batch so the blend and the colour
    // texture are bound the way every blit binds them.
    const float x0 = static_cast<float>(d.x), y0 = static_cast<float>(d.y);
    const float x1 = static_cast<float>(d.x + d.w), y1 = static_cast<float>(d.y + d.h);
    const float c[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
    static constexpr uint8_t OPAQUE_WHITE[4] = {255, 255, 255, 255};
    const gfx::Blend blend = cell_view_ == CellView::Raw ? gfx::Blend::None : t->blend;
    quad(t->id, blend, c, 0.0f, 0.0f, 1.0f, 1.0f, OPAQUE_WHITE);
    flush();
    gl::UseProgram(program_);
}

void GlCanvas::upload_cell_pack(const uint32_t* texels, int w, int h) {
    static constexpr uint32_t PAD = 20u << 16;  // cell_pack::PAD_TEXEL: Empty, 20 degrees
    if (!texels || w <= 0 || h <= 0) {
        texels = &PAD;
        w = h = 1;
    }
    flush();  // the batch may be about to draw with the old pack
    gl::BindTexture(GL_TEXTURE_2D, cell_tex_);
    gl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (w != cell_tex_w_ || h != cell_tex_h_) {
        // Reallocated only on a size change -- a mode change or a scene whose grid
        // is narrower than the view -- so a steady frame is one sub-image upload.
        gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8UI, w, h, 0, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE,
                       texels);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        cell_tex_w_ = w;
        cell_tex_h_ = h;
    } else {
        gl::TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, texels);
    }
}

void GlCanvas::present() {
    begin_frame();
    flush();
    int dw = 0, dh = 0;
    // The drawable can be larger than the window under DPI scaling; the frame is
    // drawn at window size and stretched to it, nearest, as SDL's renderer does.
    SDL_GL_GetDrawableSize(window_, &dw, &dh);
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    gl::BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    gl::Viewport(0, 0, dw, dh);
    gl::BlitFramebuffer(0, 0, target_w_, target_h_, 0, 0, dw, dh, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    SDL_GL_SwapWindow(window_);
    frame_open_ = false;
}

void GlCanvas::read_pixels(std::vector<uint32_t>& out) {
    begin_frame();
    flush();
    out.resize(static_cast<size_t>(target_w_) * target_h_);
    gl::BindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    gl::PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl::ReadPixels(0, 0, target_w_, target_h_, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, out.data());
    // GL's rows run bottom-up; a surface's run top-down.
    for (int y = 0; y < target_h_ / 2; ++y) {
        uint32_t* a = out.data() + static_cast<size_t>(y) * target_w_;
        uint32_t* b = out.data() + static_cast<size_t>(target_h_ - 1 - y) * target_w_;
        std::swap_ranges(a, a + target_w_, b);
    }
}

gfx::Texture GlCanvas::create_texture(int w, int h, gfx::Access access, gfx::Blend blend,
                                      gfx::Filter filter) {
    (void)access;  // GL has no static/streaming split for a plain texture
    if (w <= 0 || h <= 0) {
        error_ = "texture has no area";
        return gfx::Texture{};
    }
    flush();
    auto* t = new GlTexture;
    t->blend = blend;
    gl::GenTextures(1, &t->id);
    gl::BindTexture(GL_TEXTURE_2D, t->id);
    gl::TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                   nullptr);
    const GLint f = filter == gfx::Filter::Linear ? GL_LINEAR : GL_NEAREST;
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (gl::GetError() != GL_NO_ERROR) {
        gl::DeleteTextures(1, &t->id);
        delete t;
        error_ = "glTexImage2D failed";
        return gfx::Texture{};
    }
    return gfx::Texture{t, w, h};
}

void GlCanvas::destroy_texture(gfx::Texture& tex) {
    if (GlTexture* t = tex_of(tex)) {
        flush();  // the batch may still name it
        gl::DeleteTextures(1, &t->id);
        delete t;
    }
    tex = gfx::Texture{};
}

void GlCanvas::update_texture(const gfx::Texture& tex, const gfx::Rect* rect, const void* pixels,
                              int pitch_bytes) {
    const GlTexture* t = tex_of(tex);
    if (!t) return;
    // The batch may have drawn this texture already this frame, with the old
    // pixels -- SDL's renderer flushes for the same reason.
    flush();
    const gfx::Rect r = rect ? *rect : gfx::Rect{0, 0, tex.w, tex.h};
    gl::BindTexture(GL_TEXTURE_2D, t->id);
    // Row length in pixels: uploads a window out of a wider buffer (the grid's
    // own pixels) with no copy, the same as SDL_UpdateTexture's pitch.
    gl::PixelStorei(GL_UNPACK_ROW_LENGTH, pitch_bytes / static_cast<int>(sizeof(uint32_t)));
    gl::PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl::TexSubImage2D(GL_TEXTURE_2D, 0, r.x, r.y, r.w, r.h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                      pixels);
    gl::PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

std::string GlCanvas::describe() const {
    const auto* ver =
        reinterpret_cast<const char*>(gl::GetString(GL_VERSION));  // NOLINT(*-reinterpret-cast)
    const auto* who =
        reinterpret_cast<const char*>(gl::GetString(GL_RENDERER));  // NOLINT(*-reinterpret-cast)
    return std::string(who ? who : "?") + ", GL " + (ver ? ver : "?");
}

} // namespace gpu
