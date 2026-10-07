#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>
#include "game/boot.h"
#include "game/camera.h"
#include "game/debug_view.h"
#include "game/display.h"
#include "game/input_log.h"
#include "game/level.h"
#include "game/pacer.h"
#include "game/run.h"
#include "game/scene_activation.h"
#include "game/settings_menu.h"
#include "render/backdrop_layers.h"
#include "render/bg1_backdrop.h"
#include "render/rig_backdrop.h"
#include "render/backdrop_wrap.h"
#include "render/frame.h"
#include "render/light.h"
#include "render/overlay.h"
#include "render/player_anim.h"
#include "render/surface_plane.h"
#include "scene/bmp.h"
#include "scene/level_files.h"
#include "scene/props.h"
#include "scene/scene.h"
#include "scene/scene_list.h"
#include "scene/sprites.h"
#include "ui/text.h"
#include "ui/hotbar.h"

// The world's size, the objective column and the terrain scans that plant
// things on the ground live in game/boot.h, which is SDL-free and has a suite.
// What is left in this file is the SDL half: a window, a renderer, textures, the
// pump and the draw. Aliased here because the lines below read better as
// GRID_WIDTH than as a qualified name.
constexpr int GRID_WIDTH = boot::GRID_WIDTH;
constexpr int GRID_HEIGHT = boot::GRID_HEIGHT;

// SDL_RenderCopy below stretches the whole viewport-sized texture across the
// whole window (two null rects). The texture is the padded viewport and the
// window is exactly the scale times that, so the blit is 1:1 whatever size the
// world and the window are.
//
// Camera owns every screen-to-world and world-to-screen conversion and the
// viewport's position in the world, so mouse and render coordinates are correct
// at any grid size and any camera offset. The texture is sized to the viewport
// rather than to the whole grid, so upload cost does not scale with world size.

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

// The props layer, defined in render/frame.h with the layer ordering it is part
// of. Aliased rather than qualified everywhere below: the planting scan further
// down is scene setup, not rendering.
using frame::Prop;

