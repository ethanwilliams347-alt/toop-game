#pragma once
#include <cstdint>

// The drawing seam: every draw call the frame, the overlay and the UI make, as an
// interface a backend implements.
//
// Today there is one backend, gfx::SdlCanvas (render/sdl_canvas.h), a thin wrapper
// over SDL_Renderer that makes exactly the calls this code used to make directly.
// The interface exists so a second one -- OpenGL, with shaders -- can be put
// behind the same frame::compose and overlay::draw without either of them
// changing, and so golden_frame_test, which drives the SDL backend's software
// renderer, keeps checksumming the very code the game runs.
//
// The surface is deliberately the one SDL_Renderer already had, not a new idea of
// drawing: a stateful draw colour and blend mode, filled rects, and textured rects
// with an optional turn and flip. Anything richer (a light buffer, a material
// pass) is a later layer's business, and adding it here before it has a caller is
// how an interface ends up describing a renderer nobody wrote.
//
// No SDL in this header, on purpose. Code that only draws can include it without
// SDL's include path, which is what lets later render code stay SDL-free the way
// light.cpp and backdrop_set.cpp are.
namespace gfx {

// Rectangles and points in screen pixels (texture texels for a source rect).
// Field-for-field the same as SDL_Rect, SDL_FRect and SDL_FPoint, so the SDL
// backend converts them by copying and an aggregate initialiser written for one
// reads the same for the other.
struct Rect {
    int x, y, w, h;
};
struct FRect {
    float x, y, w, h;
};
struct FPoint {
    float x, y;
};

// How a draw combines with what is already there. Alpha is SDL_BLENDMODE_BLEND
// (source-over); Add is SDL_BLENDMODE_ADD (the light pass).
enum class Blend : unsigned char { None, Alpha, Add };

// How a stretched texture is sampled. Nearest for everything that is pixel art,
// which is everything but the downsampled light field.
enum class Filter : unsigned char { Nearest, Linear };

// Whether a texture's pixels are rewritten every frame (cells, light, atlases) or
// written once after creation (backdrop and sprite art). A hint to the backend,
// not a restriction: update_texture works on both.
enum class Access : unsigned char { Static, Streaming };

enum class Flip : unsigned char { None, Horizontal };

// An opaque texture owned by the Canvas that created it. `handle` means something
// only to that backend (an SDL_Texture* for SdlCanvas); w and h are its size in
// texels, kept here so nothing has to ask the backend for them. A
// default-constructed Texture is "no texture", and every draw that would use one
// skips instead -- the same as the null SDL_Texture* the frame used to check for.
//
// Copyable, because the frame records borrow it (several props share one
// texture); only the owner calls destroy_texture.
struct Texture {
    void* handle = nullptr;
    int w = 0;
    int h = 0;
    explicit operator bool() const { return handle != nullptr; }
};

class Canvas {
public:
    Canvas() = default;
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    virtual ~Canvas() = default;

    // --- draw state, as SDL_Renderer keeps it: set once, read by every draw after ---
    virtual void set_draw_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) = 0;
    virtual void set_blend(Blend mode) = 0;

    // --- drawing ---
    // Fills the whole target with the draw colour, ignoring the blend mode.
    virtual void clear() = 0;
    // A rect in the draw colour, combined by the draw blend mode. The int and
    // float forms are both here because the UI draws on whole pixels and the world
    // on sub-pixel positions, and a backend may rasterise the two differently (SDL's
    // software renderer does); collapsing them would move pixels.
    virtual void fill_rect(const Rect& r) = 0;
    virtual void fill_rect(const FRect& r) = 0;
    // `src` null is the whole texture. Combined by the texture's own blend mode
    // (fixed at creation), not the draw blend mode -- the SDL rule.
    virtual void blit(const Texture& tex, const Rect* src, const FRect& dst) = 0;
    // As blit, turned clockwise by `angle_deg` about `pivot` (relative to dst's
    // top-left; null is its centre), then flipped.
    virtual void blit_ex(const Texture& tex, const Rect* src, const FRect& dst, double angle_deg,
                         const FPoint* pivot, Flip flip) = 0;
    // The world's cell texture over `dst` -- frame::draw_cells' one call. A plain
    // blit by default, which is all SdlCanvas does, so the SDL path (and with it
    // golden_frame_test) is unchanged by its existence. gpu::GlCanvas overrides it
    // to draw the cells with assets/shaders/cells.frag, which reads the material
    // pack (render/cell_pack.h) the shell uploaded beside the colour; in its legacy
    // mode that shader draws exactly what this default does. A method of its own
    // rather than a flag on blit because the cells are the one draw a shader looks
    // through to what is there, and every later material pass hangs off it.
    virtual void blit_cells(const Texture& colour, const FRect& dst) { blit(colour, nullptr, dst); }
    // Shows the finished frame.
    virtual void present() = 0;

    // --- textures ---
    // ARGB8888 (0xAARRGGBB in a uint32_t), contents undefined until updated.
    // Returns a null Texture on failure, with the reason in error().
    virtual Texture create_texture(int w, int h, Access access, Blend blend,
                                   Filter filter = Filter::Nearest) = 0;
    // Releases the texture and nulls the handle. A null Texture is a no-op.
    virtual void destroy_texture(Texture& tex) = 0;
    // Writes ARGB8888 pixels into `rect` (null: the whole texture). `pitch_bytes`
    // is the source's row stride, which lets a caller upload a window straight out
    // of a larger buffer -- the grid's own pixels -- with no copy.
    virtual void update_texture(const Texture& tex, const Rect* rect, const void* pixels,
                                int pitch_bytes) = 0;

    // Why the last call that can fail did. Valid until the next canvas call.
    virtual const char* error() const = 0;
};

} // namespace gfx
