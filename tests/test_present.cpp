// The presenter (present/present.h): what the frame shows, from a Run.
//
// This was the inside of main()'s frame loop, where the only check on it was a
// person looking at the window. The one property worth most here is the one the
// enemy atlas exists for -- what is drawn in a cell is what an arrow finds
// there -- and it can be checked cell by cell now that the painting writes a
// plain buffer: every painted atlas pixel, placed in the world the way
// frame.cpp places it (the sprite's offset, mirrored when facing left), must be
// a cell where Enemy::pixel_at finds a surviving pixel, and every unpainted one
// a cell where it finds none.
//
// Links SDL only for frame.h's record types; nothing here opens a window or
// calls SDL.
//
// SDL_MAIN_HANDLED for the same reason as golden_frame_test: present.h reaches
// SDL.h through render/frame.h, and on Windows SDL.h otherwise #defines `main`
// to `SDL_main` and expects SDL2main, which no test links, to supply the real
// one. Without it the link fails on an unresolved `main` -- on Windows only,
// since SDL leaves `main` alone elsewhere. It has to come before every include
// that can reach SDL.h.
#define SDL_MAIN_HANDLED
#include "game/boot.h"
#include "present/present.h"
#include "test_util.h"

#include <cmath>
#include <string>

namespace {

void build_floor(Grid& grid, int top) {
    for (int y = top; y < grid.get_height(); ++y)
        for (int x = 0; x < grid.get_width(); ++x) grid.set_element(x, y, ElementType::Wall);
}

// The world cell frame.cpp draws atlas pixel (sx, sy) of a sprite at. Integer
// cells, from the body's own cell rather than the interpolated position: the
// property is about the rectangle's anchoring and its flip, which interpolation
// moves as a whole.
void drawn_cell(const Enemy& en, const frame::EnemySprite& es, int sx, int sy, int& wx, int& wy) {
    const int col = es.facing_left ? es.src.w - 1 - sx : sx;
    wx = en.cell_x() - es.offset_x + col;
    wy = en.cell_y() - es.offset_y + sy;
}

void test_drawn_is_hit() {
    Run run(240, 120, 3);
    build_floor(run.grid, 100);
    run.player = Player(116, 100 - Player::HEIGHT);
    const Species* kinds[] = {&species::GHOUL, &species::TROLL, &species::GHOUL, &species::FISH};
    const int columns[] = {40, 170, 205, 72};
    for (int i = 0; i < 4; ++i) {
        const int y = boot::standing_y(run.grid, columns[i], *kinds[i]);
        check("present: the fixture's enemies stand",
              y >= 0 && run.spawn_enemy(columns[i], y, *kinds[i]));
    }
    // Long enough for each to see the player and turn to it, and to walk -- so
    // poses are not the rest frame and both facings are on screen.
    for (int s = 0; s < 40; ++s) run.step(Input{});

    std::vector<uint32_t> atlas;
    std::vector<frame::EnemySprite> sprites;
    present::paint_enemies(run, 1.0f, atlas, sprites);
    check("present: one sprite per living enemy",
          static_cast<int>(sprites.size()) == run.enemies_alive(),
          std::to_string(sprites.size()) + " sprites, " + std::to_string(run.enemies_alive()) +
              " alive");

    bool seen_left = false, seen_right = false, agree = true, painted_any = false;
    std::string where;
    size_t k = 0;
    for (int slot = 0; slot < Run::MAX_ENEMIES && k < sprites.size(); ++slot) {
        const Enemy& en = run.enemies[static_cast<size_t>(slot)];
        if (!en.is_alive()) continue;
        const frame::EnemySprite& es = sprites[k++];
        (es.facing_left ? seen_left : seen_right) = true;
        check("present: a sprite's rectangle is inside its own slot",
              es.src.x == (slot % present::ENEMY_SLOT_COLS) * present::ENEMY_SLOT_W &&
                  es.src.y == (slot / present::ENEMY_SLOT_COLS) * present::ENEMY_SLOT_H &&
                  es.src.w <= present::ENEMY_SLOT_W && es.src.h <= present::ENEMY_SLOT_H);
        for (int sy = 0; sy < es.src.h; ++sy) {
            for (int sx = 0; sx < es.src.w; ++sx) {
                const uint32_t px =
                    atlas[static_cast<size_t>(es.src.y + sy) * present::ENEMY_ATLAS_W +
                          static_cast<size_t>(es.src.x + sx)];
                int wx = 0, wy = 0;
                drawn_cell(en, es, sx, sy, wx, wy);
                const bool hit = en.pixel_at(wx, wy) >= 0;
                painted_any |= px != 0u;
                if ((px != 0u) != hit && agree) {
                    agree = false;
                    where = "slot " + std::to_string(slot) + " atlas (" + std::to_string(sx) +
                            ", " + std::to_string(sy) + ") -> world (" + std::to_string(wx) + ", " +
                            std::to_string(wy) +
                            "): " + (hit ? "hit but not drawn" : "drawn but not hit");
                }
            }
        }
    }
    check("present: the atlas has bodies in it", painted_any);
    check("present: every painted cell is a cell an arrow hits, and every other is not", agree,
          where);
    check("present: the fixture shows both facings", seen_left && seen_right);
}

void test_arrows_and_words() {
    Run run(240, 120, 3);
    build_floor(run.grid, 100);
    run.player = Player(116, 100 - Player::HEIGHT);
    const int y = boot::standing_y(run.grid, 200, species::GHOUL);
    run.spawn_enemy(200, y, species::GHOUL);
    run.set_objective(60, 90);

    Input in;
    in.shoot = true;
    in.cursor_x = 200;
    in.cursor_y = 60;
    for (int s = 0; s < 3; ++s) run.step(in);

    std::vector<frame::ArrowSprite> arrows;
    present::arrows(run, 0.5f, arrows);
    bool unit = !arrows.empty();
    for (const frame::ArrowSprite& a : arrows)
        unit = unit && std::fabs(a.dir_x * a.dir_x + a.dir_y * a.dir_y - 1.0f) < 1e-3f;
    check("present: an arrow in flight is drawn, pointing along its velocity", unit,
          std::to_string(arrows.size()) + " arrows");

    const std::string readout = present::run_readout(run);
    check("present: the readout leads with HP", readout.rfind("HP:", 0) == 0, readout);
    check("present: ...bears on the objective, west of the body",
          readout.find("GOAL:") != std::string::npos &&
              readout.find("W  FOES:1") != std::string::npos,
          readout);
    const std::string diag = present::diagnostics(run, 60, ElementType::Sand, 3);
    check("present: the diagnostics name the frame rate and the brush",
          diag.rfind("FPS:60 BRUSH:", 0) == 0 && diag.find("(3)") != std::string::npos, diag);
}

}  // namespace

int main() {
    test_drawn_is_hit();
    test_arrows_and_words();
    return report();
}
