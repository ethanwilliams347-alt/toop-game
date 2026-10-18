#include "render/sdl_canvas.h"

namespace gfx {
namespace {

// gfx's records are laid out as SDL's, field for field; these hold that, so the
// conversions below are copies of the same four numbers and nothing else.
static_assert(sizeof(Rect) == sizeof(SDL_Rect) && sizeof(FRect) == sizeof(SDL_FRect) &&
                  sizeof(FPoint) == sizeof(SDL_FPoint),
              "gfx rects must match SDL's, or the SDL backend is converting more than a type");

SDL_Rect to_sdl(const Rect& r) { return SDL_Rect{r.x, r.y, r.w, r.h}; }
SDL_FRect to_sdl(const FRect& r) { return SDL_FRect{r.x, r.y, r.w, r.h}; }

SDL_BlendMode to_sdl(Blend mode) {
    switch (mode) {
        case Blend::Alpha: return SDL_BLENDMODE_BLEND;
        case Blend::Add:   return SDL_BLENDMODE_ADD;
        case Blend::None:  break;
    }
    return SDL_BLENDMODE_NONE;
}

SDL_Texture* to_sdl(const Texture& tex) { return static_cast<SDL_Texture*>(tex.handle); }

} // namespace

void SdlCanvas::set_draw_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    SDL_SetRenderDrawColor(renderer_, r, g, b, a);
}

void SdlCanvas::set_blend(Blend mode) { SDL_SetRenderDrawBlendMode(renderer_, to_sdl(mode)); }

void SdlCanvas::clear() { SDL_RenderClear(renderer_); }

void SdlCanvas::fill_rect(const Rect& r) {
    const SDL_Rect s = to_sdl(r);
    SDL_RenderFillRect(renderer_, &s);
}

void SdlCanvas::fill_rect(const FRect& r) {
    const SDL_FRect s = to_sdl(r);
    SDL_RenderFillRectF(renderer_, &s);
}

void SdlCanvas::blit(const Texture& tex, const Rect* src, const FRect& dst) {
    const SDL_Rect s = src ? to_sdl(*src) : SDL_Rect{};
    const SDL_FRect d = to_sdl(dst);
    SDL_RenderCopyF(renderer_, to_sdl(tex), src ? &s : nullptr, &d);
}

void SdlCanvas::blit_ex(const Texture& tex, const Rect* src, const FRect& dst, double angle_deg,
                        const FPoint* pivot, Flip flip) {
    const SDL_Rect s = src ? to_sdl(*src) : SDL_Rect{};
    const SDL_FRect d = to_sdl(dst);
    const SDL_FPoint c = pivot ? SDL_FPoint{pivot->x, pivot->y} : SDL_FPoint{};
    SDL_RenderCopyExF(renderer_, to_sdl(tex), src ? &s : nullptr, &d, angle_deg,
                      pivot ? &c : nullptr,
                      flip == Flip::Horizontal ? SDL_FLIP_HORIZONTAL : SDL_FLIP_NONE);
}

void SdlCanvas::present() { SDL_RenderPresent(renderer_); }

Texture SdlCanvas::create_texture(int w, int h, Access access, Blend blend, Filter filter) {
    SDL_Texture* t = SDL_CreateTexture(
        renderer_, SDL_PIXELFORMAT_ARGB8888,
        access == Access::Streaming ? SDL_TEXTUREACCESS_STREAMING : SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) return Texture{};
    // Blend and filter are properties of the texture in SDL, set per texture
    // rather than through SDL_HINT_RENDER_SCALE_QUALITY, which is global and read
    // at creation -- through it, one texture's sharpness would depend on the order
    // it and the light texture were made in.
    SDL_SetTextureBlendMode(t, to_sdl(blend));
    if (filter == Filter::Linear) SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
    return Texture{t, w, h};
}

void SdlCanvas::destroy_texture(Texture& tex) {
    if (tex) SDL_DestroyTexture(to_sdl(tex));
    tex = Texture{};
}

void SdlCanvas::update_texture(const Texture& tex, const Rect* rect, const void* pixels,
                               int pitch_bytes) {
    const SDL_Rect r = rect ? to_sdl(*rect) : SDL_Rect{};
    SDL_UpdateTexture(to_sdl(tex), rect ? &r : nullptr, pixels, pitch_bytes);
}

const char* SdlCanvas::error() const { return SDL_GetError(); }

} // namespace gfx
