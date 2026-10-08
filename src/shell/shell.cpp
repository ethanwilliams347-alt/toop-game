#include "shell/shell.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace shell {

// Loads a plain (non-scene) authored BMP as a texture -- the backdrop and prop
// art. `colorkey` marks pixel_art.COLOR_KEY (magenta, 0xFF00FF -- see
// tools/pixel_art.py) as transparent before the surface becomes a texture, which
// is how a sprite with an irregular silhouette gets transparency out of a 24-bit
// BMP with no alpha channel at all: SDL_CreateTextureFromSurface bakes a
// colour-keyed surface's key into the resulting texture's alpha, so nothing
// downstream has to know the trick happened. Backdrop layers (opaque,
// full-rect) pass colorkey=false and get a plain opaque texture.
SDL_Texture* load_art_texture(SDL_Renderer* renderer, const char* path, bool colorkey) {
    SDL_Surface* surf = SDL_LoadBMP(path);
    if (!surf) {
        std::fprintf(stderr, "Failed to load %s: %s\n", path, SDL_GetError());
        return nullptr;
    }
    if (colorkey) {
        const Uint32 key = SDL_MapRGB(surf->format, 0xFF, 0x00, 0xFF);
        SDL_SetColorKey(surf, SDL_TRUE, key);
    }
    SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    if (!tex) {
        std::fprintf(stderr, "Failed to create texture from %s: %s\n", path, SDL_GetError());
        return nullptr;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return tex;
}

// Builds the render targets for `mode` and, on success, swaps them in and
// resizes the window.
//
// Constructs everything new before releasing anything old, which is why this
// returns a bool instead of exiting. At startup a failed texture allocation can
// reasonably end the process; a hundred frames into a session it cannot, because
// the player still has a world open and the only thing that went wrong is a
// setting they can change back. On failure nothing has been destroyed and
// nothing reassigned, so the caller keeps playing at the mode it already had.
//
// `scale` joins `mode` because the render targets are sized in cells, and how
// many cells a window holds is a question about both. It is therefore called on
// a scene change as well as a mode change; the window resize at the bottom is
// idempotent when only the scale moved.
bool apply_mode(SDL_Window* window, SDL_Renderer* renderer, const DisplayMode& mode, int scale,
                RenderTargets& targets) {
    SDL_Texture* cells =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                          mode.padded_w(scale), mode.padded_h(scale));
    if (!cells) {
        std::fprintf(stderr, "Could not create a %dx%d cell texture: %s\n", mode.padded_w(scale),
                     mode.padded_h(scale), SDL_GetError());
        return false;
    }

    LightField light(mode.padded_w(scale), mode.padded_h(scale));
    SDL_Texture* light_texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                          light.cols(), light.rows());
    if (!light_texture) {
        std::fprintf(stderr, "Could not create a %dx%d light texture: %s\n", light.cols(),
                     light.rows(), SDL_GetError());
        SDL_DestroyTexture(cells);
        return false;
    }

    // Both blend modes and the light's filtering are re-applied here, not set
    // once at startup, because they are properties of a texture and these are
    // new textures. Each of the three is load-bearing and none is obvious from
    // the create call:
    //
    // BLEND on the cells. Empty is 0x00000000 in MATERIALS, so an empty cell
    // is transparent rather than black -- but only if the texture is
    // composited rather than blitted. Without this line the alpha is carried
    // to the screen and then ignored, which looks exactly like opaque black.
    //
    // ADD, not BLEND, on the light. Light is something the scene gains, not
    // something laid over it: an alpha blend towards orange would wash the
    // terrain's colour out towards the flame's, where addition brightens
    // whatever is already there and leaves a lit grey wall reading as a grey
    // wall. It also means black is free -- an unlit block adds nothing --
    // which is what lets the same texture cover the whole viewport rather
    // than needing a mask.
    //
    // Linear filtering is the entire reason a downsampled light grid is
    // acceptable; at nearest-neighbour it is BLOCK-sized squares of flat
    // colour. Set per-texture rather than through
    // SDL_HINT_RENDER_SCALE_QUALITY, which is read at creation time and is
    // global -- routing this through a global would make the cell texture's
    // sharpness depend on the order the two are created in.
    SDL_SetTextureBlendMode(cells, SDL_BLENDMODE_BLEND);
    SDL_SetTextureBlendMode(light_texture, SDL_BLENDMODE_ADD);
    SDL_SetTextureScaleMode(light_texture, SDL_ScaleModeLinear);

    // A streaming texture is created with undefined contents, and the upload each
    // frame only ever writes the rect the grid actually covers. A world smaller than
    // the viewport on either axis leaves the remainder holding whatever the driver's
    // allocation contained, which shows as garbage along the edge rather than as the
    // backdrop. Once per texture, since it can only ever be wrong once.
    {
        const std::vector<uint32_t> blank(
            static_cast<size_t>(mode.padded_w(scale)) * mode.padded_h(scale), 0);
        SDL_UpdateTexture(cells, nullptr, blank.data(), mode.padded_w(scale) * sizeof(uint32_t));
    }

    if (targets.cells) SDL_DestroyTexture(targets.cells);
    if (targets.light_texture) SDL_DestroyTexture(targets.light_texture);
    targets.cells = cells;
    targets.light_texture = light_texture;
    targets.light = std::move(light);

    SDL_SetWindowSize(window, mode.window_w, mode.window_h);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    return true;
}

