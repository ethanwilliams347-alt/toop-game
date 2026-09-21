#pragma once
#include "scene/scene_list.h"

// The decision half of activating a scene.
//
// Activating a scene is boot order run again, per scene, at runtime, so it
// belongs beside the rest of boot and under the same test rather than as a
// lambda in main.cpp that nothing reaches from either side.
//
// What is here: the two precedence rules that were otherwise prose at the call
// site -- the size order (the row beats the BMP beats the engine default) and
// rebuilding the render targets only when the scale actually changed. What stays
// in main.cpp: apply_mode, camera.set_scale, the printed lines, and the failure
// path that leaves the old targets standing. The SDL-free side decides, the
// shell acts, and the decision has a test.
//
// No SDL, by rule, and no reading of the art either: the material BMP's
// dimensions arrive as told values. main.cpp opens the BMP -- it has to anyway,
// since stamping needs a grid of the right size -- this decides what the numbers
// mean, and neither half has to know about the other.
namespace scene_activation {

// What a scene resolves to, before anything is built at it.
struct Resolved {
    int world_w = 0;
    int world_h = 0;
    bool infinite = false;

    // Screen pixels per world cell for this scene.
    int scale = 0;

    // True only when `scale` differs from the scale already in effect, which is why
    // the current scale is an input rather than something the caller diffs
    // afterwards. apply_mode destroys and rebuilds two textures and a light field:
    // free once, wasteful on every scene switch between two scenes that share a
    // scale.
    bool scale_changed = false;
};

// `def` is the row; art_w/art_h are the material BMP's own dimensions, or 0 when
// the scene declared no art or the art failed to load; default_w and default_h
// are the engine defaults; current_scale is the scale in effect right now.
//
// The size, in the order the answers are trusted: the row says so, or the
// material BMP does, or it is the engine default. The BMP comes second rather
// than first because an author who wrote a size meant it -- a scene may
// legitimately be larger than the image stamped into its corner -- and the
// default comes last because it is the only one of the three that is not a fact
// about this scene.
inline Resolved resolve(const scene_list::SceneDef& def,
                        int art_w, int art_h,
                        int default_w, int default_h,
                        int current_scale) {
    Resolved r;
    r.infinite = def.is_infinite();
    r.world_w = def.custom_width  > 0 ? def.custom_width
              : (art_w > 0 ? art_w : default_w);
    r.world_h = def.custom_height > 0 ? def.custom_height
              : (art_h > 0 ? art_h : default_h);
    r.scale = def.scale;
    r.scale_changed = def.scale != current_scale;
    return r;
}

} // namespace scene_activation
