#pragma once
#include <SDL.h>
#include <cstdint>
#include <memory>
#include <vector>
#include "game/camera.h"
#include "game/display.h"
#include "physics/grid.h"
#include "render/backdrop_set.h"
#include "render/canvas.h"
#include "render/frame.h"
#include "render/light.h"
#include "scene/scene_list.h"

// The SDL side of the game: the window and its modes, the render targets, and
// turning asset files into textures. What main.cpp used to define above main()
// and inside it, collected so main.cpp is the loop and the wiring and nothing
// else.
//
// Everything here makes SDL calls and is therefore reachable only from the game
// executable. The decisions it used to make inline that do not need SDL --
// which mode to open at, what a backdrop file says, what the HUD reads -- are in
// game/display.h, render/backdrop_set.h and present/present.h, each with a suite.
namespace gpu {
class GlCanvas;
}

namespace shell {

// The window and the backend that draws into it. `canvas` is what everything
// draws through; `gl` is the same object when the backend is OpenGL (for its
// GL-only debug keys, F5 and F12) and null otherwise; `renderer` is set only for
// the SDL backend.
struct Display {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    std::unique_ptr<gfx::Canvas> canvas;
    gpu::GlCanvas* gl = nullptr;
};

// Opens the window, hidden, with the backend `want` asks for and VSync as
// `vsync` says. GL that fails at any step -- no 3.3 core context, a missing
// entry point, a shader that will not compile, an incomplete framebuffer -- is
// reported on stderr and replaced by the SDL renderer on a fresh window, so
// asking for GL never costs a session. Returns false only when SDL itself cannot
// open a window, which is fatal either way.
//
// Hidden until show_display, so the window can be measured -- its title bar
// decides which sizes fit -- before anything decides what size it is.
bool open_display(RendererChoice want, bool vsync, Display& out);

// What the display the window is on offers: its desktop size, which common
// windowed sizes fit its usable area with the window's title bar and borders,
// and its fullscreen modes (game/display.h decides from these).
ScreenOptions screen_options(SDL_Window* window);

// Puts the window open_display made into `settings`' mode and size, and shows
// it. A mode the display refuses opens windowed instead, and `settings` is
// changed to say so -- a session at the wrong size beats none.
void show_display(Display& d, DisplaySettings& settings, const ScreenOptions& options);

// Releases the canvas, then the renderer, then the window -- the order each
// depends on the next. Every texture the canvas made must be destroyed first.
void close_display(Display& d);

// Writes the GL canvas's current frame -- between frames, the one last
// presented -- to `path` as a 32-bit BMP. False, with the reason on stderr, if
// the file cannot be written.
bool save_frame_bmp(gpu::GlCanvas& gl, const char* path);

// Loads a plain (non-scene) authored BMP as a texture -- the backdrop and prop
// art. `colorkey` marks pixel_art.COLOR_KEY (magenta, 0xFF00FF -- see
// tools/pixel_art.py) as transparent before the surface becomes a texture, which
// is how a sprite with an irregular silhouette gets transparency out of a 24-bit
// BMP with no alpha channel at all: converting a colour-keyed surface to ARGB8888
// bakes its key into the alpha, so nothing downstream has to know the trick
// happened. Opaque layers pass colorkey=false and get a plain opaque texture.
// Returns a null texture, with the reason on stderr, on failure.
gfx::Texture load_art_texture(gfx::Canvas& canvas, const char* path, bool colorkey);

// Everything whose size is a function of the display mode and the scene's
// scale, and nothing else.
//
// The list is short because the pixel buffer is grid-sized rather than
// viewport-sized, and Camera is told the viewport's size as an argument to
// follow() rather than storing it. Neither had anything to do with resolution
// switching when they were written; both are why switching is a matter of
// rebuilding two textures instead of rebuilding the renderer.
struct RenderTargets {
    gfx::Texture cells;  // the world's ARGB streaming texture
    gfx::Texture light_texture;
    LightField light{1, 1};  // replaced wholesale on every mode change
};

// Builds the render targets for a `size`-pixel output at `scale` and, on
// success, swaps them in. Constructs everything new before releasing anything
// old: on failure nothing has been destroyed, so the caller keeps playing with
// the targets it already had. The argument for each texture setting is at the
// definition.
//
// Touches no window. It is called for three reasons -- a mode switch, a scene
// with a different scale, and the window system resizing the window -- and only
// the first is a reason to move the window.
bool rebuild_targets(gfx::Canvas& canvas, const DisplayMode& size, int scale,
                     RenderTargets& targets);

// Moves the window from `from` to `to`: window mode, size, VSync. A move that
// keeps the mode and size touches only VSync, so a VSync toggle does not
// re-centre a window the player has dragged somewhere. False, with the reason on
// stderr, when SDL refuses the mode; the window may then be part-way, and the
// caller puts it back by moving it to `from`.
bool apply_window(Display& d, const DisplaySettings& from, const DisplaySettings& to,
                  const ScreenOptions& options);

// VSync on or off, on whichever backend draws: SDL_RenderSetVSync for the SDL
// renderer, the swap interval for GL. False, with the reason on stderr, when the
// driver refuses -- which leaves the frame unpaced or paced, not broken.
bool set_vsync(Display& d, bool on);

// The size, in physical pixels, of what the backend draws into: the renderer's
// output, or the GL drawable. What the render targets are sized from, rather
// than the window's size, because under DPI scaling the two differ and the
// window's is the one that is not pixels.
DisplayMode output_size(const Display& d);

// The scene's backdrop: every layer of assets/<def.backdrop>/backdrop.txt as a
// texture, at `scale`. Destroys whatever `backdrop` held first. A scene with no
// backdrop, or one whose file is refused, ends with no layers and the reason on
// stderr -- the whole set is refused rather than drawn missing the layer it
// could not read. Returns the set's anchor.
backdrop_set::Anchor load_backdrop(gfx::Canvas& canvas, const scene_list::SceneDef& def, int scale,
                                   frame::Backdrop& backdrop);

// Destroys the layers' textures and empties the backdrop. The destroy is the
// point: dropping the vector alone leaks a set's textures every scene change.
void clear_backdrop(gfx::Canvas& canvas, frame::Backdrop& backdrop);

// Sets every texel of the cell texture to transparent. upload_cells only ever
// writes the rect the grid covers, so whatever is outside it stays until this
// clears it -- once when the texture is made, and again on every scene load,
// because a scene that loads into the same texture at the same scale inherits
// the previous scene's pixels in any part of the view the new grid does not cover.
void blank_cells(gfx::Canvas& canvas, const gfx::Texture& cells);

// Uploads the camera's view of the grid into `cells`, `visible_w` x `visible_h`
// cells. `scratch` is only used when the view reaches past the grid's edge, or
// when `hidden` names cells to draw something else in -- the cells of the loose
// bodies drawn as sprites (present::paint_bodies), which would otherwise be
// drawn twice, once of them a cell at a time. Each gets its `fill` instead.
void upload_cells(gfx::Canvas& canvas, const gfx::Texture& cells, const Grid& grid,
                  const Camera& camera, int visible_w, int visible_h,
                  std::vector<uint32_t>& scratch,
                  const std::vector<frame::HiddenCell>* hidden = nullptr);

} // namespace shell