// Whether a mode's window actually fits on the display the window is on.
//
// A window larger than the desktop is one SDL will happily create, with the
// bottom and right of the game off-screen -- including the settings menu the
// player would use to change it back. Usable bounds rather than raw bounds, so a
// taskbar counts.
bool mode_fits(const DisplayMode& mode, int display_index) {
    SDL_Rect usable;
    if (SDL_GetDisplayUsableBounds(display_index, &usable) != 0) {
        // No answer is not the same as "no". A driver that cannot report its own
        // bounds should not be able to hide every mode in the menu.
        return true;
    }
    return mode.window_w <= usable.w && mode.window_h <= usable.h;
}

// --- the backdrop -----------------------------------------------------------

void clear_backdrop(frame::Backdrop& backdrop) {
    for (frame::ParallaxLayer& l : backdrop.layers)
        if (l.texture) SDL_DestroyTexture(l.texture);
    backdrop = frame::Backdrop{};
}

// Everything a layer is, and the argument for each number, is in the set's
// backdrop.txt and in render/backdrop_set.h; what is left here is turning
// records into textures.
//
// One art pixel is one world cell, so a layer's on-screen size is the art's
// native size times the scale and nothing else -- no fitting to the window. The
// caller passes the scale actually in use rather than the one the scene asked
// for, so a scene whose requested scale could not be applied still lines up.
backdrop_set::Anchor load_backdrop(SDL_Renderer* renderer, const scene_list::SceneDef& def,
                                   int scale, frame::Backdrop& backdrop) {
    clear_backdrop(backdrop);
    if (def.backdrop.empty()) {
        std::printf("Backdrop: none\n");
        return backdrop_set::Anchor::Corner;
    }
    const std::string dir = "assets/" + def.backdrop + "/";
    std::string error;
    const backdrop_set::Set set = backdrop_set::load(dir + "backdrop.txt", dir, &error);
    if (!error.empty()) {
        // Reported and drawn without, the way every parser here degrades.
        std::fprintf(stderr, "ERROR: %s\n         scene '%s' draws no backdrop.\n", error.c_str(),
                     def.name.c_str());
        return backdrop_set::Anchor::Corner;
    }

    backdrop.layers.reserve(set.layers.size());
    int missing = 0;
    for (const backdrop_set::Layer& sp : set.layers) {
        const std::string path = set.dir + sp.file;
        SDL_Texture* tex = load_art_texture(renderer, path.c_str(), !sp.opaque);
        if (!tex) {
            ++missing;
            continue;
        }
        frame::ParallaxLayer l;
        l.texture = tex;
        l.w = set.native_w * scale;
        l.h = set.native_h * scale;
        l.tex_h = set.native_h;
        l.parallax_x = backdrop_set::factor_of(set, sp);
        l.parallax_y = backdrop_set::vertical_factor_of(set, sp);
        l.bands = sp.bands;
        l.line_scroll = sp.plane;
        l.on_plane = sp.on_plane;
        l.ripple_row0 = sp.ripple_row0;
        l.ripple_row1 = sp.ripple_row1;
        l.drift = sp.drift;
        l.grade = frame::Grade{};  // identity; the art carries its own depth
        l.is_foreground = sp.foreground;
        backdrop.layers.push_back(l);
    }
    backdrop.rig = set.rig;
    backdrop.ripple_amplitude = set.ripple_amplitude;

    // Printed for the same reason Scene: and Props: are -- a backdrop that
    // half-loaded is otherwise a frame you have to recognise by eye.
    std::printf("Backdrop: %d of %d %s layers at %dx, anchored at the %s",
                static_cast<int>(backdrop.layers.size()), static_cast<int>(set.layers.size()),
                def.backdrop.c_str(), scale,
                set.anchor == backdrop_set::Anchor::Corner ? "corner" : "standing camera");
    if (set.has_rig)
        std::printf(", rig horizon %d contact %d", set.rig.horizon_row, set.rig.contact_row);
    std::printf("\n");
    if (missing)
        std::fprintf(stderr,
                     "WARNING: %d %s layer(s) failed to load - rerun that set's "
                     "generator and rebuild.\n",
                     missing, def.backdrop.c_str());
    return set.anchor;
}

