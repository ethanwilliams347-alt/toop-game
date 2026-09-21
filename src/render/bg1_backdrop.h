#pragma once

#include "render/backdrop_wrap.h"

// The band table for `bg1_08_ground`, the one layer of the art_src/Background_1
// set that is a surface rather than an object.
//
// A header and not three literals in main.cpp because it knows no SDL, so
// boot_test can include it and check every number against the BMP it describes.
// That is the only enforcement available here: no headless suite composes an
// authored frame, and the defect these numbers fix is one a person has to see.
//
// --- why the plane needs banding --------------------------------------------
//
// A single factor on a receding surface is the defect, not the value of the
// factor. The plane's authored range is 0.28x-0.52x while the hills standing on
// it run 0.20 to 0.70, so one factor slips the hills' feet against the ground
// under them by a tenth of the scene's width over the world's travel.
//
// --- why it is fixable ------------------------------------------------------
//
// bg1_08_ground is uniform across all 344 columns on 61 of its 81 opaque rows.
// The only horizontal detail is three wavy shore contours, and only two have
// anything standing on them:
//
//     rows  70..77   shore 1   hills_midfar's foot lands at art row 73 (0.30)
//     rows  87..97   shore 2   hills_near's   foot lands at art row 89 (0.70)
//     rows 102..106  shore 3   nothing stands on it
//
// So the plane is cut into three bands, each carrying one contour at the factor
// of whatever stands on it, and every cut is placed inside a run of rows that is
// uniform in colour -- 78..86 and 98..101 both are. A band boundary is a
// discontinuity in scroll offset; put it where both sides are the same flat
// colour and it is invisible at every camera position. That is why the
// boundaries are 82 and 100 rather than round numbers, and it is the one thing
// here boot_test can check.
//
// --- why not a smooth ramp --------------------------------------------------
//
// The contacts demand about 0.025 of factor per art row between rows 73 and 89,
// so a contour ten rows thick would have its top and bottom scrolling 0.25
// apart, and the shorelines would tear sideways. The generated plane ramps
// smoothly because its texture is noise with no feature wide enough to shear;
// this one is paint.
//
// The third band is 1.00 because the near field is where the foreground rocks
// and the diggable terrain are, and both are locked to the world. Same cap as
// the horizontal one: draw_backdrop_layer's coverage inequality needs every
// factor <= 1.
namespace bg1 {

inline constexpr backdrop_wrap::Band GROUND_BANDS[] = {
    {  0,  82, 0.30f},   // far water + shore 1; hills_midfar stands here
    { 82, 100, 0.70f},   // shore 2; hills_near stands here
    {100, 144, 1.00f},   // near field; the rocks and the terrain live here
};

// The art's native size, which the table above is stated in.
inline constexpr int NATIVE_W = 344;
inline constexpr int NATIVE_H = 144;

// --- the stack itself ---------------------------------------------
//
// Back to front, in strict numeric-descending filename order -- the art README's
// front-to-back table read the other way round. Confirmed by pixels rather than
// trusted: compositing in this order reproduces the reference frame exactly,
// everywhere the player is not.
//
// Order matters more than it looks. The ground plane fills art rows 63..143 at
// 56% coverage, so placed near the front it paints over the entire hill range.
//
// One factor per layer, and it is the horizontal one. Every layer in this stack
// is locked to the world vertically, at 1.00.
//
// Giving each layer its authored factor on both axes is right for a stack of
// layers each painted with its own horizon. It is false of this one: the nine
// images share one painted land plane, and the pixels say so. bg1_08_ground is
// opaque from art row 63 to 143; every hill's silhouette bottoms out on it (rows
// 67..89), the foreground rocks are painted into it (rows 126..143), and
// assets/bg1_albedo.bmp -- the terrain the player actually stands on, rows
// 132..143, world-locked at 1.00 by definition -- is a flattened copy of that
// same band with the rocks composited in. The plane is not a layer among nine;
// it is one surface that nine images each draw a piece of.
//
// Spread across a vertical factor ladder, the pieces of one surface shear apart:
// over the camera's vertical travel the ground band rises at its own factor
// while the rocks and the terrain rise the full distance, opening a gap of many
// art rows underneath objects painted as resting on it. Any two different
// vertical factors reopen that, which is why the vertical factor is 1.00 flat
// and why it is not a column in this table -- a per-layer vertical factor is the
// defect, so there is nowhere to write one.
//
// 1.00 is also the largest safe value, for the same reason the horizontal cap
// is: the layer is world-sized, so at f <= 1 it covers the viewport at every
// camera position with no gap. The inequality is at draw_backdrop_layer.
//
// What it costs is the vertical depth cue -- climbing no longer separates the
// bands. It buys a composition that is the painting at every camera height
// instead of only at the top of the world. If the cue is wanted later it cannot
// come from this column; it needs art whose layers do not share a plane.
//
// No grades. The reference composites to an exact match with no grading at all,
// so the art already carries its own aerial perspective. Depth cueing on top of
// that is a tuning pass recorded in TUNING.md, not a guess here -- which is why
// there is no grade column either.
//
// The foreground is 1.00 where the art README offers 1.00x-1.20x. A world-sized
// layer gaps at the right edge above 1.00; camera_test pins both sides of the
// inequality. If 1.20 is genuinely wanted, the fix is a wider foreground image
// (344 * 1.20 = 413 px), never a bigger number here.
//
// The ground plane's row asks for a 0.28-0.52 ramp, which one factor cannot
// express -- that is what GROUND_BANDS above answers, and why that row's
// parallax_x is the 0.0 sentinel rather than a fourth copy of a number already
// stated three times.
struct Layer {
    // Under LAYER_DIR. The directory is not stored per row: one directory for nine
    // files is a fact about the set, not about a layer, and writing it nine times is
    // eight chances to write it differently.
    const char* file;

