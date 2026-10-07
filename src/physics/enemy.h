#pragma once
#include <array>
#include <cstdint>
#include "body_art.h"
#include "enemy_art.h"
#include "fixed.h"
#include "grid.h"
#include "player.h"
#include "troll_art.h"

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
// One class for every kind of enemy. What differs between a ghoul and a troll --
// the art, the box, the gait, the attack -- is a Species, a constexpr table the
// body holds a pointer to; what is the same -- that a hit is a bite of pixels,
// that a bite severs what it cuts off from the heart, that every pixel lost is a
// grain of sand -- is this class, once. A second class per species would be a
// second copy of the part of this file that has to be right.
//
// Not a Player with an AI bolted on, although the collision is the same shape.
// Player's speed is a single constant tuned for the character's feel, its jump key
// is a wing beat, and its damage model is two `if`s about health. An enemy walks at
// a different speed, does not fly, and has no health number at all: what it has
// left is what it has left, and it dies when a part it cannot live without is gone.
// How a species attacks.
//
// Swipe: the ghoul's. Lands the step the boxes touch, rate-limited, nothing to
// see coming -- it is small and the damage is small.
//
// Slam: the troll's. It stops, winds up for `windup_steps`, and brings the club
// down on the ground in front of it -- hurting whatever is standing there and
// shaking the ground itself loose into sand -- then stands spent for
// `attack_interval` steps. The wind-up is the counterplay a big hit needs: a blow
// that takes a third of the bar has to be one the player saw coming and could
// have stepped out of, and the recovery is the window to punish it in. It is
// the Elden Ring troll's rhythm, and it is the thing that makes a body this size
// a fight rather than a wall that hurts.
enum class Attack : uint8_t { Swipe, Slam };

// Everything that differs between kinds of enemy. Velocities in cells/second,
// times in fixed steps, lengths in cells, as for Player.
struct Species {
    const body_art::Art* art;
    int pixel_count;

    // The collision box. The art anchors on it bottom-centre, like the player's.
    int width;
    int height;

    fx::v patrol_speed;
    fx::v chase_speed;
    fx::v jump_speed;
    int max_step_height;

    // How close the player has to be, per axis and centre to centre, before it
    // is noticed; and the drop a wandering body turns back at rather than walking
    // off. A chase does not check -- following you off a ledge is the point.
    int notice_x;
    int notice_y;
    int ledge_drop;

    Attack attack;
    int damage;
    // Swipe: steps between swipes. Slam: steps it stands spent after one lands.
    int attack_interval;
    // Slam only: steps from deciding to slam to the club landing.
    int windup_steps;
    // How far past the front of the box the attack lands, in cells.
    int reach;
    // Slam only: the radius of ground shaken loose where the club lands.
    int crush_radius;

    // Below this share of the pixels it was born with, it collapses -- the "it is
    // mostly sand now" rule.
    int collapse_percent;
    // At most this many pixels burn per burn tick, lowest first.
    int burn_pixels_per_tick;

    // Powder does not stop it: it stands on whatever is under a drift and
    // shoves the drift's grains out of its box, every step, instead of climbing
    // a pile or waiting to be dug out of one. See Enemy::shove_powder.
    bool wades;

    constexpr int frame_w() const { return art->w; }
    constexpr int frame_h() const { return art->h; }
    constexpr int offset_x() const { return (art->w - width) / 2; }
    constexpr int offset_y() const { return art->h - height; }

    // The bottom rows of the box in which powder collides. Above them a falling
    // grain passes through the body -- see Enemy::overlaps_solid. One more than a
    // step, so a pile the body could climb is always inside the band that sees it.
    constexpr int footing_rows() const { return max_step_height + 1; }
};

