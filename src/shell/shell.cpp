#include "shell/shell.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include "gpu/gl_canvas.h"
#include "render/sdl_canvas.h"

namespace shell {

namespace {

// Made hidden and windowed, at the smallest size, and shown only once
// show_display has put it in its mode. Hidden for two reasons. A fullscreen
// window shown at creation switches the display to the closest mode SDL can
// find at the desktop's refresh, and then again to the one asked for; a hidden
// one switches once, when it is shown. And a hidden window already has its
// title bar and borders, which is the only way to measure them -- and which
// sizes fit the desktop depends on them.
//
// ALLOW_HIGHDPI with the per-monitor DPI awareness main.cpp sets: on Windows the
// window is then measured in physical pixels, so a 3440x1440 window is that many
// pixels on a desktop scaled to 150% rather than a smaller one stretched by the
// compositor.
SDL_Window* make_window(Uint32 extra_flags) {
    return SDL_CreateWindow("SLOP Pixel Physics", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                            COMMON_SIZES[0].window_w, COMMON_SIZES[0].window_h,
                            SDL_WINDOW_HIDDEN | SDL_WINDOW_ALLOW_HIGHDPI | extra_flags);
}

// The window's title bar and borders, as last measured while it had them. A
// borderless or fullscreen window has none to measure, but the question the
// menu asks of it -- which windowed sizes would fit -- is about the window it
// would become, decorations and all. Measured first on the hidden window
// before it is ever shown, so it is known from the start.
struct Borders {
    int top = 0, left = 0, bottom = 0, right = 0;
};
Borders last_borders;

// The refresh rate a fullscreen size is entered at: the highest the display
// lists for it (fullscreen_sizes), or 0 -- the desktop's -- for a size it does
// not list.
int refresh_for(const ScreenOptions& o, const DisplayMode& size) {
    for (const FullscreenMode& m : o.fullscreen)
        if (m.size == size) return m.refresh_hz;
    return 0;
}

// Puts the window into `to`'s mode and size. Shared by start-up and the menu,
// so the window the game opens in and the one the menu switches to are made the
// same way.
bool set_window_mode(SDL_Window* window, const DisplaySettings& to, const ScreenOptions& o) {
    switch (to.window) {
        case WindowMode::Windowed: {
            if (SDL_SetWindowFullscreen(window, 0) != 0) return false;
            SDL_SetWindowSize(window, to.size.window_w, to.size.window_h);
            // Centred on the display it is on, not on display 0: a player who
            // dragged the window to a second monitor switched sizes there.
            const int index = std::max(0, SDL_GetWindowDisplayIndex(window));
            SDL_SetWindowPosition(window, static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(index)),
                                  static_cast<int>(SDL_WINDOWPOS_CENTERED_DISPLAY(index)));
            return true;
        }
        case WindowMode::Borderless:
            return SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0;
        case WindowMode::Fullscreen: {
            // Out of exclusive fullscreen first, when already in it. SDL applies a
            // new mode to a window that is already fullscreen by switching the
            // display and reporting the new size, without resizing the window's
            // own rectangle to match; leaving and re-entering is the path that
            // resizes everything, at the cost of one more resync of the monitor,
            // on a switch that is rare and is confirmed anyway.
            if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ==
                SDL_WINDOW_FULLSCREEN)
                SDL_SetWindowFullscreen(window, 0);
            SDL_DisplayMode dm{};
            dm.w = to.size.window_w;
            dm.h = to.size.window_h;
            dm.refresh_rate = refresh_for(o, to.size);
            if (SDL_SetWindowDisplayMode(window, &dm) != 0) return false;
            return SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN) == 0;
        }
    }
    return false;
}

} // namespace

void show_display(Display& d, DisplaySettings& settings, const ScreenOptions& options) {
    if (!set_window_mode(d.window, settings, options)) {
        std::fprintf(stderr, "Could not open %s at %dx%d (%s); opening windowed.\n",
                     window_mode_name(settings.window), settings.size.window_w,
                     settings.size.window_h, SDL_GetError());
        settings.window = WindowMode::Windowed;
        settings.size = fit_size(WindowMode::Windowed, settings.size, options);
        set_window_mode(d.window, settings, options);
    }
    SDL_ShowWindow(d.window);
}

