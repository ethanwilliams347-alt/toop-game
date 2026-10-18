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
            eye_heat = static_cast<uint32_t>(255 * (kind.windup_steps - en.windup_left()) /
                                             kind.windup_steps);
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

        // Interpolated between the last two steps like the player is, and snapped
        // the same way on a step the body says it was lifted rather than moved --
        // climbing out of its own dust can lift one several cells in a step.
        const pacer::Interpolated at = pacer::interpolate(
            static_cast<float>(en.prev_cell_x()) + fx::to_float(en.prev_remainder_x()),
            static_cast<float>(en.prev_cell_y()) + fx::to_float(en.prev_remainder_y()),
            static_cast<float>(en.cell_x()) + fx::to_float(en.remainder_x()),
            static_cast<float>(en.cell_y()) + fx::to_float(en.remainder_y()), alpha,
            en.teleported());
        // The rectangle's own anchor. Posed column x lands on frame column x
        // facing right and on (w-1-x) facing left, and SDL's flip mirrors the
        // rectangle about its own middle -- so facing left, the rectangle's left
        // edge in the frame is w - x1, not x0. This is Enemy::pixel_at's flip
        // written for a rectangle; the two must agree or what is drawn is a cell
        // off what is hit.
        const int left_in_frame = en.facing_left() ? art.w - b.x1 : b.x0;
        out.push_back(frame::EnemySprite{gfx::Rect{slot_x, slot_y, bw, bh}, at.x, at.y,
                                         kind.offset_x() - left_in_frame, kind.offset_y() - b.y0,
                                         en.facing_left()});
    }
}

void paint_bodies(const Run& run, float alpha, std::vector<uint32_t>& atlas,
                  std::vector<frame::BodySprite>& out, std::vector<frame::HiddenCell>& hidden) {
    out.clear();
    hidden.clear();
    atlas.resize(static_cast<size_t>(BODY_ATLAS_W) * BODY_ATLAS_H, 0u);
    const Grid& g = run.grid;
    const std::vector<uint32_t>& pixels = g.get_pixels();
    const int grid_w = g.get_width(), grid_h = g.get_height();

    // What a hidden cell shows instead of the body (frame::HiddenCell): the
    // nearest liquid or grain along the four directions, looking past the body's
    // own cells for up to two -- sprite and cells can be about a cell apart, so
    // the sliver that shows can be the rim or the row inside it. Sideways first,
    // because what a sinking body is pushing through is beside it; what lands on
    // top of it is what it displaced a step ago. Anything else stops the look,
    // so a stone beside a wall with a pool behind it does not borrow the pool.
    // The record carries the material the colour came from as well, which the
    // packed-cell path (render/cell_pack.h) draws from instead of the colour.
    const auto fill_of = [&](int index, uint8_t tag) -> frame::HiddenCell {
        static constexpr int DX[4] = {-1, 1, 0, 0};
        static constexpr int DY[4] = {0, 0, 1, -1};
        // Cell index to column and row; the division is meant to truncate.
        const int x = index % grid_w, y = index / grid_w;
        for (int d = 0; d < 4; ++d) {
            for (int reach = 1; reach <= 2; ++reach) {
                const int nx = x + DX[d] * reach, ny = y + DY[d] * reach;
                if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h) break;
                const Element e = g.get_element(nx, ny);
                if (is_structural(e.type) && e.piece_tag == tag) continue;  // the body itself
                if (is_liquid(e.type) || material_of(e.type).move == MoveKind::Powder)
                    return {index, pixels[static_cast<size_t>(ny) * grid_w + nx], e.type};
                break;
            }
        }
        return {index, 0u, ElementType::Empty};
    };

    // Shelves: left to right along a row as tall as its tallest picture, then the
    // next row down.
    int shelf_x = 0, shelf_y = 0, shelf_h = 0;
    for (int i = 0; i < g.tipping_count(); ++i) {
        const std::vector<Grid::BodyCell>& shape = g.body_shape(i);
        const std::vector<int>& at = g.body_cells(i);
        if (shape.empty()) continue;

        int min_x = shape[0].x, max_x = shape[0].x, min_y = shape[0].y, max_y = shape[0].y;
        for (const Grid::BodyCell& c : shape) {
            min_x = std::min<int>(min_x, c.x);
            max_x = std::max<int>(max_x, c.x);
            min_y = std::min<int>(min_y, c.y);
            max_y = std::max<int>(max_y, c.y);
        }
        const int w = max_x - min_x + 1, h = max_y - min_y + 1;
        if (w > BODY_ATLAS_W || h > BODY_ATLAS_H) continue;
        if (shelf_x + w > BODY_ATLAS_W) {
            shelf_x = 0;
            shelf_y += shelf_h;
            shelf_h = 0;
        }
        if (shelf_y + h > BODY_ATLAS_H) continue;  // full this frame: drawn as cells

        const gfx::Rect src{shelf_x, shelf_y, w, h};
        for (int y = 0; y < h; ++y)
            std::fill_n(atlas.begin() +
                            (static_cast<std::ptrdiff_t>(shelf_y + y) * BODY_ATLAS_W + shelf_x),
                        w, 0u);
        const uint8_t tag = g.get_element(at[0] % grid_w, at[0] / grid_w).piece_tag;
        for (size_t k = 0; k < shape.size(); ++k) {
            const int ax = shelf_x + shape[k].x - min_x, ay = shelf_y + shape[k].y - min_y;
            atlas[static_cast<size_t>(ay) * BODY_ATLAS_W + ax] = pixels[static_cast<size_t>(at[k])];
            hidden.push_back(fill_of(at[k], tag));
        }
        shelf_x += w;
        shelf_h = std::max(shelf_h, h);

        const Grid::BodyState s = g.body_state(i);
        const float lcx = fx::to_float(s.shape_cx), lcy = fx::to_float(s.shape_cy);
        float cx = fx::to_float(s.prev_x) + (fx::to_float(s.x) - fx::to_float(s.prev_x)) * alpha;
        float cy = fx::to_float(s.prev_y) + (fx::to_float(s.y) - fx::to_float(s.prev_y)) * alpha;
        float th = fx::to_float(s.prev_theta) +
                   (fx::to_float(s.theta) - fx::to_float(s.prev_theta)) * alpha;

        // Easing onto its cells before it becomes them: see SETTLE_STEPS. Lying
        // near square, it will be set down square (Grid::REST_SQUARE_ANGLE), and
        // the body says where; drawn there, the sprite is those cells pixel for
        // pixel. Otherwise it rests at an angle, and its cells are a staircase no
        // turned picture matches; it keeps its angle and eases only onto the
        // offset that fits them best, found from the cells themselves.
        //
        // Done a step early, so the last frames before the hand-back are already
        // exactly the pose it hands back in -- and at an exact quarter turn, which
        // the renderer draws without resampling. A hair off one, it resamples, and
        // every edge of the picture can land a screen pixel from the cell's.
        float angle_deg = th * 57.29578f;
        const float ease =
            1.0f - (static_cast<float>(s.steps_to_sleep) - 1.0f - alpha) / SETTLE_STEPS;
        if (ease > 0.0f) {
            float target = th, tx = 0.0f, ty = 0.0f;
            if (s.squares) {
                target = fx::to_float(s.rest_theta);
                tx = fx::to_float(s.rest_x);
                ty = fx::to_float(s.rest_y);
            } else {
                const float c = std::cos(th), sn = std::sin(th);
                for (size_t k = 0; k < shape.size(); ++k) {
                    const float dx = static_cast<float>(shape[k].x) + 0.5f - lcx;
                    const float dy = static_cast<float>(shape[k].y) + 0.5f - lcy;
                    // Cell index to column and row; the division is meant to truncate.
                    const auto col = at[k] % grid_w, row = at[k] / grid_w;
                    tx += static_cast<float>(col) + 0.5f - (dx * c - dy * sn);
                    ty += static_cast<float>(row) + 0.5f - (dx * sn + dy * c);
                }
                tx /= static_cast<float>(shape.size());
                ty /= static_cast<float>(shape.size());
            }
            if (ease >= 1.0f) {
                cx = tx;
                cy = ty;
                angle_deg = s.squares ? 90.0f * static_cast<float>(fx::quarters(s.rest_theta))
                                      : th * 57.29578f;
            } else {
                const float smooth = ease * ease * (3.0f - 2.0f * ease);
                cx += (tx - cx) * smooth;
                cy += (ty - cy) * smooth;
                angle_deg = (th + (target - th) * smooth) * 57.29578f;
            }
        }

        out.push_back(frame::BodySprite{src, cx, cy, lcx - static_cast<float>(min_x),
                                        lcy - static_cast<float>(min_y), angle_deg});
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
    status +=
        "  FOES:" + std::to_string(run.enemies_alive()) + "  KILLS:" + std::to_string(run.kills());
    return status;
}

