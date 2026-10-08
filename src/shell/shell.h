#pragma once
#include <SDL.h>
#include <cstdint>
#include <vector>
#include "game/camera.h"
#include "game/display.h"
#include "physics/grid.h"
#include "render/backdrop_set.h"
#include "render/frame.h"
#include "render/light.h"
#include "scene/scene_list.h"

// The SDL side of the game: the window's render targets, the display modes, and
// turning asset files into textures. What main.cpp used to define above main()
// and inside it, collected so main.cpp is the loop and the wiring and nothing
// else.
//
// Everything here makes SDL calls and is therefore reachable only from the game
// executable. The decisions it used to make inline that do not need SDL --
// which mode to open at, what a backdrop file says, what the HUD reads -- are in
// game/display.h, render/backdrop_set.h and present/present.h, each with a suite.
namespace shell {

// Loads a plain (non-scene) authored BMP as a texture -- the backdrop and prop
// art. `colorkey` marks pixel_art.COLOR_KEY (magenta, 0xFF00FF -- see
// tools/pixel_art.py) as transparent before the surface becomes a texture, which
// is how a sprite with an irregular silhouette gets transparency out of a 24-bit
// BMP with no alpha channel at all: SDL_CreateTextureFromSurface bakes a
// colour-keyed surface's key into the resulting texture's alpha, so nothing
// downstream has to know the trick happened. Opaque layers pass colorkey=false
// and get a plain opaque texture.
SDL_Texture* load_art_texture(SDL_Renderer* renderer, const char* path, bool colorkey);

// Everything whose size is a function of the display mode and the scene's
// scale, and nothing else.
//
// The list is short because the pixel buffer is grid-sized rather than
// viewport-sized, and Camera is told the viewport's size as an argument to
// follow() rather than storing it. Neither had anything to do with resolution
// switching when they were written; both are why switching is a matter of
// rebuilding two textures instead of rebuilding the renderer.
struct RenderTargets {
    SDL_Texture* cells = nullptr;  // the world's ARGB streaming texture
    SDL_Texture* light_texture = nullptr;
    LightField light{1, 1};  // replaced wholesale on every mode change
};

// Builds the render targets for `mode` at `scale` and, on success, swaps them in
// and resizes the window. Constructs everything new before releasing anything
// old: on failure nothing has been destroyed, so the caller keeps playing at the
// mode it already had. The argument for each texture setting is at the
// definition.
bool apply_mode(SDL_Window* window, SDL_Renderer* renderer, const DisplayMode& mode, int scale,
                RenderTargets& targets);

// Whether a mode's window actually fits on the display. No answer counts as yes:
// a driver that cannot report its own bounds should not hide every mode.
bool mode_fits(const DisplayMode& mode, int display_index);

// The scene's backdrop: every layer of assets/<def.backdrop>/backdrop.txt as a
// texture, at `scale`. Destroys whatever `backdrop` held first. A scene with no
// backdrop, or one whose file is refused, ends with no layers and the reason on
// stderr -- the whole set is refused rather than drawn missing the layer it
// could not read. Returns the set's anchor.
backdrop_set::Anchor load_backdrop(SDL_Renderer* renderer, const scene_list::SceneDef& def,
                                   int scale, frame::Backdrop& backdrop);

// Destroys the layers' textures and empties the backdrop. The destroy is the
// point: dropping the vector alone leaks a set's textures every scene change.
void clear_backdrop(frame::Backdrop& backdrop);

// Uploads the camera's view of the grid into `cells`, `visible_w` x `visible_h`
// cells. `scratch` is only used when the view reaches past the grid's edge.
void upload_cells(SDL_Texture* cells, const Grid& grid, const Camera& camera, int visible_w,
                  int visible_h, std::vector<uint32_t>& scratch);

} // namespace shell