ScreenOptions screen_options(SDL_Window* window) {
    ScreenOptions o;
    const int index = window ? std::max(0, SDL_GetWindowDisplayIndex(window)) : 0;

    SDL_DisplayMode desk{};
    if (SDL_GetDesktopDisplayMode(index, &desk) == 0) o.desktop = DisplayMode{desk.w, desk.h};

    // Usable bounds rather than the display's, so the taskbar counts; and the
    // window's own decorations on top, so it is the whole window that fits and
    // not only the part the game draws in. A window that does not fit is one
    // SDL will happily create, with its title bar -- the handle to move it by --
    // or the bottom of the game off the edge of the desktop.
    //
    // That is not a formality on a monitor with an auto-hiding taskbar, where
    // the usable area is the whole display: there a 3440x1440 window on a
    // 3440x1440 monitor "fits" until its title bar is counted, and then has it
    // above the top of the screen.
    SDL_Rect usable{};
    const bool know_usable = SDL_GetDisplayUsableBounds(index, &usable) == 0;
    Borders b;
    if (window && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0 &&
        SDL_GetWindowBordersSize(window, &b.top, &b.left, &b.bottom, &b.right) == 0)
        last_borders = b;
    b = last_borders;
    for (int i = 0; i < COMMON_SIZE_COUNT; ++i) {
        // No answer is not the same as "no". A driver that cannot report its own
        // bounds should not be able to hide every size in the menu.
        o.windowed_fits[static_cast<size_t>(i)] =
            !know_usable || (COMMON_SIZES[i].window_w + b.left + b.right <= usable.w &&
                             COMMON_SIZES[i].window_h + b.top + b.bottom <= usable.h);
    }

    std::vector<FullscreenMode> reported;
    const int n = SDL_GetNumDisplayModes(index);
    for (int k = 0; k < n; ++k) {
        SDL_DisplayMode m{};
        if (SDL_GetDisplayMode(index, k, &m) == 0)
            reported.push_back(FullscreenMode{DisplayMode{m.w, m.h}, m.refresh_rate});
    }
    o.fullscreen = fullscreen_sizes(reported);
    return o;
}

bool open_display(RendererChoice want, bool vsync, Display& out) {
    if (want == RendererChoice::Gl) {
        gpu::GlCanvas::set_window_attributes();
        SDL_Window* window = make_window(SDL_WINDOW_OPENGL);
        std::string why = SDL_GetError();
        if (window) {
            std::unique_ptr<gpu::GlCanvas> gl =
                gpu::GlCanvas::create(window, "assets/shaders", vsync, why);
            if (gl) {
                std::printf("Renderer: OpenGL (%s)\n", gl->describe().c_str());
                out.window = window;
                out.gl = gl.get();
                out.canvas = std::move(gl);
                // Stated either way, not only when on: a new context's swap
                // interval is the driver's choice, and "off" has to mean off.
                set_vsync(out, vsync);
                return true;
            }
            // A fresh window for the SDL renderer rather than this one: a window
            // made for GL has had its pixel format chosen for GL, and SDL would
            // otherwise have to recreate it under the renderer anyway.
            SDL_DestroyWindow(window);
        }
        std::fprintf(stderr, "OpenGL renderer unavailable (%s); falling back to SDL.\n",
                     why.c_str());
    }

    SDL_Window* window = make_window(0);
    if (!window) {
        std::fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return false;
    }
    const Uint32 flags = SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0u);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, flags);
    if (!renderer) {
        std::fprintf(stderr, "Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return false;
    }
    std::printf("Renderer: SDL\n");
    out.window = window;
    out.renderer = renderer;
    out.gl = nullptr;
    out.canvas = std::make_unique<gfx::SdlCanvas>(renderer);
    return true;
}

bool apply_window(Display& d, const DisplaySettings& from, const DisplaySettings& to,
                  const ScreenOptions& options) {
    if (from.window != to.window || from.size != to.size) {
        if (!set_window_mode(d.window, to, options)) {
            std::fprintf(stderr, "Could not switch to %s at %dx%d: %s\n",
                         window_mode_name(to.window), to.size.window_w, to.size.window_h,
                         SDL_GetError());
            return false;
        }
    }
    // Set again after a mode switch as well as on a toggle: some drivers reset
    // the swap chain's pacing when the window goes in or out of fullscreen.
    set_vsync(d, to.vsync);
    return true;
}