    // Horizontal only; see above for why there is no vertical column. Meaningless
    // when `banded` is set -- GROUND_BANDS carries a factor per band instead -- and
    // 0.0 is the sentinel that says so.
    float parallax_x;

    // The sky is the only layer painted edge to edge. Everything in front of it is a
    // silhouette and is colour-keyed, so this is what the loader hands to
    // load_art_texture, inverted.
    bool opaque;

    // Scrolls as GROUND_BANDS rather than as one factor. A field on the row rather
    // than a filename comparison in the loader, so reordering the table cannot
    // silently band the wrong image, and boot_test can require exactly one of them.
    bool banded;

    // Drawn after the world rather than before it. The rocks are the only layer the
    // player passes behind.
    bool is_foreground;
};

inline constexpr Layer LAYERS[] = {
    {"bg1_09_sky.bmp",           0.04f,  true, false, false},
    {"bg1_08_ground.bmp",        0.00f, false,  true, false},
    {"bg1_07_mountains.bmp",     0.12f, false, false, false},
    {"bg1_06_hills_far.bmp",     0.20f, false, false, false},
    {"bg1_05_hills_midfar.bmp",  0.30f, false, false, false},
    {"bg1_04_hills_mid.bmp",     0.42f, false, false, false},
    {"bg1_03_hills_midnear.bmp", 0.55f, false, false, false},
    {"bg1_02_hills_near.bmp",    0.70f, false, false, false},
    {"bg1_01_fg_rocks.bmp",      1.00f, false, false,  true},
};

// The one directory every Layer::file is relative to.
inline constexpr const char* LAYER_DIR = "assets/bg1/";

// What a complete load looks like, and the number the launch line reports
// against. Derived rather than written: a tenth row must not need a second edit
// here to be counted, or the stack reports "9 of 9" while drawing ten.
inline constexpr int LAYER_COUNT = static_cast<int>(sizeof(LAYERS) / sizeof(LAYERS[0]));

// --- `bg1_ext`: the same location, 688x288 -----------------------------------
//
// A second set, generated from the first by `python tools/generate_bg1_ext.py`
// out of art_src/Background_1's own pixels. The art argument is in that script's
// header; what belongs here is the three things the loader has to know.
//
// The parallax factors are bg1's, unchanged, and that is the point of the set
// rather than an economy: they are one over a depth, and the extension is the
// same nine depths of the same place. A layer that scrolled differently here
// would be a different landscape wearing the same palette. boot_test checks the
// two tables against each other rather than trusting this paragraph.
//
// EXT_NATIVE_W/H is exactly twice NATIVE_W/H, and the art is not twice the
// picture: the generator anchors bottom-anchored art to the bottom and
// top-anchored art to the top, so the rows a 1080p window shows while the body
// is standing are the same rows bg1 shows. The extra height buys somewhere to
// climb to, and a mountain range with sky above it.
//
// The band table moved down by 144 and did not change shape, because the ground
// plane's structure is bottom-anchored like everything standing on it. The
// contacts the factors are chosen for are at art rows 217 and 233 here, exactly
// bg1's 73 and 89 plus 144, and the cuts are bg1's 82 and 100 plus 144. Rows
// 222..230 and 242..245 are flat, so a scroll step across either is invisible.
inline constexpr backdrop_wrap::Band EXT_GROUND_BANDS[] = {
    {  0, 226, 0.30f},   // far water + shore 1; hills_midfar stands here
    {226, 244, 0.70f},   // shore 2; hills_near stands here
    {244, 288, 1.00f},   // near field; the rocks and the terrain live here
};

inline constexpr int EXT_NATIVE_W = NATIVE_W * 2;  // 688
inline constexpr int EXT_NATIVE_H = NATIVE_H * 2;  // 288

inline constexpr Layer EXT_LAYERS[] = {
    {"bg1_ext_09_sky.bmp",           0.04f,  true, false, false},
    {"bg1_ext_08_ground.bmp",        0.00f, false,  true, false},
    {"bg1_ext_07_mountains.bmp",     0.12f, false, false, false},
    {"bg1_ext_06_hills_far.bmp",     0.20f, false, false, false},
    {"bg1_ext_05_hills_midfar.bmp",  0.30f, false, false, false},
    {"bg1_ext_04_hills_mid.bmp",     0.42f, false, false, false},
    {"bg1_ext_03_hills_midnear.bmp", 0.55f, false, false, false},
    {"bg1_ext_02_hills_near.bmp",    0.70f, false, false, false},
    {"bg1_ext_01_fg_rocks.bmp",      1.00f, false, false,  true},
};

inline constexpr const char* EXT_LAYER_DIR = "assets/bg1_ext/";

inline constexpr int EXT_LAYER_COUNT =
    static_cast<int>(sizeof(EXT_LAYERS) / sizeof(EXT_LAYERS[0]));

// --- `bg_gemini`: extended 688x288 newly generated scene ---------------------
inline constexpr backdrop_wrap::Band GEMINI_GROUND_BANDS[] = {
    {  0, 164, 0.30f},   // far water + shore 1; hills_midfar stands here
    {164, 200, 0.70f},   // shore 2; hills_near stands here
    {200, 288, 1.00f},   // near field; the rocks and terrain live here
};

inline constexpr Layer GEMINI_LAYERS[] = {
    {"bg_gemini_09_sky.bmp",           0.04f,  true, false, false},
    {"bg_gemini_08_ground.bmp",        0.00f, false,  true, false},
    {"bg_gemini_07_mountains.bmp",     0.12f, false, false, false},
    {"bg_gemini_06_hills_far.bmp",     0.20f, false, false, false},
    {"bg_gemini_05_hills_midfar.bmp",  0.30f, false, false, false},
    {"bg_gemini_04_hills_mid.bmp",     0.42f, false, false, false},
    {"bg_gemini_03_hills_midnear.bmp", 0.55f, false, false, false},
    {"bg_gemini_02_hills_near.bmp",    0.70f, false, false, false},
    {"bg_gemini_01_fg_rocks.bmp",      1.00f, false, false,  true},
};

inline constexpr const char* GEMINI_LAYER_DIR = "assets/bg_gemini/";

inline constexpr int GEMINI_LAYER_COUNT =
    static_cast<int>(sizeof(GEMINI_LAYERS) / sizeof(GEMINI_LAYERS[0]));

// --- `bg_forest`: a different place at the same 688x288 ----------------------
//
// The first set that is not bg1's landscape: a forest trail, generated by
// `python tools/generate_bg_forest.py` out of tools/pixel_art.py's PALETTE
// rather than out of another set's pixels.
//
// The ladder below is bg_gemini's, unchanged, and that is a claim about the
// camera rather than about the art. A parallax factor is one over a depth, and
// nine depths viewed through the same lens are the same nine numbers whatever is
// standing at them -- a forest that scrolled differently would be saying the
// camera moved, not that the trees did. What differs is only what is painted at
// each depth.
//
// The band table is bg_gemini's too, and its cuts are load-bearing for the same
// reason: the generator paints rows 158..170 and 196..204 flat specifically so
// the cuts at 164 and 200 fall on paint that has no horizontal detail, and the
// two layers that stand on the plane have their feet inside those flat runs.
// test_bg1_ground_bands reads the BMP and checks the cuts; the feet are the
// generator's own contract with them, written down here because nothing can
// check that half.
inline constexpr backdrop_wrap::Band FOREST_GROUND_BANDS[] = {
    {  0, 164, 0.30f},   // far floor; hills_midfar stands here, on flat ground_far
    {164, 200, 0.70f},   // understory; hills_near stands here, on flat dirt_mid
    {200, 288, 1.00f},   // the trail bed; the rocks and the terrain live here
};

inline constexpr Layer FOREST_LAYERS[] = {
    {"bg_forest_09_sky.bmp",           0.04f,  true, false, false},
    {"bg_forest_08_ground.bmp",        0.00f, false,  true, false},
    {"bg_forest_07_mountains.bmp",     0.12f, false, false, false},
    {"bg_forest_06_hills_far.bmp",     0.20f, false, false, false},
    {"bg_forest_05_hills_midfar.bmp",  0.30f, false, false, false},
    {"bg_forest_04_hills_mid.bmp",     0.42f, false, false, false},
    {"bg_forest_03_hills_midnear.bmp", 0.55f, false, false, false},
    {"bg_forest_02_hills_near.bmp",    0.70f, false, false, false},
    {"bg_forest_01_fg_rocks.bmp",      1.00f, false, false,  true},
};

inline constexpr const char* FOREST_LAYER_DIR = "assets/bg_forest/";

inline constexpr int FOREST_LAYER_COUNT =
    static_cast<int>(sizeof(FOREST_LAYERS) / sizeof(FOREST_LAYERS[0]));

// --- the sets, and how a scene finds one -------------------------------------
//
// A scene names its set here, once. Matching on the scene's name at the call
// site instead is a chain that compiles perfectly while loading one set's images
// into another set's world, since nothing about a wrong stack fails to build.
//
// Everything a set is, is on the row. The alternative was a second copy of the
// loader per set, which is how two tables eventually disagree about which one is
// banded. boot_test iterates this array, so every check one set has is a check
// every other set has, including the ones nobody thought to write twice.
struct Set {
    // The assets/scenes.txt row this stack backs. A scene with no set here draws the
    // generated three-layer backdrop.
    const char* scene;

