#pragma once
#include <SDL.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "render/canvas.h"

// The OpenGL 3.3 core backend of gfx::Canvas.
//
// At this stage it is a parity backend: it draws what gfx::SdlCanvas draws, with
// the same blend equations and the same rect snapping, so switching renderer=gl
// changes nothing on screen. That is the point of doing it first -- the shader
// passes that come after it (cells, light, post) each move pixels on purpose,
// and they need a GL frame already known to equal the SDL one to be measured
// against. gl_frame_test holds the two within a few levels of each other.
//
// How it draws: every fill and blit is a quad appended to one vertex batch,
// flushed when the texture or blend mode changes, before an upload, and at
// present. A fill is a quad over a 1x1 white texture, so one shader
// (assets/shaders/sprite.*) does both. The frame goes into an offscreen
// framebuffer the window's size, and present() blits that to the window and
// swaps. Drawing offscreen is what lets read_pixels work on a hidden window --
// the default framebuffer of one that is not shown has no pixels to read on some
// drivers -- and it is the target later passes will read from.
//
// Rect snapping copies SDL's software renderer, which golden_frame_test runs:
// every destination rect is truncated toward zero field by field (x, y, w and h
// separately, as SDL_render_sw.c does), and a fill is at least one pixel each
// way. A GPU left to itself rasterises a float rect by pixel centres instead,
// which disagrees by a pixel along about half of all edges.
//
// One context, one window: it is created on a window that has SDL_WINDOW_OPENGL,
// owns its context, and must outlive every texture it made.
namespace gpu {

class GlCanvas final : public gfx::Canvas {
public:
    // Call before SDL_CreateWindow for any window this will be made on. On Windows
    // the pixel format is chosen when the window is made, so asking for a 3.3 core
    // context afterwards is too late for some of it.
    static void set_window_attributes();

    // Makes a context on `window` (which needs SDL_WINDOW_OPENGL), loads the GL
    // entry points, compiles the shaders from `shader_dir`, and builds the
    // offscreen target. Null, with the reason in `why`, if any of it fails -- the
    // caller falls back to the SDL renderer. `vsync` asks for a swap interval of 1.
    static std::unique_ptr<GlCanvas> create(SDL_Window* window, const char* shader_dir, bool vsync,
                                            std::string& why);

    ~GlCanvas() override;

    void set_draw_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
    void set_blend(gfx::Blend mode) override;
    void clear() override;
    void fill_rect(const gfx::Rect& r) override;
    void fill_rect(const gfx::FRect& r) override;
    void blit(const gfx::Texture& tex, const gfx::Rect* src, const gfx::FRect& dst) override;
    void blit_ex(const gfx::Texture& tex, const gfx::Rect* src, const gfx::FRect& dst,
                 double angle_deg, const gfx::FPoint* pivot, gfx::Flip flip) override;
    void present() override;

    // --- the cells (R1) ---
    //
    // What cells.frag draws (its u_mode). Legacy is the default and the only one a
    // player sees: the colour texture, exactly as a plain blit would draw it --
    // gl_frame_test holds it to that. The others are debug views; see cells.frag.
    enum class CellView : int { Legacy = 0, Material = 1, Raw = 2 };
    void set_cell_view(CellView v) { cell_view_ = v; }
    CellView cell_view() const { return cell_view_; }

    // Draws the cell texture through cells.frag, with the last upload_cell_pack
    // bound beside it. Same rect snapping and blend as blit (the Raw view draws
    // unblended, so its bytes arrive as they are).
    void blit_cells(const gfx::Texture& colour, const gfx::FRect& dst) override;

    // Uploads the material pack (cell_pack::CellPack::texels(), w x h, row-major,
    // the window the colour texture's top-left w x h covers) into the RGBA8UI
    // texture cells.frag reads. Takes the bare buffer so slop_gpu does not depend
    // on slop_render. Reallocates only when the size changes; w or h <= 0 leaves a
    // 1x1 pad texel, which the shader reads as an Empty cell everywhere.
    void upload_cell_pack(const uint32_t* texels, int w, int h);

    gfx::Texture create_texture(int w, int h, gfx::Access access, gfx::Blend blend,
                                gfx::Filter filter = gfx::Filter::Nearest) override;
    void destroy_texture(gfx::Texture& tex) override;
    void update_texture(const gfx::Texture& tex, const gfx::Rect* rect, const void* pixels,
                        int pitch_bytes) override;

    const char* error() const override { return error_.c_str(); }

    // Recompiles the shaders from disk (F5 in a debug session). On a compile or
    // link failure the running program is kept and the log is in error(); returns
    // whether the new program took.
    bool reload_shaders();

    // The frame drawn so far, top row first, as ARGB8888 -- the layout of an SDL
    // surface, so a test can compare it with one byte for byte. Flushes first.
    // Sized target_w() x target_h().
    void read_pixels(std::vector<uint32_t>& out);
    int target_w() const { return target_w_; }
    int target_h() const { return target_h_; }

    // What the driver says it is, for the startup line.
    std::string describe() const;

private:
    struct Vertex {
        float x, y, u, v;
        uint8_t r, g, b, a;
    };

    GlCanvas(SDL_Window* window, SDL_GLContext context, std::string shader_dir)
        : window_(window), context_(context), shader_dir_(std::move(shader_dir)) {}

    bool init(std::string& why);
    bool build_program(const char* vert, const char* frag, unsigned& out, std::string& log) const;
    bool build_programs(unsigned& sprite, unsigned& cells, std::string& log) const;
    void bind_cell_uniforms();
    bool resize_target(int w, int h);
    void begin_frame();
    void flush();
    // Queues one quad, flushing first if it needs a different texture or blend.
    // `corners` are the four screen positions top-left, top-right, bottom-right,
    // bottom-left; u0..v1 the texture coordinates of the top-left and
    // bottom-right corners.
    void quad(unsigned texture, gfx::Blend blend, const float corners[8], float u0, float v0,
              float u1, float v1, const uint8_t rgba[4]);
    void textured(const gfx::Texture& tex, const gfx::Rect* src, const gfx::FRect& dst,
                  double angle_deg, const gfx::FPoint* pivot, gfx::Flip flip);

    SDL_Window* window_;
    SDL_GLContext context_;
    std::string shader_dir_;
    std::string error_;

    unsigned program_ = 0;
    int u_scale_ = -1;
    unsigned cells_program_ = 0;
    int cu_scale_ = -1, cu_mode_ = -1;
    unsigned cell_tex_ = 0;  // RGBA8UI, the material pack
    int cell_tex_w_ = 0, cell_tex_h_ = 0;
    CellView cell_view_ = CellView::Legacy;
    unsigned vao_ = 0, vbo_ = 0;
    unsigned white_ = 0;     // 1x1 opaque white, the "texture" of every fill
    unsigned fbo_ = 0, fbo_tex_ = 0;
    int target_w_ = 0, target_h_ = 0;
    bool frame_open_ = false;

    uint8_t color_[4] = {0, 0, 0, 255};
    gfx::Blend draw_blend_ = gfx::Blend::None;

    // The batch. Reused across frames; it only grows.
    std::vector<Vertex> verts_;
    unsigned batch_tex_ = 0;
    gfx::Blend batch_blend_ = gfx::Blend::None;
    bool blend_known_ = false;  // whether GL's blend state is batch_blend_ yet
    gfx::Blend gl_blend_ = gfx::Blend::None;
};

} // namespace gpu
