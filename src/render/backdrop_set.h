#pragma once
#include <string>
#include <vector>
#include "render/backdrop_wrap.h"
#include "render/depth_rig.h"

// A backdrop: the painted layers behind (and one in front of) a scene, and how
// each one moves with the camera. Read from `assets/<dir>/backdrop.txt`, which a
// scene names with `backdrop=<dir>` in assets/scenes.txt.
//
// --- one system, where there were three ---------------------------------------
//
// There used to be three ways to draw a background. A generated sky, mountain
// range and receding ground tile (render/backdrop_layers.h, written by
// tools/generate_backdrop.py) that only the `empty` debug scene still used, with
// a near-terrain tint pass compiled in and switched off. The `bg1` family
// (render/bg1_backdrop.h): nine hand-painted layers at typed factors, locked
// vertically, with the ground cut into three bands. And the perspective rig
// (render/depth_rig.h, render/rig_backdrop.h), which derives each factor from the
// row the layer stands on. frame.cpp branched between them inside every layer
// function, frame::Backdrop carried fields for all three, and main.cpp had a
// loader for each.
//
// The rig turned out to contain the other two as settings. A bg1 layer placed at
// -(cam * f) is a rig layer whose anchor is the world's corner; bg1's locked
// vertical is a rig with vertical_strength 0 (depth_rig.h says so: "k = 0 is
// bg1's locked vertical"); and bg1's bands are per-row factors, which the rig's
// plane already draws, just stepped instead of continuous. So there is now one
// layer type, one file format, one loader and one draw function, and a set picks
// its behaviour with three lines of data:
//
//     anchor corner | standing   where the stack is exactly the painting
//     rig h c k                  the camera the painting implies
//     layer ... bands=...        a painted surface cut on flat paint
//
// The generated three-layer backdrop is gone rather than migrated. It had one
// user, a debug scene; its plane geometry existed to feed the switched-off tint
// pass; and the rig's line-scrolled plane is the better version of its strip
// loop. `empty` now draws the clear colour behind the cells.
//
// --- why a text file -----------------------------------------------------------
//
// The tables used to be C++ headers, so a new backdrop was a code change and a
// rebuild, and a scene found its set by a name comparison in main.cpp. Now it is
// an asset drop: paint the layers, write the file, name the directory in
// scenes.txt. The numbers still have to agree with the art, and backdrop_set_test
// still checks every shipped set against its BMPs -- feet, bands on flat paint,
// the wrap seam -- exactly as boot_test and rig_test did for the headers.
//
// Like every parser here it is all-or-nothing: the first malformed record
// rejects the whole file and names its line. A stack missing the layer it could
// not read draws the hills with no ground under them and says nothing.
//
// SDL-free, and float is fine here: this is render data, never simulation.
//
// --- the format ----------------------------------------------------------------
//
// One record per line; `#` to end-of-line is a comment.
//
//     size     <w> <h>                 the art's native size; required, first
//     anchor   corner | standing       optional; default standing
//     rig      <horizon> <contact> <k> optional; see depth_rig.h
//     ripple   <cells>                 optional; amplitude of rippled rows
//     layer    <file> <how it moves> [flags]
//
// Layers are listed back to front. Each says how it moves with exactly one of:
//
//     factor=<f>        one factor, horizontal; vertical is depth_rig's of it
//     foot=<row>        stands on the plane at that row; factor derived (needs rig)
//     plane             the ground plane, one factor per row (needs rig)
//     bands=r0:r1:f,... a painted surface in horizontal bands, each at its own
//                       factor; bands tile the layer's rows exactly
//
// and any of these flags:
//
//     opaque            painted edge to edge; loaded with no colour key
//     foreground        drawn in front of the player and the cells
//     on_plane          placed vertically by the plane's rows (needs rig, ripple=)
//     ripple=r0:r1      art rows [r0, r1) shimmer sideways with time
//     drift=<cells/s>   sideways motion with no camera motion (clouds)
namespace backdrop_set {

using Band = backdrop_wrap::Band;

enum class Anchor {
    // The stack is the painting when the camera is at the world's top-left: every
    // layer is placed at -(cam * f). What the bg1 family was drawn for, and the
    // only anchor under which a world-sized layer at f <= 1 never needs a second
    // copy -- so art that does not tile can be used.
    Corner,
    // The stack is the painting where a standing player's camera sits, and the
    // parallax spreads out from there (depth_rig.h, "the anchor"). Every layer
    // wraps, so the art must tile; backdrop_set_test checks the seam.
    Standing,
};

struct Layer {
    std::string file;  // under the set's directory
    int line = 0;      // in backdrop.txt, for diagnostics

    // Exactly one of the four ways to move is set.
    bool has_factor = false;
    float factor = 0.0f;
    int foot_row = -1;  // >= 0 when the layer stands on the plane
    bool plane = false;
    std::vector<Band> bands;

    bool opaque = false;
    bool foreground = false;
    bool on_plane = false;
    int ripple_row0 = 0, ripple_row1 = 0;  // half-open; empty when equal
    float drift = 0.0f;
};

struct Set {
    std::string dir;  // "assets/<dir>/", where the layers are
    int native_w = 0, native_h = 0;
    Anchor anchor = Anchor::Standing;
    // Off: no layer may use foot= or plane, and the vertical is locked (k = 0),
    // which is the bg1 family's model.
    bool has_rig = false;
    depth_rig::Rig rig{0, 1, 0.0f};
    float ripple_amplitude = 0.0f;  // cells
    std::vector<Layer> layers;
};

// The layer's horizontal factor: its own, or derived from its feet. Meaningless
// for a plane or a banded layer, whose factor is per row; 0 for those.
inline float factor_of(const Set& set, const Layer& l) {
    if (l.foot_row >= 0) return depth_rig::factor_at(set.rig, static_cast<float>(l.foot_row));
    return l.has_factor ? l.factor : 0.0f;
}

// The layer's vertical factor. A banded layer is locked (the parser refuses
// bands in a set with any vertical parallax; see backdrop_set.cpp), and so is a
// set with no rig.
inline float vertical_factor_of(const Set& set, const Layer& l) {
    if (!set.has_rig || !l.bands.empty() || l.plane) return 1.0f;
    return depth_rig::vertical_factor(set.rig, factor_of(set, l));
}

// Parses `path`. `dir` is stored on the set as where its layers live. On any
// malformed record the set comes back with no layers and `error` names the line.
// A missing file is an error too, unlike a missing level or prop list: a scene
// that names a backdrop and gets none is a scene drawn over the clear colour.
Set load(const std::string& path, const std::string& dir, std::string* error);

} // namespace backdrop_set