    const char* dir;
    int native_w, native_h;

    const Layer* layers;
    int layer_count;

    // The banded layer's table. Held on the set rather than looked up globally
    // because it is stated in this set's art rows, and the sets' rows differ.
    const backdrop_wrap::Band* bands;
    int band_count;
};

inline constexpr Set SETS[] = {
    {"bg1",       LAYER_DIR,        NATIVE_W,     NATIVE_H,
     LAYERS,       LAYER_COUNT,        GROUND_BANDS,
     static_cast<int>(sizeof(GROUND_BANDS) / sizeof(GROUND_BANDS[0]))},
    {"bg1_ext",   EXT_LAYER_DIR,    EXT_NATIVE_W, EXT_NATIVE_H,
     EXT_LAYERS,   EXT_LAYER_COUNT,   EXT_GROUND_BANDS,
     static_cast<int>(sizeof(EXT_GROUND_BANDS) / sizeof(EXT_GROUND_BANDS[0]))},
    {"bg_gemini", GEMINI_LAYER_DIR, EXT_NATIVE_W, EXT_NATIVE_H,
     GEMINI_LAYERS, GEMINI_LAYER_COUNT, GEMINI_GROUND_BANDS,
     static_cast<int>(sizeof(GEMINI_GROUND_BANDS) / sizeof(GEMINI_GROUND_BANDS[0]))},
    {"bg_forest", FOREST_LAYER_DIR, EXT_NATIVE_W, EXT_NATIVE_H,
     FOREST_LAYERS, FOREST_LAYER_COUNT, FOREST_GROUND_BANDS,
     static_cast<int>(sizeof(FOREST_GROUND_BANDS) / sizeof(FOREST_GROUND_BANDS[0]))},
};

inline constexpr int SET_COUNT = static_cast<int>(sizeof(SETS) / sizeof(SETS[0]));

// Null for a scene that has no authored stack. Null rather than a default set:
// falling back to one set's images for an unrecognised name would draw one
// world's art over another world's terrain, which is a lookup bug that looks
// like a rendering bug.
inline const Set* find(const char* scene) {
    if (!scene) return nullptr;
    for (int i = 0; i < SET_COUNT; ++i) {
        const char* a = SETS[i].scene;
        const char* b = scene;
        while (*a && *a == *b) { ++a; ++b; }
        if (!*a && !*b) return &SETS[i];
    }
    return nullptr;
}

} // namespace bg1