bool set_vsync(Display& d, bool on) {
    // GlCanvas made its context current on this thread when it was created and
    // never releases it, so the swap interval set here is that context's.
    const int r =
        d.gl ? SDL_GL_SetSwapInterval(on ? 1 : 0) : SDL_RenderSetVSync(d.renderer, on ? 1 : 0);
    if (r != 0)
        std::fprintf(stderr, "Could not turn VSync %s: %s\n", on ? "on" : "off", SDL_GetError());
    return r == 0;
}

DisplayMode output_size(const Display& d) {
    int w = 0, h = 0;
    if (d.renderer)
        SDL_GetRendererOutputSize(d.renderer, &w, &h);
    else
        SDL_GL_GetDrawableSize(d.window, &w, &h);
    return DisplayMode{w, h};
}

void close_display(Display& d) {
    d.gl = nullptr;
    d.canvas.reset();  // the GL context goes with its canvas, before its window
    if (d.renderer) SDL_DestroyRenderer(d.renderer);
    if (d.window) SDL_DestroyWindow(d.window);
    d.renderer = nullptr;
    d.window = nullptr;
}

bool save_frame_bmp(gpu::GlCanvas& gl, const char* path) {
    std::vector<uint32_t> px;
    gl.read_pixels(px);
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(
        px.data(), gl.target_w(), gl.target_h(), 32,
        gl.target_w() * static_cast<int>(sizeof(uint32_t)), SDL_PIXELFORMAT_ARGB8888);
    const bool ok = s && SDL_SaveBMP(s, path) == 0;
    if (!ok) std::fprintf(stderr, "Could not save %s: %s\n", path, SDL_GetError());
    SDL_FreeSurface(s);
    return ok;
}

// Loads a plain (non-scene) authored BMP as a texture -- the backdrop and prop
// art. `colorkey` marks pixel_art.COLOR_KEY (magenta, 0xFF00FF -- see
// tools/pixel_art.py) as transparent before the surface becomes a texture, which
// is how a sprite with an irregular silhouette gets transparency out of a 24-bit
// BMP with no alpha channel at all: converting a colour-keyed surface to
// ARGB8888 bakes its key into the alpha, so nothing downstream has to know the
// trick happened. Backdrop layers (opaque, full-rect) pass colorkey=false and
// get a plain opaque texture.
//
// The conversion is done here rather than left to SDL_CreateTextureFromSurface,
// which makes the same one internally, so that the texture comes from the
// canvas and any backend can draw it. SDL_LoadBMP stays: it is SDL's file
// reader, not its renderer, and decoding a BMP is not a renderer's job.
gfx::Texture load_art_texture(gfx::Canvas& canvas, const char* path, bool colorkey) {
    SDL_Surface* surf = SDL_LoadBMP(path);
    if (!surf) {
        std::fprintf(stderr, "Failed to load %s: %s\n", path, SDL_GetError());
        return gfx::Texture{};
    }
    if (colorkey) {
        const Uint32 key = SDL_MapRGB(surf->format, 0xFF, 0x00, 0xFF);
        SDL_SetColorKey(surf, SDL_TRUE, key);
    }
    SDL_Surface* argb = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(surf);
    if (!argb) {
        std::fprintf(stderr, "Failed to convert %s: %s\n", path, SDL_GetError());
        return gfx::Texture{};
    }
    const gfx::Texture tex =
        canvas.create_texture(argb->w, argb->h, gfx::Access::Static, gfx::Blend::Alpha);
    if (tex)
        canvas.update_texture(tex, nullptr, argb->pixels, argb->pitch);
    else
        std::fprintf(stderr, "Failed to create texture from %s: %s\n", path, canvas.error());
    SDL_FreeSurface(argb);
    return tex;
}