// Everything whose size is a function of the display mode, and nothing else.
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
bool apply_mode(SDL_Window* window, SDL_Renderer* renderer,
                const DisplayMode& mode, int scale, RenderTargets& targets) {
    SDL_Texture* cells = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        mode.padded_w(scale), mode.padded_h(scale));
    if (!cells) {
        std::fprintf(stderr, "Could not create a %dx%d cell texture: %s\n",
                     mode.padded_w(scale), mode.padded_h(scale), SDL_GetError());
        return false;
    }

    LightField light(mode.padded_w(scale), mode.padded_h(scale));
    SDL_Texture* light_texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        light.cols(), light.rows());
    if (!light_texture) {
        std::fprintf(stderr, "Could not create a %dx%d light texture: %s\n",
                     light.cols(), light.rows(), SDL_GetError());
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

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        std::fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return 1;
    }

    // Which modes this display can actually show, and which of them to open at.
    // Only the first needs SDL: mode_fits asks the display, choose_display_mode
    // decides, and the deciding is the half with the wrong answers in it -- a
    // stored mode that no longer fits, and a desktop smaller than every mode in
    // the table. Both are in display_test rather than in a checklist step nobody
    // can run without two monitors.
    bool mode_available[DISPLAY_MODE_COUNT];
    for (int i = 0; i < DISPLAY_MODE_COUNT; ++i)
        mode_available[i] = mode_fits(DISPLAY_MODES[i], 0);

    const int stored_mode = load_display_mode();
    const ModeChoice choice =
        choose_display_mode(mode_available, DISPLAY_MODE_COUNT, stored_mode);
    int mode_index = choice.index;
    switch (choice.why) {
        case ModeChoice::Why::NothingFits:
            std::fprintf(stderr, "No display mode fits this desktop; opening at %dx%d anyway.\n",
                         DISPLAY_MODES[mode_index].window_w, DISPLAY_MODES[mode_index].window_h);
            break;
        case ModeChoice::Why::StoredTooBig:
            std::fprintf(stderr, "Stored mode %dx%d does not fit this display; ignoring it.\n",
                         DISPLAY_MODES[stored_mode].window_w, DISPLAY_MODES[stored_mode].window_h);
            break;
        case ModeChoice::Why::Stored:
        case ModeChoice::Why::Largest:
            break;
    }
    DisplayMode mode = DISPLAY_MODES[mode_index];

    // How many screen pixels one world cell covers, for the scene loaded right
    // now. It starts at the default and activate_scene moves it, so it is
    // deliberately not const and deliberately not read off Camera -- the camera
    // is told this value, it does not decide it.
    //
    // Everything sized in cells is a function of the pair (mode, view_scale): the
    // two render targets, the light field, the camera's viewport, and the upload
    // rect. When either half moves, apply_mode rebuilds all of it.
    int view_scale = Camera::DEFAULT_SCALE;

    // Printed for the same reason the seed is: a mode chosen by a fallback path
    // is invisible otherwise, and "it opened smaller than I asked for" is not
    // something a player can debug from the window alone.
    std::printf("Display: %dx%d (%dx%d cells at %dx)\n", mode.window_w, mode.window_h,
                mode.viewport_w(view_scale), mode.viewport_h(view_scale), view_scale);

    SDL_Window* window = SDL_CreateWindow(
        "SLOP Pixel Physics",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        mode.window_w, mode.window_h,
        SDL_WINDOW_SHOWN
    );
    if (!window) {
        std::fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        std::fprintf(stderr, "Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The world's ARGB8888 streaming texture and the light texture, both sized
    // to the viewport rather than to the whole grid, and both rebuilt by this
    // same call whenever the mode changes. Here a failure is fatal, unlike in
    // the menu: there is no earlier mode to fall back to.
    RenderTargets targets;
    if (!apply_mode(window, renderer, mode, view_scale, targets)) {
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    // The backdrop: two static parallax layers plus the ground plane, generated
    // by tools/generate_backdrop.py, which sizes each layer to cover the
    // camera's full pan range at its own factor.
    //
    // Which BMP each key resolves to is data -- see src/scene/sprites.h. The
    // literals below are the fallback, so this file still says what ships; the
    // manifest is how a drawing dropped into assets/ gets in front of them
    // without a code change. A malformed manifest is reported and then ignored
    // wholesale rather than half-applied, because a half-applied rebinding table
    // is a scene where some art moved and some did not.
    std::string sprite_manifest_error;
    SpriteManifest sprites = load_sprite_manifest("assets/sprites.txt", &sprite_manifest_error);
    if (!sprite_manifest_error.empty()) {
        std::fprintf(stderr, "WARNING: %s\n", sprite_manifest_error.c_str());
        std::fprintf(stderr, "         every sprite falls back to its shipped file.\n");
    }

    // The parallax factors are in the generated render/backdrop_layers.h, written
    // by `python tools/generate_backdrop.py --header` from the same table that
    // sizes these images. What is left here is the loading.
    frame::Backdrop backdrop;
    backdrop.sky = load_art_texture(
        renderer, sprites.path_for("backdrop_sky", "backdrop_sky.bmp").c_str(), false);
    backdrop.mountains = load_art_texture(
        renderer, sprites.path_for("backdrop_mountains", "backdrop_mountains.bmp").c_str(), true);
    // The ground plane loads with no colour key: it is an opaque surface rather
    // than a silhouette, so there is nothing in it to key out and a key would
    // punch holes in the plane wherever the ramp landed on the key colour.
    backdrop.ground = load_art_texture(
        renderer, sprites.path_for("backdrop_ground", "backdrop_ground.bmp").c_str(), false);

    if (backdrop.sky)
        SDL_QueryTexture(backdrop.sky, nullptr, nullptr, &backdrop.sky_w, &backdrop.sky_h);
    if (backdrop.mountains)
        SDL_QueryTexture(backdrop.mountains, nullptr, nullptr,
                         &backdrop.mountain_w, &backdrop.mountain_h);
    if (backdrop.ground)
        SDL_QueryTexture(backdrop.ground, nullptr, nullptr,
                         &backdrop.ground_w, &backdrop.ground_h);

    // The seam at the pan limit, turned into a printed line. A backdrop layer has
    // to be large enough to cover the window plus the camera's whole pan range at
    // that layer's factor; if it is not, the layer runs out of image before the
    // world runs out of ground and an edge of raw clear colour appears -- at the
    // far edge of the map, which is the last place anybody looks.
    //
    // The generated header carries the size the generator would produce, so the
    // disagreement is checkable, and this says so at startup rather than at the
    // map's edge.
    //
    // A warning and not a failure: an undersized backdrop is a cosmetic defect at
    // one extreme of the world, and refusing to launch over it would be worse
    // than the seam. A line here means the art and the header disagree, which is
    // either a stale BMP (rerun the generator) or a hand-edited header.
    auto check_layer_size = [](const char* name, SDL_Texture* tex, int w, int h,
                               const backdrop_layers::Layer& expected) {
        if (!tex) return;
        if (w < expected.width || h < expected.height) {
            std::fprintf(stderr,
                         "WARNING: backdrop %s is %dx%d but parallax %.2f/%.2f needs at "
                         "least %dx%d - expect a seam at the pan limit.\n"
                         "         rerun: python tools/generate_backdrop.py\n",
                         name, w, h, expected.parallax_x, expected.parallax_y,
                         expected.width, expected.height);
        }
    };
    check_layer_size("sky", backdrop.sky, backdrop.sky_w, backdrop.sky_h,
                     backdrop_layers::SKY);
    check_layer_size("mountains", backdrop.mountains, backdrop.mountain_w,
                     backdrop.mountain_h, backdrop_layers::MOUNTAINS);
    // The ground plane is checked by the same lambda and the number means
    // something different for it: a wrapping layer cannot have a seam at the pan
    // limit. For a tile, undersized means the texture repeats sooner than the art
    // was drawn for, and the vertical half is sharper still -- the tile's rows are
    // the plane's depth, so a short tile is a plane missing part of its recession.
    check_layer_size("ground", backdrop.ground, backdrop.ground_w,
                     backdrop.ground_h, backdrop_layers::GROUND);

    // The same tile again, on the CPU, reduced to one colour per row.
    // render/surface_plane.cpp blends the near terrain toward the plane's own
    // value at its own depth, and it cannot read an SDL_Texture, so the BMP is
    // read a second time through bmp::read.
    //
    // Read again rather than kept from the texture load, which is a deliberate
    // small waste: load_art_texture returns a texture and throws the pixels away,
    // and threading a copy out of it would put a second output parameter on a
    // function four other layers call and do not want.
    //
    // A failure here is not fatal: ground_rows stays empty, TileRows::count is 0,
    // and the pass copies the window through unchanged. The warning is printed
    // because a silently-disabled depth pass is the kind of thing that gets
    // noticed six sessions later.
    std::vector<uint8_t> ground_rows;
    {
        bmp::Image ground_img;
        std::string err;
        const std::string path = sprites.path_for("backdrop_ground", "backdrop_ground.bmp");
        if (bmp::read(path.c_str(), ground_img, &err)) {
            surface_plane::average_rows(ground_img.pixels.data(), ground_img.width,
                                        ground_img.height, ground_rows);
        } else {
            std::printf("WARNING: ground plane rows unavailable (%s) - "
                        "the near terrain will not take the plane's value\n", err.c_str());
        }
    }

    // The `ground` row's grade, read out of frame.cpp's table rather than written
    // here a second time. The blend has to match the plane as composited, so it
    // needs this number; the table stays the one place it is decided.
    frame::Grade ground_grade{};
    for (int i = 0; i < frame::LAYER_COUNT; ++i) {
        if (std::string(frame::LAYERS[i].name) == "ground") {
            ground_grade = frame::LAYERS[i].grade;
            break;
        }
    }

    // Scratch for the surface-plane pass. Resized at the upload site rather than
    // sized here, because `mode` is not fixed for the run -- the settings menu
    // switches resolutions and apply_mode rebuilds every target when it does. A
    // buffer sized once at boot survives every test and fails on the one machine
    // whose owner opens the menu.
    std::vector<uint32_t> cell_window;
    std::vector<int> plane_src_row_for;
    std::vector<int> plane_row_scale;
    std::vector<int> cell_depth;

    // Props: sprites from tools/generate_props.py, positioned by a prop list
    // rather than by a list in this file, so a second scene can have props at
    // all. See src/scene/props.h for why the format is a text list and not a
    // second BMP.
    //
    // One texture per distinct sprite name, not one per record: nine trees are
    // three images, and the cache is what keeps that true as a scene grows. Keyed
    // by name so prop_textures is also the destroy list at shutdown.
    std::vector<std::pair<std::string, SDL_Texture*>> prop_textures;
    auto prop_texture = [&](const std::string& sprite) -> SDL_Texture* {
        for (auto& entry : prop_textures)
            if (entry.first == sprite) return entry.second;
        // A prop's name is already its own key, so the manifest can rebind one
        // without the scene file changing: the record still says `tree_a`, and
        // which drawing that is becomes a swap you can make from the command
        // line.
        const std::string path = sprites.path_for(sprite, sprite + ".bmp");
        SDL_Texture* tex = load_art_texture(renderer, path.c_str(), true);
        if (!tex) {
            std::fprintf(stderr, "WARNING: prop sprite '%s' did not load; "
                                 "every prop naming '%s' is skipped.\n",
                         path.c_str(), sprite.c_str());
        }
        prop_textures.emplace_back(sprite, tex);
        return tex;
    };

    // The player sheet, from tools/player_sheet.py. Colour-keyed like the props,
    // and like them one BMP pixel is one world cell. Every number about the
    // sheet's layout comes from the generated header rather than being retyped
    // here.
    //
    // The sprite is deliberately larger than the collision box, and the generated
    // offsets are the whole of that decoupling: the box stays the size every
    // movement constant is tuned to, and the larger frame is drawn anchored to the
    // box's bottom-centre, so the head overhangs upward into space that collides
    // with nothing and the sleeves hang outside the box's width. A sleeve visually
    // overlapping a wall is correct: it is art, not body.
    //
    // The aiming arm is not drawn, which is why nothing here reads a hotspot.
    // Bringing it back costs the hotspot image and a rotate-about-the-shoulder
    // draw.
    //
    // The path is a manifest lookup and the layout is not. Which BMP this is can
    // be changed from the command line (tools/load_sprite.py); how many frames it
    // holds and what they mean cannot, because that is the animation table in
    // tools/player_sheet.py and the header it generates. So the one thing worth
    // checking here is that the image fits the layout the code is compiled
    // against -- a sheet with the wrong frame size loads fine and draws fine, it
    // just draws the wrong rectangles.
    const std::string player_sheet_path =
        sprites.path_for("player_sheet", "player_sheet_fly.bmp");
    SDL_Texture* player_tex = load_art_texture(renderer, player_sheet_path.c_str(), true);
    if (player_tex) {
        int sheet_w = 0, sheet_h = 0;
        SDL_QueryTexture(player_tex, nullptr, nullptr, &sheet_w, &sheet_h);
        const int need_w = player_sprite::SHEET_COLS * player_sprite::FRAME_W;
        const int need_h = player_sprite::SHEET_ROWS * player_sprite::FRAME_H;
        if (sheet_w != need_w || sheet_h != need_h) {
            std::fprintf(stderr,
                         "WARNING: %s is %dx%d, but the animation table expects %dx%d "
                         "(%d cols x %d rows of %dx%d).\n",
                         player_sheet_path.c_str(), sheet_w, sheet_h, need_w, need_h,
                         player_sprite::SHEET_COLS, player_sprite::SHEET_ROWS,
                         player_sprite::FRAME_W, player_sprite::FRAME_H);
            std::fprintf(stderr,
                         "         The figure will draw sliced. Either the sheet is the "
                         "wrong art, or ANIMATIONS in tools/player_sheet.py needs "
                         "updating and --header re-running.\n");
        }
    }
    player_anim::State anim_state;

    // The enemies' bodies, one slot per Run::enemies slot, redrawn every frame
    // from each body's surviving pixels in its current pose. A slot is the largest
    // species' frame plus the most any pose reaches outside it (Enemy::MAX_POSE_PAD)
    // on every side -- a troll's club raised over its head is well above its
    // frame -- so any slot can hold any species in any pose. The slots sit in a
    // grid rather than one row, which at that size would be wider than the
    // smallest texture limit SDL promises.
    //
    // Built rather than loaded, because there is no fixed picture to load: an enemy
    // looks like whatever is left of it, posed however it is standing, and both
    // are simulation state (see physics/enemy_art.h and physics/rig.h). Every
    // frame rather than on change, and only the rectangle each pose covers is
    // written and uploaded: a few thousand pixels a body, against a cache keyed
    // on which body is in which slot in which pose, which is a correctness
    // problem in exchange for nothing measurable.
    constexpr int ENEMY_SLOT_W = Enemy::MAX_FRAME_W + 2 * Enemy::MAX_POSE_PAD;
    constexpr int ENEMY_SLOT_H = Enemy::MAX_FRAME_H + 2 * Enemy::MAX_POSE_PAD;
    constexpr int ENEMY_SLOT_COLS = 6;
    constexpr int ENEMY_SLOT_ROWS = (Run::MAX_ENEMIES + ENEMY_SLOT_COLS - 1) / ENEMY_SLOT_COLS;
    constexpr int ENEMY_ATLAS_W = ENEMY_SLOT_W * ENEMY_SLOT_COLS;
    constexpr int ENEMY_ATLAS_H = ENEMY_SLOT_H * ENEMY_SLOT_ROWS;
    static_assert(ENEMY_ATLAS_W <= 2048 && ENEMY_ATLAS_H <= 2048,
                  "the enemy atlas outgrew the texture size every SDL renderer supports");
    SDL_Texture* enemy_atlas = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                                 SDL_TEXTUREACCESS_STREAMING,
                                                 ENEMY_ATLAS_W, ENEMY_ATLAS_H);
    if (enemy_atlas) {
        SDL_SetTextureBlendMode(enemy_atlas, SDL_BLENDMODE_BLEND);
    } else {
        // Loud, not fatal: the enemies are still simulated and can still be shot,
        // they are just invisible -- which is worse than a missing sprite and is
        // why this says so rather than falling back to nothing quietly.
        std::fprintf(stderr, "WARNING: could not create the enemy texture (%s); "
                             "enemies will not be drawn.\n", SDL_GetError());
    }
    std::vector<uint32_t> enemy_atlas_pixels(
        static_cast<size_t>(ENEMY_ATLAS_W) * ENEMY_ATLAS_H, 0u);
    std::vector<frame::EnemySprite> enemy_sprites;
    std::vector<frame::ArrowSprite> arrow_sprites;
    enemy_sprites.reserve(Run::MAX_ENEMIES);
    arrow_sprites.reserve(Quiver::CAPACITY);

    // Which way the figure faces. Tracked here rather than on Player because
    // Player is simulation and this is presentation: a facing flag on the body
    // would be state the determinism tests would have to account for. Sampled off
    // the same key state the input struct is built from, and sticky -- it holds
    // the last direction actually pressed, so a player standing still keeps
    // facing where they were going rather than snapping to a default.
    bool facing_left = false;

    // Which scenes exist, loaded before anything that depends on which one is
    // active. The format's reasoning is in scene/scene_list.h.
    //
    // A malformed list falls back to the built-in default rather than refusing to
    // start: the failure is loud on stderr, and a game that will not boot because
    // a debug convenience was mistyped is worse than one that boots into the
    // location it shipped with. The fallback lives in scene_list.cpp so that "the
    // file is missing" and "the file lists exactly the shipped location" are
    // provably the same world.
    std::string scene_list_error;
    std::vector<scene_list::SceneDef> scenes =
        scene_list::load_scene_list("assets/scenes.txt", &scene_list_error);
    if (!scene_list_error.empty()) {
        std::fprintf(stderr, "ERROR: %s\n", scene_list_error.c_str());
        std::fprintf(stderr, "       Falling back to the built-in scene.\n");
    }
    if (scenes.empty()) scenes = scene_list::default_scene_list();
    int active_scene = 0;

    // The prop list is per scene, so these are not const. The texture cache above
    // is keyed by sprite name and is also the destroy list, so two scenes naming
    // the same tree share one texture and shutdown is unchanged -- which is why
    // switching scenes needs no texture bookkeeping of its own.
    //
    // A malformed prop list is loud and costs every prop, not the bad line. The
    // alternative -- skip the row that would not parse -- is a scene that renders,
    // renders wrong, and says nothing. props.h has the argument in full.
    std::vector<PropDef> prop_defs;
    std::vector<SDL_Texture*> prop_tex;
    std::vector<int> prop_w;
    std::vector<int> prop_h;
    auto load_props_for = [&](const scene_list::SceneDef& def) {
        prop_defs.clear();
        if (!def.props.empty()) {
            std::string prop_error;
            prop_defs = load_prop_list("assets/" + def.props, &prop_error);
            if (!prop_error.empty()) {
                std::fprintf(stderr, "ERROR: %s\n", prop_error.c_str());
                std::fprintf(stderr, "       No props are drawn. Fix the line above and re-run.\n");
            }
        }
        // One entry per record, parallel to prop_defs, holding what only a window
        // can answer: the sprite's texture and its native size in world cells. A
        // record whose sprite did not load keeps its slot with a null texture and
        // a width of 0, which is how boot::plant_props -- which knows no SDL -- is
        // told about it without being handed one.
        prop_tex.assign(prop_defs.size(), nullptr);
        prop_w.assign(prop_defs.size(), 0);
        prop_h.assign(prop_defs.size(), 0);
        for (size_t i = 0; i < prop_defs.size(); ++i) {
            SDL_Texture* tex = prop_texture(prop_defs[i].sprite);
            if (!tex) continue;  // already warned by prop_texture, once per name
            prop_tex[i] = tex;
            SDL_QueryTexture(tex, nullptr, nullptr, &prop_w[i], &prop_h[i]);
        }
    };
    // The count is NOT printed here. A prop that has a texture is not yet a prop
    // that got placed -- the terrain scan below drops any with no ground under them
    // -- so a count taken at this point reports texture loads while saying
    // "placed". See below.
    std::vector<Prop> props;

    // The reticle is the cursor, so the OS one would be a second pointer sitting
    // on top of it. SDL scopes this to its own window rather than globally, so the
    // desktop pointer is untouched the moment the mouse leaves.
    SDL_ShowCursor(SDL_DISABLE);

    // The only nondeterministic line in the project, and it is here rather than
    // inside Grid on purpose: the simulation is a pure function of its seed, and
    // exactly one place gets to choose that seed. Two draws because
    // std::random_device yields 32 bits at a time.
    //
    // Printed because a seed you cannot read is only half of determinism -- this
    // is the number that turns "it collapsed weirdly" into something that can be
    // reproduced.
    std::random_device rd;
    const uint64_t world_seed = (static_cast<uint64_t>(rd()) << 32) | rd();
    std::printf("World seed: %llu\n", static_cast<unsigned long long>(world_seed));

    Run run(GRID_WIDTH, GRID_HEIGHT, world_seed);  // starts fully Empty, player mid-air
                                                  // - boot::stand_player_on_ground below
                                                  // is what puts it down, once there is
                                                  // terrain to put it on
    
    // The world's size and paradigm are variables, not the two constants at the
    // top of this file. boot::GRID_WIDTH/HEIGHT are still the engine default and
    // still what Run is built with; a scene row may replace them, and everything
    // downstream of the camera has to be stated against these rather than against
    // the constants. A site left reading GRID_WIDTH after a scene of a different
    // size loads either clamps the camera to the wrong rectangle or uploads pixels
    // from outside the grid.
    int world_w = GRID_WIDTH;
    int world_h = GRID_HEIGHT;
    bool world_infinite = false;
    // --- the session recorder ---
    //
    // Recording is always on, and F9 saves what has been recorded so far. The
    // alternative -- F9 starts recording -- does not work: a log has to begin at a
    // world state the replay can rebuild, and the only such state is the one right
    // here, before the first step. A recording started two minutes in would replay
    // from the scene as loaded into inputs that assume two minutes of dug tunnels
    // and poured water.
    //
    // The cost of always-on is one Input per fixed step and no work per step beyond
    // the copy. The cap below is what keeps that a fact rather than a hope; it stops
    // recording rather than dropping the oldest steps, because a log missing its
    // middle is not a session and there is no honest way to replay one.
    input_log::Log recording;
    recording.header.grid_w = GRID_WIDTH;
    recording.header.grid_h = GRID_HEIGHT;
    recording.header.seed = world_seed;
    // scene_cells and the start fingerprint are filled after the world is built,
    // not here. The world is put together by activate_scene below, and a header
    // written before it would describe a world that does not exist yet -- which is
    // precisely the "log that replays into the wrong world" failure the
    // fingerprint exists to catch.
    constexpr size_t MAX_RECORDED_STEPS = 60 * 60 * 30;  // half an hour of play
    bool recording_full = false;

    // Shown under the HUD for a few seconds after F9, because a save that reports
    // only on stdout is a save a player in a fullscreen window cannot see happen.
    std::string record_notice;
    double record_notice_timer = 0.0;
    int saved_logs = 0;

    // Plant each prop on the terrain that is actually under it, rather than on a
    // hardcoded ground line. The scan is boot::plant_props; it runs here, after the
    // scene is stamped and before the first frame, because props are not simulated
    // and terrain that moves later must not drag them with it.
    //
    // What is left in this file is turning the report into draw records and saying
    // out loud what was dropped -- the two things that need a texture and a stderr
    // respectively.
    auto plant_props_now = [&](const scene_list::SceneDef& def) {
        props.clear();
        const boot::PlantingReport planting =
            boot::plant_props(run.grid, prop_defs, prop_w);
        for (int i : planting.no_ground) {
            std::fprintf(stderr, "WARNING: assets/%s: prop at x=%.1f has no ground "
                                 "under it and is not drawn.\n",
                         def.props.c_str(), prop_defs[i].x);
        }
        props.reserve(planting.planted.size());
        for (const boot::Planted& p : planting.planted) {
            props.push_back(Prop{ prop_tex[p.def_index], prop_w[p.def_index], prop_h[p.def_index],
                                  prop_defs[p.def_index].x, static_cast<float>(p.anchor_y) });
        }
        // Printed after planting, because this is the line the launch check reads
        // and it has to count props that will actually be drawn. Printed
        // immediately after the textures load instead -- before either way a prop
        // can be dropped -- a run with an unplantable prop reports every prop
        // placed on stdout while warning on stderr that one was not drawn. A
        // check that asserts the wrong thing fails silently, because it passes.
        // Both drops are in the count: a sprite that would not load, and a prop
        // with no ground under it.
        std::printf("Props: %d of %d placed\n", static_cast<int>(props.size()),
                    static_cast<int>(prop_defs.size()));
    };

    // Centred, and deliberately not configurable from here. A moving vertical
    // anchor was built and removed at the tester's request, and the whole mechanism
    // went with it, so there is nothing left to hold.
    Camera camera;

    // Declared above the scene lambdas rather than below them, and that is a
    // requirement rather than a tidy-up: they capture by reference, so
    // activate_scene can only tell the camera a scene's scale, and
    // load_bg1_layers can only read it back, if the camera is already in scope
    // where those lambdas are written.

    // --- the authored backdrop stack --------------------------
    //
    // frame::Backdrop::layers is owned here and by nothing else, so every path that
    // changes scene goes through these two. The clear destroys before it empties,
    // which is the whole reason it is a function: cycling scenes with a
    // layers.clear() that only dropped the pointers would leak nine textures a
    // cycle with nothing on screen ever saying so.
    auto clear_custom_layers = [&]() {
        for (frame::ParallaxLayer& l : backdrop.layers)
            if (l.texture) SDL_DestroyTexture(l.texture);
        backdrop.layers.clear();
        backdrop.rig_on = false;
    };

    // An authored backdrop set: nine images at the set's own native size, listed
    // back to front. bg1::SETS is where a scene name picks one.
    //
    // The factors are the art's own, from art_src/Background_1/README.md, and they
    // are carried in bg1_backdrop.h rather than in backdrop_layers.h because that
    // header is generated from the tool that derives factors and sizes together.
    // This art arrived with its factors already chosen by whoever painted it;
    // there is nothing to derive, so there is nothing to generate. An extended set
    // keeps those same factors -- it is the same nine depths of the same place, so
    // a different ladder would make it a different landscape in the same palette.
    //
    // Everything else about this table is argued at the table itself.
    auto load_bg1_layers = [&](const bg1::Set& set) {
        // The stack, its nine parallax factors and the argument for every one of
        // them are the set's rows in render/bg1_backdrop.h. Nothing about them is
        // restated here; what is left below is the loading.
        //
        // Which stack is a parameter rather than a name compared in here: two
        // authored sets read by one loader cannot drift apart, and a chain of
        // `if (def.name == ...)` is how one world's images end up over another
        // world's terrain while compiling perfectly. bg1::find is the only place
        // a scene name is matched to a stack.
        const int NATIVE_W = set.native_w, NATIVE_H = set.native_h;

        // One art pixel is one world cell, so the on-screen size of a layer is
        // the world's size in screen pixels and nothing else -- no fitting, no
        // deriving from the window. Computing a scale here from the window height
        // is sizing art to a window rather than to a world; the window's job is
        // to be a view into this, which is what the scene's `scale` column buys.
        //
        // Read off the camera rather than off def.scale so that a scene whose
        // requested scale could not be applied draws its backdrop at the scale
        // actually in use, instead of at the one it asked for and did not get.
        const int scale = camera.scale();
        const int layer_w = NATIVE_W * scale;
        const int layer_h = NATIVE_H * scale;

        clear_custom_layers();
        backdrop.layers.reserve(static_cast<size_t>(set.layer_count));
        int missing = 0;
        for (int i = 0; i < set.layer_count; ++i) {
            const bg1::Layer& sp = set.layers[i];
            const std::string path = std::string(set.dir) + sp.file;
            SDL_Texture* tex = load_art_texture(renderer, path.c_str(), !sp.opaque);
            if (!tex) { ++missing; continue; }
            frame::ParallaxLayer l;
            l.texture = tex;
            l.w = layer_w;
            l.h = layer_h;
            l.parallax_x = sp.parallax_x;
            // One shared plane, and there is no per-layer vertical factor to read --
            // the argument is at bg1::Layer::parallax_x.
            l.parallax_y = 1.0f;
            l.tex_h = NATIVE_H;
            if (sp.banded)
                l.bands.assign(set.bands, set.bands + set.band_count);
            l.grade = frame::Grade{};  // identity; the art carries its own depth
            l.is_foreground = sp.is_foreground;
            backdrop.layers.push_back(l);
        }
        // Printed for the same reason Scene: and Props: are -- a backdrop that
        // half-loaded is otherwise a frame you have to recognise by eye, and
        // layers.empty() is the switch that silently restores the generated
        // three, so a total failure looks like the old backdrop working rather
        // than like the new one missing.
        std::printf("Backdrop: %d of %d %s layers at %dx (%dx%d px over a %dx%d world)\n",
                    static_cast<int>(backdrop.layers.size()), set.layer_count,
                    set.scene, scale, layer_w, layer_h, world_w, world_h);
        if (missing)
            std::fprintf(stderr, "WARNING: %d %s layer(s) failed to load - rerun that "
                                 "set's generator (tools/convert_background_layers.py "
                                 "for bg1, tools/generate_bg1_ext.py for bg1_ext) and "
                                 "rebuild.\n",
                         missing, set.scene);
    };

    // A perspective-rig set (render/rig_backdrop.h). The loading is load_bg1_layers'
    // -- native-size textures stretched by the scale, colour-keyed unless opaque --
    // and what differs is only what is read off each row: the factor is derived
    // from the layer's feet by the rig, the vertical factor from that, and the
    // plane, ripple and drift flags are carried through for draw_rig_layer.
    auto load_rig_layers = [&](const rig_backdrop::Set& set) {
        const int scale = camera.scale();
        clear_custom_layers();
        backdrop.layers.reserve(static_cast<size_t>(set.layer_count));
        int missing = 0;
        for (int i = 0; i < set.layer_count; ++i) {
            const rig_backdrop::Layer& sp = set.layers[i];
            const std::string path = std::string(set.dir) + sp.file;
            SDL_Texture* tex = load_art_texture(renderer, path.c_str(), !sp.opaque);
            if (!tex) { ++missing; continue; }
            frame::ParallaxLayer l;
            l.texture = tex;
            l.w = set.native_w * scale;
            l.h = set.native_h * scale;
            l.tex_h = set.native_h;
            l.parallax_x = rig_backdrop::factor_of(set, sp);
            l.parallax_y = depth_rig::vertical_factor(set.rig, l.parallax_x);
            l.line_scroll = sp.line_scroll;
            l.on_plane = sp.on_plane;
            l.ripple_row0 = sp.ripple_row0;
            l.ripple_row1 = sp.ripple_row1;
            l.drift = sp.drift;
            l.grade = frame::Grade{};
            l.is_foreground = sp.is_foreground;
            backdrop.layers.push_back(l);
        }
        backdrop.rig_on = true;
        backdrop.rig = set.rig;
        backdrop.ripple_amplitude = set.ripple_amplitude;
        std::printf("Backdrop: %d of %d %s layers at %dx, perspective rig "
                    "(horizon row %d, contact row %d)\n",
                    static_cast<int>(backdrop.layers.size()), set.layer_count, set.scene,
                    scale, set.rig.horizon_row, set.rig.contact_row);
        if (missing)
            std::fprintf(stderr, "WARNING: %d %s layer(s) failed to load - rerun "
                                 "python tools/generate_bg_tarn.py and rebuild.\n",
                         missing, set.scene);
    };

    // Everything building a world is, in the order it has to happen, and the only
    // caller of the steps above. Boot calls it once, the scene key calls it to
    // switch scenes, and restart_run calls it on a win or a loss. There is
    // deliberately no second path that puts a world together.
    //
    // The order is not arbitrary. The grid is wiped before it is stamped, stamped
    // before anything scans it for a surface, and the body is standing before the
    // camera is asked where to look. The objective is cleared inside its own step
    // rather than here, because "the level changed" is the fact that clears it and
    // this is the only place that fact exists.
    auto activate_scene = [&](int index) {
        if (index < 0 || index >= static_cast<int>(scenes.size())) return;
        active_scene = index;
        const scene_list::SceneDef& def = scenes[static_cast<size_t>(index)];

        // The files first, because a scene that states no size is the material
        // BMP's size, and that cannot be known until the BMP is open. The loader
        // is shared with the replay bench (scene/level_files.h), so the two read a
        // scene the same way.
        const level_files::Loaded loaded = level_files::load(def, "assets/", level::is_species);
        for (const std::string& e : loaded.errors) std::fprintf(stderr, "ERROR: %s\n", e.c_str());
        for (const std::string& w : loaded.warnings) std::fprintf(stderr, "WARNING: %s\n", w.c_str());
        const scene_activation::Resolved resolved =
            scene_activation::resolve(def, loaded.scene.width, loaded.scene.height,
                                      GRID_WIDTH, GRID_HEIGHT, view_scale);
        world_infinite = resolved.infinite;
        world_w = resolved.world_w;
        world_h = resolved.world_h;

        // The scene's screen scale, applied before anything measured in cells.
        // Both the render targets and the camera's viewport are functions of it,
        // so it has to move first or they are rebuilt to the previous scene's
        // framing.
        //
        // The rebuild is skipped when the scale did not move -- that is
        // Resolved::scale_changed. A failure here is reported and then ignored:
        // the old targets are untouched on failure, so the scene still loads,
        // framed at the previous scale. That is wrong-looking and playable, which
        // beats a black window.
        if (resolved.scale_changed) {
            const int previous = view_scale;
            view_scale = resolved.scale;
            if (!apply_mode(window, renderer, mode, view_scale, targets)) {
                std::fprintf(stderr, "WARNING: could not rebuild render targets at %dx "
                                     "for scene '%s'; staying at %dx.\n",
                             view_scale, def.name.c_str(), previous);
                view_scale = previous;
            }
            camera.set_scale(view_scale);
            std::printf("Scale: %dx (%dx%d cells)\n", view_scale,
                        mode.viewport_w(view_scale), mode.viewport_h(view_scale));
        }

        // After the size and the scale, and before anything is stamped: the stack
        // is drawn in screen pixels, which the line above has just settled, and
        // nothing below this reads the backdrop.
        if (const bg1::Set* set = bg1::find(def.name.c_str())) load_bg1_layers(*set);
        else if (const rig_backdrop::Set* rs = rig_backdrop::find(def.name.c_str()))
            load_rig_layers(*rs);
        else clear_custom_layers();

        // The world itself: one call, shared with the replay bench and the tests,
        // so there is no second path that puts a world together. See game/level.h
        // for what it does and why it is not here.
        const level::Report built = level::start(run, def, loaded.scene, loaded.level, world_seed);
        for (const level::Line& line : level::describe(built, def)) {
            if (line.warning) std::fprintf(stderr, "WARNING: %s\n", line.text.c_str());
            else std::printf("%s\n", line.text.c_str());
        }

        // Props after the world, because they are planted on the terrain it
        // stamped. Render-only, so not part of what level::start rebuilds.
        load_props_for(def);
        plant_props_now(def);

        // The world is new, so a log of inputs into the old one is not a log of
        // anything.
        recording.steps.clear();
        // Re-stated here as well as at boot, because a scene may resize the grid.
        // A header naming the old dimensions would describe a world the log
        // cannot replay into.
        recording.header.grid_w = run.grid.get_width();
        recording.header.grid_h = run.grid.get_height();
        recording.header.scene = def.name;
        recording.header.scene_cells = built.scene_cells;
        recording.header.start_fingerprint = input_log::fingerprint(run);
        recording_full = false;
    };
    activate_scene(active_scene);

    bool running = true;
    SDL_Event e;

    ElementType current_brush = ElementType::Sand;
    int brush_size = 3;
    // The one-shot the next step takes (N, T), held until a step runs.
    Command pending_command;

    // The settings menu is a state, not an overlay with a flag: while it is open
    // the fixed-step loop below does not run, so the world is frozen rather than
    // continuing to simulate behind a screen the player cannot act through. It
    // still renders, because a settings screen over a black void gives no way to
    // judge a resolution change against the thing being resized.
    //
    // ESC opens this rather than quitting: a key that ends the session without
    // confirmation is the wrong key to leave next to a menu, so quitting is an item
    // in the menu, where it takes two deliberate presses.
    enum class Screen { Playing, Settings };
    Screen screen = Screen::Playing;

    // The navigation and selection live in game/settings_menu.h and have a suite.
    // What is left here is the keysyms and the two calls that need a window. The
    // cursor stays in a local because the drawing code below reads it.
    menu::State menu_state;
    menu::open(menu_state, mode_index);
    int& menu_cursor = menu_state.cursor;

    // Shown under the menu for a few seconds after a switch is attempted, so a
    // refused mode says so instead of looking like a dead key.
    std::string menu_notice;
    double menu_notice_timer = 0.0;

    uint64_t prev_counter = SDL_GetPerformanceCounter();
    const double counter_freq = static_cast<double>(SDL_GetPerformanceFrequency());

    // The accumulator, the freeze rule and the interpolation alpha live in
    // game/pacer.h. Reading the clock stays here; deciding what the elapsed seconds
    // mean does not.
    pacer::Pacer frame_pacer;

    int frames_this_second = 0;
    double title_timer = 0.0;
    int fps_display = 0;

    // Rebuilt every frame rather than cached for a second. The brush name and the
    // awake-chunk count are not periodic readouts like the frame rate, they are
    // answers to "what did that key just do" and "has the world settled yet" --
    // cached, pressing a hotbar key leaves the HUD naming the old brush for up to a
    // second, which reads as the input being sluggish. Only the frame rate is
    // genuinely a once-a-second quantity, so only the frame rate is cached.
    //
    // This is an instrument, and an instrument that lags makes every measurement
    // taken with it suspect.
    std::string hud_text;

    // Previous frame's sub-cell player position, for render interpolation. The
    // simulation steps at 60 Hz and the display may run far faster, so each
    // simulated position is shown for several frames and the motion steps visibly
    // even once the sub-cell remainder is accounted for. Updated inside the step
    // loop so it always holds the state one step behind the current one --
    // including on frames where no step runs at all, which is most of them at a
    // high refresh rate.
    float prev_player_x = run.player.visual_x();
    float prev_player_y = run.player.visual_y();

    // --- starting the run over ---
    //
    // One path, Run::reset(seed), and not a second set of code that puts things
    // back. A win and a loss both come here, and so does the debug reset.
    //
    // The scene has to be re-stamped because Run::reset wipes the grid -- the run
    // does not own the level, main.cpp does -- and the objective has to be
    // re-derived because the terrain it was scanned off has just been rebuilt.
    //
    // The recording starts over too, and that is what keeps the replay guarantee
    // intact. A session log replays by rebuilding the world from the seed and the
    // scene and replaying the inputs into it; a log that spanned a reset would
    // replay into a world several minutes of play deep, and the bench could not tell
    // that from a stale log. Resetting on the same seed and re-stamping the same
    // scene reproduces the world the recording started in exactly. saved_logs is
    // deliberately not reset, so a second save still writes a new file.
    auto restart_run = [&]() {
        // Delegated rather than duplicated. Re-stamping the scene and re-placing
        // the objective here would be the same lines activate_scene runs, minus
        // the spawn -- so a restart would drop the body back into mid-air and let
        // it fall, which is exactly what boot::stand_player_on_ground removes at
        // launch. Two paths is how that gets fixed in one of them.
        activate_scene(active_scene);

        // The body is somewhere else entirely now, so last step's drawn position
        // is not something to ease away from. The interpolation clamp below would
        // catch this on its own; setting them is the honest version of relying on
        // that.
        prev_player_x = run.player.visual_x();
        prev_player_y = run.player.visual_y();
        anim_state = player_anim::State{};
        frame_pacer.accumulator = 0.0;
    };

    // --- the debug tooling ---------------------------------------------
    //
    // The state and every decision in it are in game/debug_view.h, which is SDL-free
    // and has a suite; what is left here is the key bindings and the drawing.
    DebugView debug;

    // Exactly one fixed step, and the only place one happens.
    //
    // Extracted so that the pause's single-step and the frame pacer run the same
    // code rather than two copies of it. A second copy under the step key is how the
    // recorder ends up fed by one path and not the other, and a session log missing
    // the steps taken while paused is not a session: it replays into a world those
    // steps had changed.
    //
    // The accumulator is deliberately not touched here. Time is the pacer's
    // business, and a single-step is a step that no time was spent on.
    auto advance_one_step = [&](const Input& step_input) {
        prev_player_x = run.player.visual_x();
        prev_player_y = run.player.visual_y();

        // Captured here, inside the one place a step happens, and this placement
        // is the whole of what makes the log replayable. One record per step,
        // never per rendered frame: the same session played at any frame rate
        // produces the same list of records, because there is no sampling left in
        // it. Recording in the input-building block instead would store one
        // record per frame and put the frame rate back into the measurement.
        if (recording.steps.size() < MAX_RECORDED_STEPS) {
            recording.steps.push_back(step_input);
        } else if (!recording_full) {
            recording_full = true;
            std::fprintf(stderr, "Session recording stopped at %d steps (half an hour); "
                                 "F9 still writes what was recorded up to that point.\n",
                         static_cast<int>(MAX_RECORDED_STEPS));
        }

        run.step(step_input);

        // The animation clock. Advanced here, inside the one place a step
        // happens, and nowhere else -- see the timing note at the top of
        // render/player_anim.h. Driving it from the render loop would make the
        // walk cycle's speed a function of frame rate, which would present as an
        // art problem.
        player_anim::Conditions cond;
        cond.on_ground = run.player.is_on_ground();
        // != 0 rather than an epsilon, which is correct rather than merely tidy:
        // horizontal velocity is exactly zero or exactly +/-MOVE_SPEED, with no
        // float noise for an epsilon to absorb.
        cond.moving = run.player.velocity_x() != 0;
        cond.vel_y = run.player.velocity_y();  // sign only; see Conditions
        // Read off the tool rather than off the step's return value. Taking the
        // one step a dig landed on and restarting the swing there pins the figure
        // on frame 0 whenever the button is held, because the tool lands digs
        // faster than the swing's frames can play.
        //
        // The swing is a duration the simulation owns and this only reports where
        // in it the tool is. The direction still holds: rendering must not drive
        // simulation, and this is a read, on the fixed step, of a value the tool
        // would have computed with no window attached.
        cond.dig_progress = run.dig_tool.swing_progress();
        cond.flapped = run.player.flapped();
        player_anim::update(anim_state, cond, 1);
    };

    while (running) {
        while (SDL_PollEvent(&e) != 0) {
            if (e.type == SDL_QUIT) {
                running = false;
            }
            else if (screen == Screen::Settings && e.type == SDL_KEYDOWN) {
                // Keyboard-driven rather than click-driven, which is a scope decision:
                // this project has no widget layer, no hit-testing and no focus
                // model, and adding three of them to change a resolution would be a
                // larger feature than the one being built.
                //
                // This switch is only the binding -- which keysyms mean which of the
                // menu's four verbs. Every branch that would otherwise follow is
                // menu::key in game/settings_menu.h, where shell_test can reach it.
                menu::Key mk = menu::Key::Back;
                bool bound = true;
                switch (e.key.keysym.sym) {
                    case SDLK_UP:
                    case SDLK_w: mk = menu::Key::Prev; break;
                    case SDLK_DOWN:
                    case SDLK_s: mk = menu::Key::Next; break;
                    case SDLK_RETURN:
                    case SDLK_KP_ENTER:
                    case SDLK_SPACE: mk = menu::Key::Confirm; break;
                    case SDLK_ESCAPE: mk = menu::Key::Back; break;
                    default: bound = false; break;
                }
                if (bound) {
                    menu::Outcome out = menu::key(menu_state, mk, mode_available,
                                                  DISPLAY_MODE_COUNT, mode_index);
                    // The one thing the state machine cannot do for itself: try the
                    // switch, on a window, and report which way it went.
                    if (out.act == menu::Act::ApplyMode) {
                        const DisplayMode& wanted = DISPLAY_MODES[out.mode];
                        if (apply_mode(window, renderer, wanted, view_scale, targets)) {
                            mode = wanted;
                            mode_index = out.mode;
                            // Persisted at the point it takes effect, not on the way out
                            // of the menu: a crash between the two would otherwise lose a
                            // setting the player watched succeed.
                            out = menu::mode_applied(menu_state, out.mode,
                                                     save_display_mode(mode));
                        } else {
                            out = menu::mode_refused(menu_state, mode_index);
                        }
                    }
                    if (out.notice) {
                        menu_notice = out.notice;
                        menu_notice_timer = out.notice_seconds;
                    }
                    if (out.act == menu::Act::Close) screen = Screen::Playing;
                    else if (out.act == menu::Act::Quit) running = false;
                }
            }
            else if (e.type == SDL_MOUSEWHEEL) {
                brush_size += e.wheel.y;
                if (brush_size < 1) brush_size = 1;
                if (brush_size > 32) brush_size = 32;
            }
            else if (e.type == SDL_KEYDOWN) {
                // The material keys are a loop over ui::HOTBAR rather than a switch
                // for one reason: the row drawn at the bottom of the screen has to
                // be telling the truth about what each key does, and the only way to
                // guarantee that is for the binding and the icon to come out of the
                // same table.
                for (int i = 0; i < ui::HOTBAR_COUNT; ++i) {
                    if (e.key.keysym.sym == ui::HOTBAR[i].key) {
                        current_brush = ui::HOTBAR[i].type;
                        break;
                    }
                }
                if (e.key.keysym.sym == SDLK_ESCAPE) {
                    screen = Screen::Settings;
                    menu::open(menu_state, mode_index);
                }
                // Only while the run is over, so R is inert during play rather than a
                // key that throws a session away by mis-hitting it -- the same
                // argument that moved quitting off ESC and into the settings menu.
                //
                // The debug reset works mid-run, which is exactly the mis-hit R
                // refuses, so it takes a modifier: Ctrl+R is not a key anybody reaches
                // for while aiming for sand, and the two meanings stay apart without R
                // acquiring a second one.
                //
                // else if, because with the run over both branches match and a reset
                // that ran twice would be invisible -- the second one produces exactly
                // the world the first one did.
                //
                // Held keys repeat, and every binding below except the step key is a
                // toggle or a one-shot. SDL sends a KEYDOWN per repeat, so without this
                // a held pause key flickers at the OS repeat rate and a held reset
                // rebuilds the world dozens of times a second -- both of which read as
                // the key not working rather than as working too well. The step key is
                // the exception and wants the repeats: stepping at the repeat rate is
                // how you scrub through a collapse.
                const bool repeat = e.key.repeat != 0;
                const bool ctrl_held = (e.key.keysym.mod & KMOD_CTRL) != 0;
                if (e.key.keysym.sym == SDLK_r && ctrl_held && !repeat) {
                    // The same seed, not a fresh one, for two reasons. A debugging
                    // session is worth nothing if the world it is being debugged in
                    // changes underneath it. And the recorder's header seed is written
                    // once at startup, so a reset onto a new seed would silently make
                    // every session saved afterwards replay into the wrong world -- a
                    // log that is wrong rather than absent.
                    restart_run();
                    record_notice = "WORLD RESET";
                    record_notice_timer = 2.0;
                } else if (e.key.keysym.sym == SDLK_r && !repeat &&
                           run.outcome() != Run::Outcome::Playing) {
                    restart_run();
                }

                // --- pause, single-step, the free camera, the inspector ---
                // An enemy at the cursor. A development key like the brush, and a
                // request rather than a call on the Run: the spawn changes the world,
                // so it has to arrive through Input and be in the recording. See the
                // step loop below for why it is consumed by the first step only.
                if (e.key.keysym.sym == SDLK_n && !repeat) pending_command = Command::spawn(species::GHOUL);
                // A troll at the cursor, standing on it -- point at the ground.
                if (e.key.keysym.sym == SDLK_t && !repeat) pending_command = Command::spawn(species::TROLL);

                if (e.key.keysym.sym == SDLK_p && !repeat) debug.toggle_pause();
                if (e.key.keysym.sym == SDLK_PERIOD) debug.request_single_step();
                if (e.key.keysym.sym == SDLK_i && !repeat) debug.inspector = !debug.inspector;
                if (e.key.keysym.sym == SDLK_f && !repeat) {
                    if (debug.free_camera) {
                        debug.attach_camera();
                    } else {
                        // Handed the player's centre rather than the camera's own, which are
                        // the same view: Camera::follow was given this exact number last
                        // frame and clamped it, and detach_camera clamps it the same way.
                        // Reading it back off the camera would mean deriving a centre from a
                        // clamped view, which is the one arithmetic here that could put the
                        // view somewhere it was not.
                        debug.detach_camera(static_cast<float>(run.player.center_x()),
                                            static_cast<float>(run.player.center_y()),
                                            mode.padded_w(view_scale), mode.padded_h(view_scale),
                                            world_w, world_h);
                    }
                }
                // Cycles to the next scene in assets/scenes.txt. A function key
                // because every letter near the movement keys is a hotbar slot, and a
                // key that rebuilds the world is a worse thing to hit while reaching
                // for sand than one that writes a file.
                //
                // Cycle rather than a key per scene, because the list is authored and
                // a binding per row would be a fixed number of keys pretending to be
                // a variable-length list. One key, and the scene name is printed and
                // shown on the HUD so the cycle is never something to guess at.
                //
                // It does nothing when the list holds one scene. Silence is right
                // there: nothing changed, so nothing is reported.
                if (e.key.keysym.sym == SDLK_F7 && scenes.size() > 1) {
                    activate_scene((active_scene + 1) % static_cast<int>(scenes.size()));

                    // The body is somewhere else entirely, so last frame's drawn
                    // position is not something to ease away from.
                    prev_player_x = run.player.visual_x();
                    prev_player_y = run.player.visual_y();
                    anim_state = player_anim::State{};
                    frame_pacer.accumulator = 0.0;

                    // A detached camera is looking at a world that no longer exists.
                    // Re-attaching is the honest answer: re-aiming it at the same
                    // coordinates would be pointing a debug view at whatever happens to
                    // be at those cells now.
                    if (debug.free_camera) debug.attach_camera();

                    record_notice = "SCENE  " + scenes[static_cast<size_t>(active_scene)].name;
                    record_notice_timer = 2.0;
                }
                // Writes everything played so far to a session log. Deliberately not
                // bound to a letter: every letter within reach of the movement keys is
                // a hotbar slot, and a key that saves a file is a bad thing to hit
                // while reaching for sand.
                if (e.key.keysym.sym == SDLK_F9) {
                    // The end state is captured at the moment of writing, not at the end
                    // of the session, because this is the end of the recording being
                    // written -- the replay has to check against the world the last
                    // recorded step produced.
                    recording.header.end_fingerprint = input_log::fingerprint(run);
                    recording.header.end_player_x = run.player.cell_x();
                    recording.header.end_player_y = run.player.cell_y();

                    // A second save in one session does not overwrite the first: two
                    // takes of a session are two measurements, and the interesting one
                    // is often the earlier.
                    const std::string path = saved_logs == 0
                        ? std::string("session.rec")
                        : "session_" + std::to_string(saved_logs + 1) + ".rec";

                    std::string log_error;
                    if (input_log::write(path.c_str(), recording, &log_error)) {
                        saved_logs++;
                        record_notice = "SAVED " + path + "  " +
                                        std::to_string(recording.steps.size()) + " STEPS";
                        std::printf("Recorded session written to %s: %d steps, seed %llu\n",
                                    path.c_str(), static_cast<int>(recording.steps.size()),
                                    static_cast<unsigned long long>(world_seed));
                    } else {
                        record_notice = "COULD NOT SAVE THE SESSION LOG";
                        std::fprintf(stderr, "ERROR: %s\n", log_error.c_str());
                    }
                    record_notice_timer = 4.0;
                }
            }
        }

        // Above the camera, because the free camera pans in real time and has to
        // have panned before the view is aimed -- otherwise the pan lands a frame
        // late and the mouse-to-world conversion below resolves the cursor against
        // last frame's view, which is a cell of aiming error at every pan speed and
        // several at a fast one.
        const uint64_t now_counter = SDL_GetPerformanceCounter();
        double frame_time = static_cast<double>(now_counter - prev_counter) / counter_freq;
        prev_counter = now_counter;
        frame_time = pacer::clamp_frame_time(frame_time);

        if (menu_notice_timer > 0.0) menu_notice_timer -= frame_time;
        if (record_notice_timer > 0.0) record_notice_timer -= frame_time;

        // Movement is read from live key state rather than key events, so holding a
        // key keeps moving instead of firing once and repeating on the OS key-repeat
        // delay. Sampled here, once, and used for both the free camera's pan and the
        // body's input below.
        const uint8_t* keys = SDL_GetKeyboardState(nullptr);

        // --- panning the free camera ---
        //
        // The same keys that move the body, because while the camera is detached the
        // body is not being driven. Two sets of movement keys, one of them dead
        // depending on a mode, is how you end up pressing the wrong one for a whole
        // session.
        //
        // Real seconds rather than fixed steps: the camera is presentation, it moves
        // while the world is paused, and a pan measured in simulated time would stop
        // dead exactly when you paused to look at something.
        if (debug.free_camera && screen == Screen::Playing) {
            float pan_x = 0.0f, pan_y = 0.0f;
            if (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])  pan_x -= 1.0f;
            if (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) pan_x += 1.0f;
            if (keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP])    pan_y -= 1.0f;
            if (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])  pan_y += 1.0f;
            const bool fast = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT];
            const float speed = DebugView::PAN_CELLS_PER_SECOND *
                                (fast ? DebugView::PAN_FAST_MULTIPLIER : 1.0f) *
                                static_cast<float>(frame_time);
            debug.pan(pan_x * speed, pan_y * speed, mode.padded_w(view_scale), mode.padded_h(view_scale),
                      world_w, world_h);
        }

        // The viewport follows the player, clamped at the world's edges. Recomputed
        // once per rendered frame, same as the mouse and keyboard samples below it.
        //
        // Or follows the free camera, which is the whole of what detaching means:
        // Camera is unchanged and still owns every conversion and the clamp, it is
        // simply handed a different centre.
        if (debug.free_camera) {
            // The free camera stays clamped even in an infinite scene, and that is
            // not an oversight: debug.pan already clamps its own centre to the world
            // rect, so an unclamped follow here would disagree with the thing feeding
            // it. This camera is for inspecting the world that exists.
            camera.follow(debug.cam_x, debug.cam_y,
                          mode.padded_w(view_scale), mode.padded_h(view_scale), world_w, world_h);
        } else {
            camera.follow_mode(static_cast<float>(run.player.center_x()), static_cast<float>(run.player.center_y()),
                               mode.padded_w(view_scale), mode.padded_h(view_scale), world_w, world_h, world_infinite);
        }

        // Handle continuous mouse pressing.
        int mouseX, mouseY;
        const uint32_t mouseState = SDL_GetMouseState(&mouseX, &mouseY);

        const int gridX = camera.screen_to_world_x(mouseX);
        const int gridY = camera.screen_to_world_y(mouseY);

        // Sampled once per rendered frame: a real mouse and keyboard cannot be
        // sampled at the simulation's fixed rate, since there is no such thing as
        // "the input for a step that has not happened yet". Every fixed step this
        // frame accumulates gets its own call to run.step() with this sample,
        // rather than the brush being painted once up here before the loop starts.
        //
        // The free camera drives the pan keys instead of the body, and the
        // suppression happens here rather than inside Run, for two reasons. The
        // body genuinely stands still while you look around, rather than walking
        // off the far side of the world unwatched. And what the recorder stores is
        // what the simulation was given, so a session with debug camera work in it
        // still replays exactly -- suppressing after the record would write a log
        // of keys the world never saw.
        Input input;
        input.left  = !debug.free_camera && (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT]);
        input.right = !debug.free_camera && (keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]);
        input.jump  = !debug.free_camera &&
                      (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]);
        // Sticky facing, updated only when a direction is actually held. Both keys
        // down at once keeps the current facing rather than picking one, which
        // matches what the body does -- Player cancels the two against each other
        // and stands still.
        if (input.left != input.right) facing_left = input.left;
        input.cursor_x = gridX;
        input.cursor_y = gridY;
        input.dig   = (mouseState & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0;
        // Right-click, not left. Digging is the game's action and gets the primary
        // button; the material brush is a development tool and moved out of its way.
        input.brush_active = (mouseState & SDL_BUTTON(SDL_BUTTON_RIGHT)) != 0;
        input.brush_type = current_brush;
        input.brush_size = brush_size;
        // The bow on E: held, it looses at the cursor on the bow's own draw. A key
        // rather than a mouse button because both buttons are taken -- the dig on
        // the left and the brush on the right -- and E sits under the finger that
        // is already on W.
        input.shoot = !debug.free_camera && keys[SDL_SCANCODE_E];
        input.command = pending_command;

        // The camera is centred and stays centred; nothing per-frame is left to do to
        // it besides the follow further down.
        //
        // Worth keeping the note a removed moving anchor leaves behind: it was updated
        // after the cursor was resolved, on purpose, so that the aim was read through
        // the frame the player was actually looking at. Any later feature that moves
        // the view from player input inherits that ordering problem and the positive
        // feedback loop behind it.

        // The freeze rule is pacer::world_advances, and it is one function because it
        // is one mechanism. The argument -- that freezing means not accumulating time,
        // never skipping the step loop with the accumulator still filling -- is written
        // at the function rather than here, because that is where the next caller will
        // read it. The rejected version banks every frozen second and spends it in one
        // catch-up burst at the stall clamp, which matters most for a finished run,
        // where the last thing that happened is the thing the player has to look at.
        const bool run_over = run.outcome() != Run::Outcome::Playing;
        const int steps = frame_pacer.steps(
            frame_time,
            pacer::world_advances(screen == Screen::Settings, run_over, debug.paused));
        // A command is a one-shot carried on a per-frame sample, so the first step
        // that runs takes it and the rest of this frame's steps run without it. A
        // frame that runs no steps -- paused, or in the menu -- keeps it for the
        // next frame that does, rather than losing the keypress.
        auto consume_spawn = [&]() {
            input.command = Command{};
            pending_command = Command{};
        };
        for (int i = 0; i < steps; ++i) {
            advance_one_step(input);
            consume_spawn();
        }

        // The single-step. Outside the pacer's loop and spending none of its time,
        // which is what makes it advance the world by exactly one step and leave the
        // accumulator where the pause left it.
        while (debug.consume_single_step()) {
            advance_one_step(input);
            consume_spawn();
        }

        // Where between the last two simulated states this frame falls, and the
        // teleport clamp that stops resolve_overlap's several-cell shove being eased
        // across. Both are game/pacer.h. prev_* still lives outside the loop, which is
        // what lets alpha keep climbing on the frames that buy no step at all.
        const float alpha = frame_pacer.alpha(debug.paused);
        const pacer::Interpolated draw_player =
            pacer::interpolate(prev_player_x, prev_player_y,
                               run.player.visual_x(), run.player.visual_y(), alpha);
        float draw_player_x = draw_player.x;
        float draw_player_y = draw_player.y;

        // Re-aimed at the interpolated position before drawing. The call above is
        // what the mouse-to-world conversion needed and runs before the step; this
        // one is what the render needs, and using the pre-step camera for it would
        // reintroduce a whole frame of lag between the player and the world.
        //
        // The detached camera has nothing to re-aim -- it is not tracking anything
        // that moved -- so this correction is the attached case's, and applying it to
        // a free camera would drag the view towards the player it was detached from.
        if (!debug.free_camera) {
            // follow_mode and not follow, for the reason stated at VERTICAL_ANCHOR:
            // this is the second of the two aiming calls in one rendered frame, and
            // a framing rule applied at one of them and not the other tears the
            // backdrop against the world every frame.
            camera.follow_mode(draw_player_x + Player::WIDTH / 2.0f, draw_player_y + Player::HEIGHT / 2.0f,
                               mode.padded_w(view_scale), mode.padded_h(view_scale), world_w, world_h, world_infinite);
        }

        // Upload only the visible rect, not the whole grid, starting from the
        // camera's current view rather than always (0, 0). Clamped to the grid's own
        // size so this stays correct if the grid is ever smaller than the viewport;
        // the case this exists for is the opposite one, where the clamp is a no-op
        // and the rect is the full viewport every frame.
        //
        // There is a viewport-sized copy between the grid and the texture, and it is
        // not free. A zero-copy upload works by handing SDL the grid's own buffer
        // with the grid's pitch, so SDL reads the right columns out of each row and
        // skips the rest -- but surface_plane::apply has to change some of those
        // pixels, because the near terrain has to get brighter than it is and a
        // colour multiply can only darken. The zero-copy path is still reachable and
        // still used: a window row that is not on the plane is a straight copy, and
        // when the tile failed to load the whole pass is one.
        const std::vector<uint32_t>& pixels = run.grid.get_pixels();
        const int visible_w = std::min(mode.padded_w(view_scale), run.grid.get_width());
        const int visible_h = std::min(mode.padded_h(view_scale), run.grid.get_height());
        const SDL_Rect visible_rect{0, 0, visible_w, visible_h};

        // Which tile row each window row of cells is looking at, or -1 for the rows
        // that are not on the plane at all.
        //
        // The near edge clamps to the tile's last row rather than falling off to -1,
        // which is the same choice frame.cpp makes in its fill below the plane's near
        // edge: what is past the near end of a receding plane is ground nearer still.
        // Dropping to -1 here would put a horizontal line across the terrain at
        // whatever row the plane's near edge happened to reach.
        const backdrop_wrap::Plane plane = backdrop_wrap::plane_geometry(
            frame::ground_horizon_y(camera, backdrop.mountain_h),
            backdrop.ground_h,
            backdrop_layers::GROUND.parallax_x,
            backdrop_layers::GROUND_NEAR_X);
        const float band = plane.bottom_y - plane.horizon_y;
        plane_src_row_for.assign(static_cast<size_t>(visible_h), -1);
        plane_row_scale.assign(static_cast<size_t>(visible_h), 0);

        // The near-ground pass, currently off. Leaving every row at -1 is the pass's
        // own "no tile" path, so apply degrades to a straight copy -- no second code
        // path, and turning it back on is this one constant.
        //
        // Why it is off: at full strength the pass does not tint a cell, it replaces
        // it. weight_at_depth returns 255 for every cell a few rows below the air in
        // its own column, and blend(from, to, 255) is `to` exactly -- so on any row
        // below the horizon, all matter under a thin top crust is painted the ground
        // tile's row colour, which is the same colour for every column in that row.
        // Material placed or fallen below the horizon is then invisible, because it is
        // being drawn in the backdrop's own colour. On a scene whose subject is
        // scenery that reads as recession; on one whose subject is the material the
        // player puts down, it erases the subject.
        //
        // The finding that has to survive into whatever replaces it: the ramp needs a
        // floor that keeps material identity, or a bound that separates world terrain
        // from placed material, before full strength can mean 255 again.
        constexpr bool PLANE_ON_NEAR_TERRAIN = false;

        for (int wy = 0; PLANE_ON_NEAR_TERRAIN && wy < visible_h; ++wy) {
            // The centre of the cell row, in screen pixels, including the camera's
            // sub-cell remainder -- the same shift draw_cells applies to the texture
            // this feeds. Sampling the top edge instead would bias every row half a
            // cell toward the horizon.
            const float screen_y =
                (static_cast<float>(wy) + 0.5f - camera.frac_y()) * static_cast<float>(view_scale);
            if (band <= 0.0f || screen_y < plane.horizon_y) continue;
            const float t = (screen_y - plane.horizon_y) / band;
            const int scale = surface_plane::row_scale_at(t);
            if (scale <= 0) continue;
            int row = backdrop.ground_h - 1;
            if (t < 1.0f) {
                row = static_cast<int>(backdrop_wrap::plane_src_at(plane, t) + 0.5f);
                if (row < 0) row = 0;
                if (row > backdrop.ground_h - 1) row = backdrop.ground_h - 1;
            }
            plane_src_row_for[static_cast<size_t>(wy)] = row;
            plane_row_scale[static_cast<size_t>(wy)] = scale;
        }

        // With the pass off, every row is a straight copy, and a straight copy is what
        // SDL_UpdateTexture already does given the grid's own pitch. So the upload
        // reads the grid directly and skips both apply's copy and its depth_map, which
        // runs over the whole window unconditionally and whose output only the
        // (unreachable) blend path reads. Only when the window lies wholly inside the
        // grid, though: apply is what clears rows past the grid's edge, and a scene
        // with the horizontal clamp dropped can put the window there. Turning the pass
        // back on is still the one constant above.
        const int grid_w = run.grid.get_width();
        const int view_x = camera.view_x(), view_y = camera.view_y();
        const bool window_inside_grid = view_x >= 0 && view_y >= 0 &&
                                        view_x + visible_w <= grid_w &&
                                        view_y + visible_h <= run.grid.get_height();
        if (!PLANE_ON_NEAR_TERRAIN && window_inside_grid) {
            SDL_UpdateTexture(targets.cells, &visible_rect,
                              pixels.data() + static_cast<size_t>(view_y) * grid_w + view_x,
                              grid_w * static_cast<int>(sizeof(uint32_t)));
        } else {
            const size_t window_cells =
                static_cast<size_t>(visible_w) * static_cast<size_t>(visible_h);
            if (cell_window.size() != window_cells) cell_window.assign(window_cells, 0u);
            if (cell_depth.size() != window_cells) cell_depth.assign(window_cells, -1);

            const surface_plane::View view{view_x, view_y, visible_w, visible_h};
            const surface_plane::TileRows tile_rows{
                ground_rows.empty() ? nullptr : ground_rows.data(),
                static_cast<int>(ground_rows.size() / 3)
            };
            surface_plane::apply(pixels.data(), grid_w, run.grid.get_height(), view,
                                 tile_rows, plane_src_row_for.data(), plane_row_scale.data(),
                                 ground_grade.r, ground_grade.g, ground_grade.b,
                                 cell_depth.data(), cell_window.data());
            SDL_UpdateTexture(targets.cells, &visible_rect, cell_window.data(),
                              visible_w * static_cast<int>(sizeof(uint32_t)));
        }

        // Computed against the same view origin the cell upload just used, and after
        // camera.follow for the same reason that upload is: a light field built from
        // last frame's view would slide against the world it is lighting.
        //
        // Recomputed from scratch every frame rather than carried between them. Not
        // cheap: grid_bench's light/fire row measured ~15 ms (91% of a 60 Hz frame)
        // before the propagate sweep was threaded and ~4.3 ms (26%) after, on a
        // 20-thread machine; a dark view is ~0.6 ms. Still the right trade, because
        // the alternative is a cache keyed on both the camera and every temperature in
        // view -- a correctness problem. If this needs to get cheaper, the sweep's
        // iteration count is the lever, not a cache.
        targets.light.update(run.grid, camera.view_x(), camera.view_y());
        if (targets.light.any_light()) {
            SDL_UpdateTexture(targets.light_texture, nullptr, targets.light.pixels().data(),
                              targets.light.cols() * sizeof(uint32_t));
        }

        // --- the world layers ---
        //
        // Everything from the clear to the light pass lives in render/frame.cpp, and
        // golden_frame_test checksums the result. What stays below this call is UI,
        // and that split is not filing: the light pass is the last thing in the world,
        // and anything drawn after it is deliberately not lit.
        // --- the enemies and the arrows, at this frame's alpha ---
        //
        // Interpolated between the last two steps like the body is, and with the
        // same teleport clamp for the enemies -- climbing out of its own dust can
        // lift one a few cells in a step. Not for the arrows: they cover several
        // cells every step by design, and the clamp would make every arrow in
        // flight strobe.
        enemy_sprites.clear();
        for (int slot = 0; slot < Run::MAX_ENEMIES; ++slot) {
            const Enemy& en = run.enemies[static_cast<size_t>(slot)];
            if (!en.is_alive()) continue;
            const Species& kind = en.species();
            const body_art::Art& art = *kind.art;

            // The slam's telegraph: the eyes heat from their own colour toward
            // white-hot over the wind-up, so the thing about to land is readable
            // from the face, which is where the player is aiming anyway. Drawn
            // here, from the windup count the body exposes, and nowhere in the
            // simulation -- it changes nothing an arrow can hit.
            uint32_t eye_heat = 0;
            if (en.windup_left() > 0 && kind.windup_steps > 0) {
                eye_heat = static_cast<uint32_t>(
                    255 * (kind.windup_steps - en.windup_left()) / kind.windup_steps);
            }
            // The pose's rectangle, painted cell by cell from the same question an
            // arrow asks (Enemy::posed_pixel), so what is drawn in a cell is what
            // is hit there. Whatever the pose cannot reach is left as it was:
            // nothing samples outside the rectangle.
            const rig::Box b = en.pose_bounds();
            const int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
            if (bw <= 0 || bh <= 0) continue;
            const int slot_x = (slot % ENEMY_SLOT_COLS) * ENEMY_SLOT_W;
            const int slot_y = (slot / ENEMY_SLOT_COLS) * ENEMY_SLOT_H;
            for (int y = 0; y < bh; ++y) {
                uint32_t* row = enemy_atlas_pixels.data() +
                                static_cast<size_t>(slot_y + y) * ENEMY_ATLAS_W + slot_x;
                for (int x = 0; x < bw; ++x) {
                    const int index = en.posed_pixel(b.x0 + x, b.y0 + y);
                    uint32_t c = 0u;
                    if (index >= 0) {
                        const int ax = index % art.w, ay = index / art.w;
                        c = art.color_at(ax, ay);
                        if (eye_heat > 0 && art.is_head(ax, ay)) {
                            const uint32_t g = (c >> 8) & 0xFFu, bl = c & 0xFFu;
                            c = 0xFFFF0000u |
                                ((g + (255u - g) * eye_heat / 255u) << 8) |
                                (bl + (255u - bl) * eye_heat / 255u);
                        }
                    }
                    row[x] = c;
                }
            }
            const SDL_Rect src{slot_x, slot_y, bw, bh};
            if (enemy_atlas) {
                SDL_UpdateTexture(enemy_atlas, &src,
                                  enemy_atlas_pixels.data() +
                                      static_cast<size_t>(slot_y) * ENEMY_ATLAS_W + slot_x,
                                  ENEMY_ATLAS_W * static_cast<int>(sizeof(uint32_t)));
            }
            const pacer::Interpolated at = pacer::interpolate(
                static_cast<float>(en.prev_cell_x()) + fx::to_float(en.prev_remainder_x()),
                static_cast<float>(en.prev_cell_y()) + fx::to_float(en.prev_remainder_y()),
                static_cast<float>(en.cell_x()) + fx::to_float(en.remainder_x()),
                static_cast<float>(en.cell_y()) + fx::to_float(en.remainder_y()), alpha);
            // The rectangle's own anchor. Posed column x lands on frame column x
            // facing right and on (w-1-x) facing left, and SDL's flip mirrors the
            // rectangle about its own middle -- so facing left, the rectangle's
            // left edge in the frame is w - x1, not x0. This is Enemy::pixel_at's
            // flip written for a rectangle; the two must agree or what is drawn is
            // a cell off what is hit.
            const int left_in_frame = en.facing_left() ? art.w - b.x1 : b.x0;
            enemy_sprites.push_back(frame::EnemySprite{
                src, at.x, at.y, kind.offset_x() - left_in_frame, kind.offset_y() - b.y0,
                en.facing_left()});
        }

        arrow_sprites.clear();
        for (const Arrow& a : run.quiver.arrows()) {
            if (!a.live) continue;
            const float px = static_cast<float>(a.prev_x) + fx::to_float(a.prev_rem_x);
            const float py = static_cast<float>(a.prev_y) + fx::to_float(a.prev_rem_y);
            const float nx = static_cast<float>(a.x) + fx::to_float(a.rem_x);
            const float ny = static_cast<float>(a.y) + fx::to_float(a.rem_y);
            const float vx = fx::to_float(a.vel_x), vy = fx::to_float(a.vel_y);
            const float len = std::sqrt(vx * vx + vy * vy);
            arrow_sprites.push_back(frame::ArrowSprite{
                px + (nx - px) * alpha, py + (ny - py) * alpha,
                len > 0.0f ? vx / len : 0.0f, len > 0.0f ? vy / len : 0.0f});
        }

        frame::Params fp;
        fp.camera = &camera;
        fp.padded_w = mode.padded_w(view_scale);
        fp.padded_h = mode.padded_h(view_scale);
        fp.is_infinite = world_infinite;
        fp.world_w = world_w;
        fp.world_h = world_h;
        fp.backdrop = backdrop;
        fp.props = &props;
        fp.cells = targets.cells;
        fp.has_objective = run.has_objective();
        fp.objective_x = run.objective_x();
        fp.objective_y = run.objective_y();
        fp.player_tex = player_tex;
        fp.player_x = draw_player_x;
        fp.player_y = draw_player_y;
        fp.facing_left = facing_left;
        fp.sheet_col = anim_state.sheet_col();
        fp.sheet_row = anim_state.sheet_row();
        fp.player_box_w = Player::WIDTH;
        fp.player_box_h = Player::HEIGHT;
        fp.enemy_atlas = enemy_atlas;
        fp.enemies = &enemy_sprites;
        fp.arrows = &arrow_sprites;
        fp.light = &targets.light;
        fp.light_texture = targets.light_texture;

        // The rig's anchor is where the camera sits with the player standing on the
        // contact row: there the stack is exactly the painting. Per frame and not
        // at load, because it depends on the viewport, which the display mode can
        // change under a loaded scene. Horizontally the world's centre -- every rig
        // layer wraps, so this only chooses which columns line up where.
        if (fp.backdrop.rig_on) {
            fp.backdrop.rig_anchor_x =
                0.5f * static_cast<float>(std::max(0, world_w - fp.padded_w));
            fp.backdrop.rig_anchor_y = depth_rig::standing_anchor_y(
                fp.backdrop.rig, Player::HEIGHT, fp.padded_h, Camera::VERTICAL_ANCHOR, world_h);
        }
        // Wall clock, for drifting clouds and rippling water. Render-only; see
        // Params::time_s. Not wrapped: any wrap period that is not a whole number of
        // cloud tiles makes the clouds jump at the wrap, and a float second count
        // still resolves a millisecond after a day of play.
        fp.time_s = static_cast<float>(static_cast<double>(SDL_GetTicks64()) / 1000.0);
        frame::compose(renderer, fp);

        // --- the screen-space layer ---
        //
        // Everything drawn after frame::compose is UI, and all of it lives in
        // render/overlay.cpp behind one call. What stays here is the half that reads
        // simulation state -- whether the cursor is in reach, what the readout says,
        // which lines go under it, which hotbar slot the brush is in -- because
        // nothing under src/render/ may ask a Run or a Grid anything.
        const int dx_cells = gridX - run.player.center_x();
        const int dy_cells = gridY - run.player.center_y();

        overlay::Params op;
        op.window_w = mode.window_w;
        op.window_h = mode.window_h;
        op.ui_scale = mode.ui_scale();
        op.view_scale = view_scale;

        // Only while the pointer is actually over this window. SDL_GetMouseState
        // keeps reporting the last position inside the window after the mouse
        // leaves, so without this the reticle sticks to the edge and reads as a
        // frozen UI element rather than as a cursor that has gone elsewhere.
        op.show_reticle = (SDL_GetMouseFocus() == window);
        op.mouse_x = mouseX;
        op.mouse_y = mouseY;
        op.in_range =
            (dx_cells * dx_cells + dy_cells * dy_cells) <= DigTool::RANGE * DigTool::RANGE;

        hud_text = "FPS:" + std::to_string(fps_display) +
                   " BRUSH:" + material_of(current_brush).name + "(" + std::to_string(brush_size) + ")" +
                   " CHUNKS:" + std::to_string(run.grid.active_chunk_count());
        // CHUNKS:0 does not mean the world has stopped, and this suffix is the
        // correction arriving on screen. A falling structural piece is carried by the
        // support queue rather than by the chunk rects, so a slab can fall the height
        // of the world with this counter reading zero the whole way. Shown as a flag
        // rather than a count because the queue's length is a number of seeds, not of
        // pieces, and would read as far more work than it is.
        if (run.grid.has_pending_support_checks()) hud_text += "+FALLING";

        // --- the run's readout ---
        //
        // HP goes first, ahead of the three diagnostics. The rest of this line is an
        // instrument for whoever is building the engine; this is the one thing on it a
        // player is playing against, and reading it should not mean scanning past a
        // frame rate.
        //
        // GOAL is a bearing, and it is text on the line that already exists rather
        // than an arrow at the screen edge. The objective starts well off-screen, so
        // without this the run is "walk east until you find it", which is not a
        // difficulty but a missing instrument. Distance is to the body's centre, in
        // cells.
        std::string status = "HP:" + std::to_string(run.player.health());
        if (run.has_objective()) {
            const int gdx = run.objective_x() - run.player.center_x();
            const int gdy = run.objective_y() - run.player.center_y();
            const int gdist = static_cast<int>(std::sqrt(
                static_cast<double>(gdx) * gdx + static_cast<double>(gdy) * gdy));
            status += "  GOAL:" + std::to_string(gdist) + (gdx < 0 ? "W" : "E");
        }
        // The enemies' line: how many are left and how many are down. Beside HP
        // because it is the other number the player is playing against.
        status += "  FOES:" + std::to_string(run.enemies_alive()) +
                  "  KILLS:" + std::to_string(run.kills());
        op.hud_text = status + "  " + hud_text;

        // The lines under the readout, pushed in the order they are drawn. A list
        // rather than three calls at three sites, for the reason the cursor in
        // overlay.cpp exists: all of these are conditional, and
        // independently-computed offsets are how two end up drawn on top of each
        // other in whichever combination nobody tried.

        // The recorder's line, drawn only while it has something to say. A permanent
        // REC indicator is worse: this records every session, so an always-on marker
        // would be furniture within a minute and invisible by the time it mattered.
        if (record_notice_timer > 0.0 && !record_notice.empty()) {
            op.hud_lines.push_back({record_notice, 0xFFFFC080});
        }

        // --- the debug lines ---
        //
        // The mode line first, because it is the one that explains why the game is not
        // responding the way it usually does, and a player who has hit the pause key
        // by accident has to be told before anything else.
        {
            const std::string modes = debug_status(debug);
            if (!modes.empty()) op.hud_lines.push_back({modes, 0xFF80D0FF});
        }
        if (debug.inspector) {
            // Reads the cell the cursor names, which the free camera can put outside
            // the world -- describe_cell says so rather than clamping, because "there
            // is no cell there" and "there is an empty cell there" are different
            // answers, and a debug tool that conflates them is the same failure as a
            // legend resolving an unknown colour to Empty.
            op.hud_lines.push_back({describe_cell(run.grid, gridX, gridY), 0xFFE0E0E0});
        }

        // --- the material hotbar ---
        //
        // The selected slot is looked up rather than stored, so current_brush stays
        // the single fact about what is selected. A second index kept beside it is how
        // a highlight ends up on the wrong box after some later code path sets the
        // brush without going through a key.
        op.hotbar_selected = -1;
        for (int i = 0; i < ui::HOTBAR_COUNT; ++i) {
            if (ui::HOTBAR[i].type == current_brush) op.hotbar_selected = i;
        }

        op.run_over = run_over;
        op.won = run.outcome() == Run::Outcome::Won;

        op.settings_open = (screen == Screen::Settings);
        op.cursor = menu_cursor;
        op.mode_count = DISPLAY_MODE_COUNT;
        op.current_mode = mode_index;
        op.modes = DISPLAY_MODES;
        op.available = mode_available;
        // An expired notice is handed over as no notice at all. The timer is a fact
        // about this loop, not about the drawing, and passing it would put a second
        // copy of "is it still showing" on the far side of the seam.
        if (menu_notice_timer > 0.0) op.notice = menu_notice;

        overlay::draw(renderer, op);

        SDL_RenderPresent(renderer);

        // Surface the frame rate so performance regressions are visible while working.
        frames_this_second++;
        title_timer += frame_time;
        if (title_timer >= 1.0) {
            // Only the frame rate is cached -- it is a rate, and a rate needs an
            // interval to be measured over. The brush name and the awake-chunk count
            // are read straight from live state where the string is built above;
            // awake chunks in particular are shown because they explain the frame
            // rate, and a count that lags by a second cannot.
            fps_display = frames_this_second;
            frames_this_second = 0;
            title_timer = 0.0;
        }
    }

    if (player_tex) SDL_DestroyTexture(player_tex);
    if (enemy_atlas) SDL_DestroyTexture(enemy_atlas);
    // The cache is the destroy list -- one entry per distinct sprite name, so a
    // scene with fifty trees of three kinds still frees exactly three textures and
    // `props` holding several borrowed copies of each is not a double free.
    for (auto& entry : prop_textures)
        if (entry.second) SDL_DestroyTexture(entry.second);
    clear_custom_layers();
    if (backdrop.ground) SDL_DestroyTexture(backdrop.ground);
    if (backdrop.mountains) SDL_DestroyTexture(backdrop.mountains);
    if (backdrop.sky) SDL_DestroyTexture(backdrop.sky);
    SDL_DestroyTexture(targets.light_texture);
    SDL_DestroyTexture(targets.cells);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
