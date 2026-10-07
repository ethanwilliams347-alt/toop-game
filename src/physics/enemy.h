#pragma once
#include <array>
#include <cstdint>
#include "enemy_art.h"
#include "fixed.h"
#include "grid.h"
#include "player.h"

// A body that comes apart where it is hit.
//
// The same kind of thing as the Player -- a box that lives outside the cell grid,
// with its own position and velocity, colliding against is_solid -- plus one
// thing the player does not have: the sprite is not a picture laid over the box
// but a set of pixels the body actually consists of, each of which can stop
// existing. When one does, it is written into the grid as a grain of Sand in the
// pixel's own colour, at the cell the pixel was covering. From there it is
// ordinary matter: it falls, piles, sinks in water and buries things, and nothing
// about it remembers having been an arm.
//
// The pixel-to-cell mapping is one to one because the art is drawn at one BMP
// pixel per world cell, like everything else in assets/. That is the whole reason
// a hit can be local: the arrow lands on a cell, the cell names a pixel, and the
// bite is a disc of pixels around it.
//
// Not a Player with an AI bolted on, although the collision is the same shape.
// Player's speed is a single constant tuned for the character's feel, its jump key
// is a wing beat, and its damage model is two `if`s about health. An enemy walks at
// a different speed, does not fly, and has no health number at all: what it has
// left is what it has left, and it dies when a part it cannot live without is gone.
class Enemy {
public:
    // The player's box, so the art -- drawn on the player's frame -- anchors the
    // same way: bottom-centre, with the arms hanging outside the box's columns.
    static constexpr int WIDTH = Player::WIDTH;
    static constexpr int HEIGHT = Player::HEIGHT;
    static constexpr int FRAME_W = enemy_art::W;
    static constexpr int FRAME_H = enemy_art::H;
    static constexpr int OFFSET_X = (FRAME_W - WIDTH) / 2;
    static constexpr int OFFSET_Y = FRAME_H - HEIGHT;

    // --- movement, in cells per second like Player's ---------------------
    //
    // A wander and a chase, both well under the player's MOVE_SPEED: the player has
    // to be able to walk away from one, and has to be able to stand still and line
    // up a shot at one that is coming. The chase is the faster of the two so that
    // noticing you reads as a change of gait rather than as nothing.
    static constexpr fx::v PATROL_SPEED = fx::from_int(22);
    static constexpr fx::v CHASE_SPEED = fx::from_int(45);

    // A hop, not the player's leap: enough to clear the lip of a dug trench or a
    // sand pile taller than MAX_STEP_HEIGHT, which is all it is for. Same gravity
    // as the player, so the two bodies fall alike and a shove of sand drops both the
    // same way.
    static constexpr fx::v JUMP_SPEED = fx::from_int(120);
    static constexpr fx::v GRAVITY = Player::GRAVITY;
    static constexpr fx::v MAX_FALL_SPEED = Player::MAX_FALL_SPEED;
    static constexpr int MAX_STEP_HEIGHT = Player::MAX_STEP_HEIGHT;

    // The bottom rows of the box in which powder collides. Above them a falling
    // grain passes through the body -- see overlaps_solid. One more than a step,
    // so a pile the body could climb is always inside the band that sees it.
    static constexpr int FOOTING_ROWS = MAX_STEP_HEIGHT + 1;

    // How close the player has to be, per axis and centre to centre, before it is
    // noticed. A dozen body widths: well inside the screen at every scale the
    // shipped scenes use (half the view at the zoomed-in 10x scenes, a fifth of it
    // at 4x), because an enemy that starts chasing from off-screen is one the
    // player never saw decide to.
    static constexpr int NOTICE_X = 12 * WIDTH;
    static constexpr int NOTICE_Y = 3 * HEIGHT;

    // While wandering it turns back at a drop deeper than this rather than walking
    // off it. A chase does not check -- following you off a ledge is the point.
    static constexpr int LEDGE_DROP = HEIGHT;

    // --- what it does to the player ---------------------------------------
    //
    // A swipe, landed when the boxes touch, and only while it still has an arm to
    // swipe with. Rate-limited for the reason Player's burn is: per-step contact
    // damage empties the bar before the player has seen what hit them.
    static constexpr int SWIPE_DAMAGE = 8;
    static constexpr int SWIPE_INTERVAL_STEPS = 40;

    // --- what kills it ----------------------------------------------------
    //
    // Losing the whole head, the whole heart (see enemy_art.h), or both feet --
    // both legs cut through, so there is nothing left to stand on and the rest
    // comes down as a heap -- or
    // falling below this share of the pixels it was born with. The last one is the
    // "it is mostly sand now" rule, and it is what stops a body that has been
    // whittled to a torso and one shoulder walking about as a floating lump.
    static constexpr int COLLAPSE_PERCENT = 40;

    // Heat eats it from wherever the heat is touching. Same threshold and rate as
    // the player's burn, so standing two bodies in one fire hurts both on the same
    // clock -- but where the player loses a number, this loses the pixels that are
    // in the flame, so the feet go first when it walks into a fire.
    static constexpr uint8_t BURN_TEMPERATURE = Player::BURN_TEMPERATURE;
    static constexpr int BURN_INTERVAL_STEPS = Player::BURN_INTERVAL_STEPS;

    // At most this many pixels burn per tick, lowest first. Without the cap, a
    // fire three cells deep takes every pixel in it on the first tick, which is
    // both feet at once and a body that is simply gone the step it touches a flame
    // -- no burning, just a disappearance. Two per tick is a body that visibly
    // smoulders from the ground up for most of a second before the feet give way.
    static constexpr int BURN_PIXELS_PER_TICK = 2;