// Builds the render targets for a `mode`-sized output and, on success, swaps
// them in.
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
// a scene change and a window resize as well as a mode change.
bool rebuild_targets(gfx::Canvas& canvas, const DisplayMode& mode, int scale,
                     RenderTargets& targets) {
    // Both blend modes and the light's filtering are given here, not set once at
    // startup, because they are properties of a texture and these are new
    // textures. Each of the three is load-bearing and none is obvious from the
    // create call's other arguments:
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
    gfx::Texture cells = canvas.create_texture(mode.padded_w(scale), mode.padded_h(scale),
                                               gfx::Access::Streaming, gfx::Blend::Alpha);
    if (!cells) {
        std::fprintf(stderr, "Could not create a %dx%d cell texture: %s\n", mode.padded_w(scale),
                     mode.padded_h(scale), canvas.error());
        return false;
    }

    LightField light(mode.padded_w(scale), mode.padded_h(scale));
    const gfx::Texture light_texture = canvas.create_texture(
        light.cols(), light.rows(), gfx::Access::Streaming, gfx::Blend::Add, gfx::Filter::Linear);
    if (!light_texture) {
        std::fprintf(stderr, "Could not create a %dx%d light texture: %s\n", light.cols(),
                     light.rows(), canvas.error());
        canvas.destroy_texture(cells);
        return false;
    }

    // A streaming texture is created with undefined contents, and the upload each
    // frame only ever writes the rect the grid actually covers. A world smaller than
    // the viewport on either axis leaves the remainder holding whatever the driver's
    // allocation contained, which shows as garbage along the edge rather than as the
    // backdrop. Cleared here for a new texture, and by main.cpp again on every scene
    // load -- see blank_cells for why once is not enough.
    blank_cells(canvas, cells);

    canvas.destroy_texture(targets.cells);
    canvas.destroy_texture(targets.light_texture);
    targets.cells = cells;
    targets.light_texture = light_texture;
    targets.light = std::move(light);
    return true;
}

// --- the backdrop -----------------------------------------------------------

void clear_backdrop(gfx::Canvas& canvas, frame::Backdrop& backdrop) {
    for (frame::ParallaxLayer& l : backdrop.layers) canvas.destroy_texture(l.texture);
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
backdrop_set::Anchor load_backdrop(gfx::Canvas& canvas, const scene_list::SceneDef& def, int scale,
                                   frame::Backdrop& backdrop) {
    clear_backdrop(canvas, backdrop);
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
        const gfx::Texture tex = load_art_texture(canvas, path.c_str(), !sp.opaque);
        if (!tex) {
            ++missing;
            continue;
        }
        frame::ParallaxLayer l;
        l.texture = tex;
        l.w = backdrop_set::width_of(set, sp) * scale;
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

void blank_cells(gfx::Canvas& canvas, const gfx::Texture& cells) {
    if (!cells || cells.w <= 0 || cells.h <= 0) return;
    const std::vector<uint32_t> blank(static_cast<size_t>(cells.w) * static_cast<size_t>(cells.h),
                                      0u);
    canvas.update_texture(cells, nullptr, blank.data(),
                          cells.w * static_cast<int>(sizeof(uint32_t)));
}

// Upload only the visible rect, not the whole grid, starting from the camera's
// current view rather than always (0, 0). Clamped by the caller to the grid's
// own size so this stays correct if the grid is ever smaller than the viewport.
//
// Straight from the grid's own buffer at the grid's pitch, so SDL reads the
// right columns out of each row and skips the rest -- no copy. Only when the
// window lies wholly inside the grid, though: a scene with the horizontal clamp
// dropped can put the window past its edge, and those cells have to be cleared
// rather than read from outside the buffer. That case copies.
void upload_cells(gfx::Canvas& canvas, const gfx::Texture& cells, const Grid& grid,
                  const Camera& camera, int visible_w, int visible_h,
                  std::vector<uint32_t>& scratch, const std::vector<frame::HiddenCell>* hidden) {
    const std::vector<uint32_t>& pixels = grid.get_pixels();
    const gfx::Rect visible_rect{0, 0, visible_w, visible_h};
    const int grid_w = grid.get_width();
    const int grid_h = grid.get_height();
    const int view_x = camera.view_x(), view_y = camera.view_y();
    const bool inside =
        view_x >= 0 && view_y >= 0 && view_x + visible_w <= grid_w && view_y + visible_h <= grid_h;
    const bool hiding = hidden && !hidden->empty();
    if (inside && !hiding) {
        canvas.update_texture(cells, &visible_rect,
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
    if (hiding) {
        for (const frame::HiddenCell& h : *hidden) {
            const int wx = h.index % grid_w - view_x, wy = h.index / grid_w - view_y;
            if (wx < 0 || wy < 0 || wx >= visible_w || wy >= visible_h) continue;
            scratch[static_cast<size_t>(wy) * visible_w + wx] = h.fill;
        }
    }
    canvas.update_texture(cells, &visible_rect, scratch.data(),
                          visible_w * static_cast<int>(sizeof(uint32_t)));
}

} // namespace shell
