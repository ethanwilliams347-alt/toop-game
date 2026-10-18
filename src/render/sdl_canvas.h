#pragma once
#include <SDL.h>
#include "render/canvas.h"

// The SDL_Renderer backend of gfx::Canvas: one SDL call per Canvas call, the same
// calls frame.cpp, overlay.cpp and ui/ made before the seam existed, so a frame
// drawn through it is the frame they drew -- golden_frame_test's checksums are
// unchanged across the refactor that introduced it.
//
// Borrows the renderer; whoever created it destroys it, after every texture this
// canvas made. The game hands it the window's accelerated renderer, the golden
// test and the preview tools a software renderer over a surface.
namespace gfx {

class SdlCanvas final : public Canvas {
public:
    explicit SdlCanvas(SDL_Renderer* renderer) : renderer_(renderer) {}

    void set_draw_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) override;
    void set_blend(Blend mode) override;
    void clear() override;
    void fill_rect(const Rect& r) override;
    void fill_rect(const FRect& r) override;
    void blit(const Texture& tex, const Rect* src, const FRect& dst) override;
    void blit_ex(const Texture& tex, const Rect* src, const FRect& dst, double angle_deg,
                 const FPoint* pivot, Flip flip) override;
    void present() override;

    Texture create_texture(int w, int h, Access access, Blend blend,
                           Filter filter = Filter::Nearest) override;
    void destroy_texture(Texture& tex) override;
    void update_texture(const Texture& tex, const Rect* rect, const void* pixels,
                        int pitch_bytes) override;

    const char* error() const override;

    SDL_Renderer* renderer() const { return renderer_; }

private:
    SDL_Renderer* renderer_;
};

} // namespace gfx