    // A dead slot. Run keeps a fixed pool of these so nothing on the step path
    // allocates; spawn() brings one to life.
    Enemy() = default;

    // Puts a whole, fresh body with its box's top-left at (x, y).
    void spawn(int x, int y);

    bool is_alive() const { return alive; }

    // Advances one fixed step, after the grid. `target_*` is the player's box
    // top-left; `target_alive` stops a dead player being chased about. Mutable grid
    // because heat can take pixels and pixels become sand.
    //
    // Returns true on the step a swipe lands, and leaves the damage to the caller:
    // the enemy does not hold a Player& for the same reason Player does not hold a
    // Grid& it can write to.
    bool update(Grid& grid, int target_x, int target_y, bool target_alive);

    // The frame pixel covering world cell (wx, wy), as an index into the frame
    // (y * FRAME_W + x), or -1 if no surviving pixel is there. What an arrow asks
    // as it passes through each cell.
    int pixel_at(int wx, int wy) const;

    // Takes out every surviving pixel within `radius` cells of world cell (wx, wy),
    // then everything the bite cut off from the heart, then -- if that killed it --
    // the rest. Each pixel lost goes into the grid as sand. Returns the number of
    // pixels lost.
    int shatter(Grid& grid, int wx, int wy, int radius);

    // Top-left of the box, in cells.
    int cell_x() const { return pos_x; }
    int cell_y() const { return pos_y; }
    int center_x() const { return pos_x + WIDTH / 2; }
    int center_y() const { return pos_y + HEIGHT / 2; }

    // The sub-cell remainder and last step's box, for the renderer's interpolation
    // and nothing else -- the same arrangement as Player::visual_x, except that the
    // float conversion happens on the render side rather than in this header.
    fx::v remainder_x() const { return rem_x; }
    fx::v remainder_y() const { return rem_y; }
    int prev_cell_x() const { return prev_x; }
    int prev_cell_y() const { return prev_y; }
    fx::v prev_remainder_x() const { return prev_rem_x; }
    fx::v prev_remainder_y() const { return prev_rem_y; }

    bool facing_left() const { return face_left; }
    bool is_chasing() const { return chasing; }

    // Whether frame pixel (x, y) is still there. The renderer builds the sprite from
    // this and enemy_art's colours, so what is drawn is exactly what an arrow can
    // hit, with no second copy of the body to drift from it.
    bool has_pixel(int x, int y) const { return pixels[y * FRAME_W + x] != 0; }
    int pixel_count() const { return remaining; }

    bool has_arms() const;
    bool has_feet() const;

    // True if a box at (px, py) would overlap anything solid -- static matter
    // anywhere in the box, powder only in its FOOTING_ROWS. Public for the same
    // reason Player's is.
    bool overlaps_solid(const Grid& grid, int px, int py) const;

private:
    bool alive = false;
    int pos_x = 0;
    int pos_y = 0;
    fx::v rem_x = 0;
    fx::v rem_y = 0;
    fx::v vel_x = 0;
    fx::v vel_y = 0;
    bool on_ground = false;
    bool face_left = false;
    bool chasing = false;

    int prev_x = 0;
    int prev_y = 0;
    fx::v prev_rem_x = 0;
    fx::v prev_rem_y = 0;

    int swipe_timer = 0;
    int burn_timer = 0;

    // One byte per frame pixel: 1 while it exists. A fixed array on the body rather
    // than a vector, so a pool of enemies is one allocation-free block and the step
    // never touches the heap.
    std::array<uint8_t, enemy_art::W * enemy_art::H> pixels{};
    int remaining = 0;

    // Where frame pixel (x, y) is in the world, given the facing. The renderer
    // flips the sprite with SDL_FLIP_HORIZONTAL over the same frame rect, so the two
    // agree: frame column x lands on world column (left + W-1-x) when facing left.
    int world_x_of(int x) const;
    int world_y_of(int y) const;

    // Removes one pixel and writes its grain. A pixel whose cell is already solid --
    // a foot buried in the step it is climbing -- just goes: there is nowhere for the
    // grain to be, and conjuring room would push the world around.
    void crumble(Grid& grid, int index);

    // Everything no longer connected to the heart comes off, and then the death
    // rules are applied. Called after anything that removes pixels.
    void settle_after_loss(Grid& grid);

    int climb_for(const Grid& grid, int sign) const;
    bool drop_ahead_is_deep(const Grid& grid, int sign) const;
    void move_x(const Grid& grid, int amount);
    void move_y(const Grid& grid, int amount);
};

// Speeds are lengths per unit time and the body is the player's, so the two
// relationships the comments above lean on are checked against Player rather
// than trusted.
static_assert(Enemy::CHASE_SPEED < Player::MOVE_SPEED,
              "an enemy that chases faster than the player walks cannot be walked "
              "away from - the escape is the whole counterplay to being noticed");
static_assert(Enemy::PATROL_SPEED < Enemy::CHASE_SPEED,
              "the chase should read as a change of gait from the wander");
static_assert(Enemy::OFFSET_X >= 0 && Enemy::OFFSET_Y >= 0,
              "the enemy frame is smaller than the box it is anchored to");
static_assert(Enemy::OFFSET_X == enemy_art::BOX_LEFT,
              "enemy_art::BOX_LEFT has drifted from the box the art is anchored to, so "
              "has_arms() is counting the wrong columns as arms");