namespace species {

// The moss ghoul: the player's box and the player's scale, a wander and a chase
// both under the player's walk, and a swipe when it touches you.
inline constexpr Species GHOUL{
    &enemy_art::ART,
    enemy_art::PIXEL_COUNT,
    Player::WIDTH,
    Player::HEIGHT,
    // A wander and a chase, both well under the player's MOVE_SPEED: the player
    // has to be able to walk away from one, and has to be able to stand still and
    // line up a shot at one that is coming. The chase is the faster of the two so
    // that noticing you reads as a change of gait rather than as nothing.
    fx::from_int(22),
    fx::from_int(45),
    // A hop, not the player's leap: enough to clear the lip of a dug trench or a
    // sand pile taller than a step, which is all it is for.
    fx::from_int(120),
    Player::MAX_STEP_HEIGHT,
    // A dozen body widths: well inside the screen at every scale the shipped
    // scenes use (half the view at the zoomed-in 10x scenes, a fifth of it at
    // 4x), because an enemy that starts chasing from off-screen is one the player
    // never saw decide to.
    12 * Player::WIDTH,
    3 * Player::HEIGHT,
    Player::HEIGHT,
    // A swipe, landed when the boxes touch, and only while it still has an arm
    // to swipe with. Rate-limited for the reason Player's burn is: per-step
    // contact damage empties the bar before the player has seen what hit them.
    // Reach is the arms' overhang either side of the box, so a swipe lands where
    // the drawn claws are rather than where the collision box is.
    Attack::Swipe,
    8,
    40,
    0,
    enemy_art::BOX_LEFT,
    0,
    // Losing the whole head, the whole heart, or both feet kills it, or falling
    // below this share of its pixels: what stops a body whittled to a torso and
    // one shoulder walking about as a floating lump.
    40,
    // Without a cap, a fire three cells deep takes every pixel in it on the first
    // tick, which is both feet at once and a body that is simply gone the step it
    // touches a flame. Two per tick is a body that visibly smoulders from the
    // ground up for most of a second before the feet give way.
    2,
    false,
};

// The troll: nearly three times the player's height, slow, and hitting hard
// enough that the fight is about not being under the club when it lands.
inline constexpr Species TROLL{
    &troll_art::ART,
    troll_art::PIXEL_COUNT,
    // The torso and legs. The arms and the club hang outside it, as the ghoul's
    // claws do; an arrow finds them, terrain does not.
    24,
    66,
    // Slower than the ghoul both ways. It does not need to be fast: its reach is
    // two of the player's bodies, and a chase that closes slowly is what gives
    // the player time to look up at it.
    fx::from_int(14),
    fx::from_int(32),
    // Enough to heave itself up a ledge of about twenty cells, under the player's
    // gravity. A troll that a two-body ledge stops dead is one the player farms
    // from the top of it.
    fx::from_int(140),
    // Its legs step over what would stop the player: a mound of sand, a fallen
    // plank, the lip of a crater it made itself.
    6,
    // It sees farther because it is taller -- and because its slam is slow, it
    // has to start closing in sooner to be a threat at all.
    20 * Player::WIDTH,
    90,
    33,
    Attack::Slam,
    // A third of the bar. Survivable twice; the third is the lesson.
    30,
    // Spent after a slam for most of a second: the window to get a shot into
    // the eyes, or to get out from under it.
    50,
    // Seven-tenths of a second from deciding to landing. Long enough to see and
    // walk out of at the player's speed (more than eighty cells), short enough
    // that standing still to aim through it is a decision with a price.
    42,
    // The club's length past the front of the box, which is the fist's overhang
    // plus the club swung out at arm's length: everything from the troll's front
    // edge to here is under it. Two and a half of the player's bodies.
    20,
    // The crater. Radius 5 is a bite eleven cells across out of whatever it
    // lands on: visible, enough to break a thin floor or a plank bridge, not
    // enough to dig the troll a pit to fall into in a few swings. At 4 the hole
    // sat under the club's own head and read as nothing.
    5,
    40,
    // Twice the ghoul's: a body ten times the size smouldering at the ghoul's
    // rate would stand in a fire for a minute.
    4,
    // Its own arm, shot off, is a pile of a few hundred grains around its feet,
    // far deeper than any step. Colliding with it, the first version stood
    // buried in it for the rest of the run -- the fight ended in a statue -- and
    // once it could climb out, it stood perched on the cone's tip with its legs
    // in the air. A body this heavy goes through sand, not over it.
    true,
};

}  // namespace species

class Enemy {
public:
    // The largest frame any species has. Every pool slot carries a pixel mask
    // this big, whichever species is in it, so a slot can hold any species and
    // the pool stays one fixed, allocation-free block.
    static constexpr int MAX_FRAME_W = troll_art::W;
    static constexpr int MAX_FRAME_H = troll_art::H;
    static constexpr int MAX_FRAME_PIXELS = MAX_FRAME_W * MAX_FRAME_H;

    // Shared by every species: the same gravity as the player, so bodies fall
    // alike and a shove of sand drops them all the same way; and the player's
    // burn threshold and clock, so standing two bodies in one fire hurts both on
    // the same clock -- but where the player loses a number, this loses the
    // pixels that are in the flame, so the feet go first when it walks into a
    // fire.
    static constexpr fx::v GRAVITY = Player::GRAVITY;
    static constexpr fx::v MAX_FALL_SPEED = Player::MAX_FALL_SPEED;
    static constexpr uint8_t BURN_TEMPERATURE = Player::BURN_TEMPERATURE;
    static constexpr int BURN_INTERVAL_STEPS = Player::BURN_INTERVAL_STEPS;

    // A dead slot. Run keeps a fixed pool of these so nothing on the step path
    // allocates; spawn() brings one to life.
    Enemy() = default;

    // Puts a whole, fresh body of the given species with its box's top-left at
    // (x, y).
    void spawn(int x, int y, const Species& kind = species::GHOUL);

    bool is_alive() const { return alive; }
    const Species& species() const { return *kind; }