std::string diagnostics(const Run& run, int fps, ElementType brush, int brush_size) {
    // Rebuilt every frame rather than cached for a second. The brush name and the
    // awake-chunk count are answers to "what did that key just do" and "has the
    // world settled yet"; cached, a hotbar key leaves the HUD naming the old brush
    // for up to a second. Only the frame rate is a once-a-second quantity, and
    // the caller caches that one.
    std::string text = "FPS:" + std::to_string(fps) + " BRUSH:" + material_of(brush).name + "(" +
                       std::to_string(brush_size) + ")" +
                       " CHUNKS:" + std::to_string(run.grid.active_chunk_count());
    // CHUNKS:0 does not mean the world has stopped. A falling structural piece is
    // carried by the support queue rather than by the chunk rects, so a slab can
    // fall the height of the world with the counter at zero. A flag rather than a
    // count because the queue's length is a number of seeds, not of pieces.
    if (run.grid.has_pending_support_checks()) text += "+FALLING";
    return text;
}

void stand_backdrop_anchor(frame::Backdrop& backdrop, int padded_w, int padded_h, int world_w,
                           int world_h) {
    // Horizontally the world's centre -- every layer of a standing-anchored set
    // wraps, so this only chooses which columns line up where.
    backdrop.anchor_x = 0.5f * static_cast<float>(std::max(0, world_w - padded_w));
    backdrop.anchor_y = depth_rig::standing_anchor_y(backdrop.rig, Player::HEIGHT, padded_h,
                                                     Camera::VERTICAL_ANCHOR, world_h);
}

void paint_spray(const Run& run, float alpha, std::vector<frame::SprayDrop>& out) {
    out.clear();
    for (const Grid::Spray& d : run.grid.spray()) {
        const float x =
            fx::to_float(d.prev_x) + (fx::to_float(d.x) - fx::to_float(d.prev_x)) * alpha;
        const float y =
            fx::to_float(d.prev_y) + (fx::to_float(d.y) - fx::to_float(d.prev_y)) * alpha;
        // The same floor as a thin cell, so a small drop still reads as wet.
        constexpr int FLOOR_ALPHA = 48;
        const int m = d.mass < Grid::LIQUID_FULL ? d.mass : Grid::LIQUID_FULL;
        const uint32_t a =
            static_cast<uint32_t>(FLOOR_ALPHA + m * (255 - FLOOR_ALPHA) / Grid::LIQUID_FULL);
        out.push_back({x, y, (a << 24) | (d.color & 0x00FFFFFFu)});
    }
}

} // namespace present