// --- the cells -----------------------------------------------------------------

// Upload only the visible rect, not the whole grid, starting from the camera's
// current view rather than always (0, 0). Clamped by the caller to the grid's
// own size so this stays correct if the grid is ever smaller than the viewport.
//
// Straight from the grid's own buffer at the grid's pitch, so SDL reads the
// right columns out of each row and skips the rest -- no copy. Only when the
// window lies wholly inside the grid, though: a scene with the horizontal clamp
// dropped can put the window past its edge, and those cells have to be cleared
// rather than read from outside the buffer. That case copies.
//
// (A near-terrain tint pass used to sit here, compiled in and switched off,
// blending terrain toward the generated ground plane. It went with that
// backdrop; render/backdrop_set.h has the account.)
void upload_cells(SDL_Texture* cells, const Grid& grid, const Camera& camera, int visible_w,
                  int visible_h, std::vector<uint32_t>& scratch) {
    const std::vector<uint32_t>& pixels = grid.get_pixels();
    const SDL_Rect visible_rect{0, 0, visible_w, visible_h};
    const int grid_w = grid.get_width();
    const int grid_h = grid.get_height();
    const int view_x = camera.view_x(), view_y = camera.view_y();
    const bool inside =
        view_x >= 0 && view_y >= 0 && view_x + visible_w <= grid_w && view_y + visible_h <= grid_h;
    if (inside) {
        SDL_UpdateTexture(cells, &visible_rect,
                          pixels.data() + static_cast<size_t>(view_y) * grid_w + view_x,
                          grid_w * static_cast<int>(sizeof(uint32_t)));
        return;
    }
    scratch.assign(static_cast<size_t>(visible_w) * static_cast<size_t>(visible_h), 0u);
    for (int wy = 0; wy < visible_h; ++wy) {
        const int gy = view_y + wy;
        if (gy < 0 || gy >= grid_h) continue;
        for (int wx = 0; wx < visible_w; ++wx) {
            const int gx = view_x + wx;
            if (gx < 0 || gx >= grid_w) continue;
            scratch[static_cast<size_t>(wy) * visible_w + wx] =
                pixels[static_cast<size_t>(gy) * grid_w + gx];
        }
    }
    SDL_UpdateTexture(cells, &visible_rect, scratch.data(),
                      visible_w * static_cast<int>(sizeof(uint32_t)));
}

} // namespace shell