    // Advances one fixed step, after the grid. `target_*` is the player's box
    // top-left; `target_alive` stops a dead player being chased about. Mutable grid
    // because heat can take pixels, pixels become sand, and a slam shakes the
    // ground loose.
    //
    // Returns true on the step an attack lands on the target, and leaves the
    // damage (species().damage) to the caller: the enemy does not hold a Player&
    // for the same reason Player does not hold a Grid& it can write to.
    bool update(Grid& grid, int target_x, int target_y, bool target_alive);

    // The frame pixel covering world cell (wx, wy), as an index into the frame
    // (y * frame_w + x), or -1 if no surviving pixel is there. What an arrow asks
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
    int center_x() const { return pos_x + kind->width / 2; }
    int center_y() const { return pos_y + kind->height / 2; }

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

    // Steps until a slam that is winding up lands, or 0 when none is. Public for
    // the renderer's telegraph -- the eyes flare as it winds up -- and for tests.
    int windup_left() const { return windup; }

    // Whether frame pixel (x, y) is still there. The renderer builds the sprite from
    // this and the art's colours, so what is drawn is exactly what an arrow can
    // hit, with no second copy of the body to drift from it.
    bool has_pixel(int x, int y) const { return pixels[y * kind->frame_w() + x] != 0; }
    int pixel_count() const { return remaining; }

    bool has_arms() const;
    bool has_feet() const;

    // True if a box at (px, py) would overlap anything solid -- static matter
    // anywhere in the box, powder only in its footing rows. Public for the same
    // reason Player's is.
    bool overlaps_solid(const Grid& grid, int px, int py) const;

private:
    const Species* kind = &species::GHOUL;
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

    // Swipe: steps until it may swipe again. Slam: steps it stands spent.
    int attack_timer = 0;
    // Slam only: steps until the club lands, 0 when not winding up.
    int windup = 0;
    int burn_timer = 0;

    // One byte per frame pixel: 1 while it exists, indexed y * frame_w + x for
    // the species' own frame. A fixed array on the body rather than a vector, so
    // a pool of enemies is one allocation-free block and the step never touches
    // the heap.
    std::array<uint8_t, MAX_FRAME_PIXELS> pixels{};
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

    // The attack, decided and landed. Returns true on the step it lands on the
    // target.
    bool attack(Grid& grid, int target_x, int target_y, bool target_alive);
    bool target_in_reach(int target_x, int target_y) const;
    void slam(Grid& grid);
    void shove_powder(Grid& grid);

    int climb_for(const Grid& grid, int sign) const;
    bool drop_ahead_is_deep(const Grid& grid, int sign) const;
    void move_x(const Grid& grid, int amount);
    void move_y(const Grid& grid, int amount);
};

// Speeds are lengths per unit time and the ghoul's body is the player's, so the
// relationships the comments above lean on are checked rather than trusted --
// for every species, since a new one is a new row of numbers that can break them.
constexpr bool species_is_sound(const Species& s) {
    return s.chase_speed < Player::MOVE_SPEED && s.patrol_speed < s.chase_speed &&
           s.offset_x() >= 0 && s.offset_y() >= 0 && s.offset_x() == s.art->box_left &&
           s.art->w <= Enemy::MAX_FRAME_W && s.art->h <= Enemy::MAX_FRAME_H &&
           s.footing_rows() <= s.height;
}
static_assert(species_is_sound(species::GHOUL) && species_is_sound(species::TROLL),
              "a species breaks one of: chasing slower than the player walks (the "
              "escape is the whole counterplay to being noticed), wandering slower "
              "than it chases, a frame at least as big as its box, the art's "
              "box_left matching the box it is anchored to (or has_arms counts the "
              "wrong columns), or a frame that fits Enemy::MAX_FRAME_*");

// The troll is meant to be imposing, and "imposing" is a ratio: well over twice
// the player's height, or it is a big ghoul.
static_assert(species::TROLL.height * 2 > Player::HEIGHT * 5,
              "the troll is no longer two and a half times the player's height");

// The slam's wind-up has to be escapable: at the player's walk, the wind-up has
// to carry the player out past the club's reach from anywhere under it.
static_assert(fx::per_step(Player::MOVE_SPEED) * species::TROLL.windup_steps >
                  fx::from_int(species::TROLL.reach + species::TROLL.width),
              "the troll's slam winds up too fast to walk out from under");

// And it has to reach past the ghoul-sized gap a player would stand in to shoot
// it from the front: a slam that cannot reach a body standing against its front
// edge is a troll the player hugs.
static_assert(species::TROLL.reach >= Player::WIDTH,
              "the troll's slam cannot reach a player standing against it");

// Enemy::slam puts the crater as far out along the reach as keeps it inside,
// and throws debris up to 2r+2 cells either side of its centre. This is what
// keeps the near-side debris off the troll's own feet: closer, and every slam
// would drop grains into its footing rows and lift it out of its own crater.
static_assert(species::TROLL.reach >= 3 * species::TROLL.crush_radius + 4,
              "the troll's crater is too big for its reach: the debris thrown back "
              "toward it would land in its own footing");
