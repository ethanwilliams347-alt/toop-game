#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "game/run.h"
#include "render/frame.h"

// The presenter: what the frame shows, worked out from the simulation's state.
//
// Everything here reads a `const Run&` and writes plain buffers and records --
// the enemy atlas's pixels, the sprite lists, the HUD's text. It makes no SDL
// call; frame.h's record types carry an SDL_Rect, which is the only SDL in
// sight, and that is a type rather than a call. So present_test can paint a
// body into the atlas and read the pixels back without a window, which the
// same code inside main()'s frame loop could never be.
//
// It sits between the two halves the rest of the tree keeps apart: it may read
// the simulation (src/render/ may not) and it knows the frame's record types
// (src/physics/ and src/game/ may not). That is why it is its own directory and
// its own source set rather than a file in either. Nothing in it is ever an
// input to a step.
namespace present {

// --- the enemy atlas -------------------------------------------------------
//
// The enemies' bodies, one slot per Run::enemies slot, redrawn every frame from
// each body's surviving pixels in its current pose. A slot is the largest
// species' frame plus the most any pose reaches outside it (Enemy::MAX_POSE_PAD)
// on every side -- a troll's club raised over its head is well above its frame
// -- so any slot can hold any species in any pose. The slots sit in a grid
// rather than one row, which at that size would be wider than the smallest
// texture limit SDL promises.
//
// Built rather than loaded, because there is no fixed picture to load: an enemy
// looks like whatever is left of it, posed however it is standing, and both are
// simulation state (physics/enemy_art.h, physics/rig.h). Every frame rather than
// on change, and only the rectangle each pose covers is written: a few thousand
// pixels a body, against a cache keyed on which body is in which slot in which
// pose, which is a correctness problem in exchange for nothing measurable.
inline constexpr int ENEMY_SLOT_W = Enemy::MAX_FRAME_W + 2 * Enemy::MAX_POSE_PAD;
inline constexpr int ENEMY_SLOT_H = Enemy::MAX_FRAME_H + 2 * Enemy::MAX_POSE_PAD;
inline constexpr int ENEMY_SLOT_COLS = 6;
inline constexpr int ENEMY_SLOT_ROWS = (Run::MAX_ENEMIES + ENEMY_SLOT_COLS - 1) / ENEMY_SLOT_COLS;
inline constexpr int ENEMY_ATLAS_W = ENEMY_SLOT_W * ENEMY_SLOT_COLS;
inline constexpr int ENEMY_ATLAS_H = ENEMY_SLOT_H * ENEMY_SLOT_ROWS;
static_assert(ENEMY_ATLAS_W <= 2048 && ENEMY_ATLAS_H <= 2048,
              "the enemy atlas outgrew the texture size every SDL renderer supports");

// Paints every living enemy into `atlas` (ENEMY_ATLAS_W x ENEMY_ATLAS_H ARGB,
// row-major) and appends one sprite per body to `out`, interpolated to `alpha`
// between the last two steps. Each sprite's `src` is the rectangle of the atlas
// that was written, which is also the rectangle the caller has to upload.
void paint_enemies(const Run& run, float alpha, std::vector<uint32_t>& atlas,
                   std::vector<frame::EnemySprite>& out);

// One sprite per arrow in flight or at rest, interpolated like the enemies but
// without the teleport clamp: an arrow covers several cells a step by design.
void arrows(const Run& run, float alpha, std::vector<frame::ArrowSprite>& out);

// --- the HUD's words ---------------------------------------------------------

// The run's readout: HP first, then the bearing to the objective, then the foes
// left and the kills -- the numbers a player is playing against, ahead of the
// diagnostics.
std::string run_readout(const Run& run);

// The engine's instruments: frame rate, brush and awake chunks, with +FALLING
// while the support queue is carrying a piece the chunk count cannot see.
std::string diagnostics(const Run& run, int fps, ElementType brush, int brush_size);

// --- the backdrop's anchor ---------------------------------------------------

// Where a standing-anchored backdrop is exactly the painting: the camera with the
// player standing on the rig's contact row, horizontally at the world's centre.
// Per frame, because it depends on the viewport, which the display mode can
// change under a loaded scene.
void stand_backdrop_anchor(frame::Backdrop& backdrop, int padded_w, int padded_h, int world_w,
                           int world_h);

} // namespace present
