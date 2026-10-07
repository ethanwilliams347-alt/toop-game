#include "present/present.h"

#include <algorithm>
#include <cmath>
#include "game/camera.h"
#include "game/pacer.h"
#include "physics/material.h"
#include "render/depth_rig.h"

namespace present {

void paint_enemies(const Run& run, float alpha, std::vector<uint32_t>& atlas,
                   std::vector<frame::EnemySprite>& out) {
    out.clear();
    atlas.resize(static_cast<size_t>(ENEMY_ATLAS_W) * ENEMY_ATLAS_H, 0u);
    for (int slot = 0; slot < Run::MAX_ENEMIES; ++slot) {
        const Enemy& en = run.enemies[static_cast<size_t>(slot)];
        if (!en.is_alive()) continue;
        const Species& kind = en.species();
        const body_art::Art& art = *kind.art;

        // The slam's telegraph: the eyes heat from their own colour toward
        // white-hot over the wind-up, so the thing about to land is readable from
        // the face, which is where the player is aiming anyway. Drawn here, from
        // the windup count the body exposes, and nowhere in the simulation -- it
        // changes nothing an arrow can hit.
        uint32_t eye_heat = 0;
        if (en.windup_left() > 0 && kind.windup_steps > 0) {
            eye_heat = static_cast<uint32_t>(
                255 * (kind.windup_steps - en.windup_left()) / kind.windup_steps);
        }
        // The pose's rectangle, painted cell by cell from the same question an
        // arrow asks (Enemy::posed_pixel), so what is drawn in a cell is what is
        // hit there. Whatever the pose cannot reach is left as it was: nothing
        // samples outside the rectangle.
        const rig::Box b = en.pose_bounds();
        const int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
        if (bw <= 0 || bh <= 0) continue;
        const int slot_x = (slot % ENEMY_SLOT_COLS) * ENEMY_SLOT_W;
        const int slot_y = (slot / ENEMY_SLOT_COLS) * ENEMY_SLOT_H;
        for (int y = 0; y < bh; ++y) {
            uint32_t* row = atlas.data() + static_cast<size_t>(slot_y + y) * ENEMY_ATLAS_W + slot_x;
            for (int x = 0; x < bw; ++x) {
                const int index = en.posed_pixel(b.x0 + x, b.y0 + y);
                uint32_t c = 0u;
                if (index >= 0) {
                    const int ax = index % art.w, ay = index / art.w;
                    c = art.color_at(ax, ay);
                    if (eye_heat > 0 && art.is_head(ax, ay)) {
                        const uint32_t g = (c >> 8) & 0xFFu, bl = c & 0xFFu;
                        c = 0xFFFF0000u | ((g + (255u - g) * eye_heat / 255u) << 8) |
                            (bl + (255u - bl) * eye_heat / 255u);
                    }
                }
                row[x] = c;
            }
        }

        // Interpolated between the last two steps like the body is, and with the
        // same teleport clamp -- climbing out of its own dust can lift one a few
        // cells in a step.
        const pacer::Interpolated at = pacer::interpolate(
            static_cast<float>(en.prev_cell_x()) + fx::to_float(en.prev_remainder_x()),
            static_cast<float>(en.prev_cell_y()) + fx::to_float(en.prev_remainder_y()),
            static_cast<float>(en.cell_x()) + fx::to_float(en.remainder_x()),
            static_cast<float>(en.cell_y()) + fx::to_float(en.remainder_y()), alpha);
        // The rectangle's own anchor. Posed column x lands on frame column x
        // facing right and on (w-1-x) facing left, and SDL's flip mirrors the
        // rectangle about its own middle -- so facing left, the rectangle's left
        // edge in the frame is w - x1, not x0. This is Enemy::pixel_at's flip
        // written for a rectangle; the two must agree or what is drawn is a cell
        // off what is hit.
        const int left_in_frame = en.facing_left() ? art.w - b.x1 : b.x0;
        out.push_back(frame::EnemySprite{SDL_Rect{slot_x, slot_y, bw, bh}, at.x, at.y,
                                         kind.offset_x() - left_in_frame,
                                         kind.offset_y() - b.y0, en.facing_left()});
    }
}

void arrows(const Run& run, float alpha, std::vector<frame::ArrowSprite>& out) {
    out.clear();
    for (const Arrow& a : run.quiver.arrows()) {
        if (!a.live) continue;
        const float px = static_cast<float>(a.prev_x) + fx::to_float(a.prev_rem_x);
        const float py = static_cast<float>(a.prev_y) + fx::to_float(a.prev_rem_y);
        const float nx = static_cast<float>(a.x) + fx::to_float(a.rem_x);
        const float ny = static_cast<float>(a.y) + fx::to_float(a.rem_y);
        const float vx = fx::to_float(a.vel_x), vy = fx::to_float(a.vel_y);
        const float len = std::sqrt(vx * vx + vy * vy);
        out.push_back(frame::ArrowSprite{px + (nx - px) * alpha, py + (ny - py) * alpha,
                                         len > 0.0f ? vx / len : 0.0f,
                                         len > 0.0f ? vy / len : 0.0f});
    }
}

std::string run_readout(const Run& run) {
    // HP goes first. The rest of the line is an instrument for whoever is
    // building the engine; this is the one thing on it a player is playing
    // against, and reading it should not mean scanning past a frame rate.
    std::string status = "HP:" + std::to_string(run.player.health());

    // GOAL is a bearing, and it is text on the line that already exists rather
    // than an arrow at the screen edge. The objective starts well off-screen, so
    // without this the run is "walk east until you find it", which is not a
    // difficulty but a missing instrument. Distance is to the body's centre, in
    // cells.
    if (run.has_objective()) {
        const int gdx = run.objective_x() - run.player.center_x();
        const int gdy = run.objective_y() - run.player.center_y();
        const int gdist = static_cast<int>(
            std::sqrt(static_cast<double>(gdx) * gdx + static_cast<double>(gdy) * gdy));
        status += "  GOAL:" + std::to_string(gdist) + (gdx < 0 ? "W" : "E");
    }
    // How many enemies are left and how many are down. Beside HP because it is
    // the other number the player is playing against.
    status += "  FOES:" + std::to_string(run.enemies_alive()) +
              "  KILLS:" + std::to_string(run.kills());
    return status;
}

std::string diagnostics(const Run& run, int fps, ElementType brush, int brush_size) {
    // Rebuilt every frame rather than cached for a second. The brush name and the
    // awake-chunk count are answers to "what did that key just do" and "has the
    // world settled yet"; cached, a hotbar key leaves the HUD naming the old brush
    // for up to a second. Only the frame rate is a once-a-second quantity, and
    // the caller caches that one.
    std::string text = "FPS:" + std::to_string(fps) + " BRUSH:" + material_of(brush).name +
                       "(" + std::to_string(brush_size) + ")" +
                       " CHUNKS:" + std::to_string(run.grid.active_chunk_count());
    // CHUNKS:0 does not mean the world has stopped. A falling structural piece is
    // carried by the support queue rather than by the chunk rects, so a slab can
    // fall the height of the world with the counter at zero. A flag rather than a
    // count because the queue's length is a number of seeds, not of pieces.
    if (run.grid.has_pending_support_checks()) text += "+FALLING";
    return text;
}

void stand_backdrop_anchor(frame::Backdrop& backdrop, int padded_w, int padded_h,
                           int world_w, int world_h) {
    // Horizontally the world's centre -- every layer of a standing-anchored set
    // wraps, so this only chooses which columns line up where.
    backdrop.anchor_x = 0.5f * static_cast<float>(std::max(0, world_w - padded_w));
    backdrop.anchor_y = depth_rig::standing_anchor_y(backdrop.rig, Player::HEIGHT, padded_h,
                                                     Camera::VERTICAL_ANCHOR, world_h);
}

} // namespace present
