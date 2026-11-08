#pragma once
#include <array>
#include "fixed.h"
#include "box_body.h"
#include "grid.h"

// What the player is being told to do this step.
//
// Deliberately plain bools rather than SDL key codes: the simulation stays free
// of any SDL dependency, so the player is testable headlessly for the same
// reason the grid is, and main.cpp remains the only file that knows a keyboard
// exists.
//
// Movement only. Aiming and digging are the tools' inputs, not the body's: the
// dig tool and the bow take the cursor from Input directly (Run::step), and a
// body that never writes to the grid has no use for either.
struct PlayerInput {
    bool left = false;
    bool right = false;
    bool jump = false;
};

// A rigid body that lives outside the cell grid.
//
// Everything else in this engine is a cell that gets stepped in place. A cell
// can only move one step per frame in one of eight directions, which is fine
// for sand and useless for a character that needs sub-cell speed, a jump arc,
// and a body several cells tall that must stay in one piece.
//
// So the player is an axis-aligned box with its own position and velocity that
// only ever reads the grid, asking one question of it: is this cell solid? It
// never writes cells, so it cannot break the rule that all writes go through
// set_element / swap_elements.
class Player {
public:
    // Body size in cells. Sized so the character resolves at the same number of
    // cells as the reference art it is imitating, not merely at the same apparent
    // size on screen: a body built from too few cells makes every interaction with
    // uneven terrain read chunkier, because a two-cell step lip is then a large
    // fraction of the body's height.
    //
    // A hat, if one is ever drawn, overhangs as sprite only. It is not part of the
    // collision box and must not be added to HEIGHT.
    static constexpr int WIDTH = 8;
    static constexpr int HEIGHT = 20;

    // Cells per second, and cells per second squared, in `fx` fixed point. Real
    // units rather than per-step amounts, so the tuning still means the same thing
    // if the fixed step ever changes.
    //
    // Written as exact rationals, never as a float cast. from_ratio(225, 2) is
    // 112.5 and is the same value on every compiler; fx::v(112.5f * 65536) is a
    // float expression a compiler is free to fold its own way. Read the second
    // number as the denominator of the value in TUNING.md.
    //
    // All five are lengths per unit time, so they scale with the body: a speed in
    // cells only means something relative to the size of the thing moving, and
    // holding body-lengths-per-second fixed is what keeps the character feeling the
    // same at a different scale.
    static constexpr fx::v MOVE_SPEED = fx::from_ratio(225, 2);  // 112.5
    static constexpr fx::v JUMP_SPEED = fx::from_int(175);  // ~30 cells, still ~1.5 bodies
    static constexpr fx::v GRAVITY = fx::from_int(500);
    static constexpr fx::v MAX_FALL_SPEED = fx::from_int(400);

    // --- flight ---------------------------------------------------------
    //
    // The character is a bird, so holding the key beats wings rather than doing
    // nothing until the feet are back on the ground. Three constants and an
    // interval, and the relationship between them is the whole feel:
    //
    // A flap is an impulse, not a velocity set. Setting vel_y outright would let
    // one beat cancel a terminal-speed dive, which is the opposite of weight -- a
    // bird arresting a stoop takes several beats and visibly loses height doing it.
    // Subtracting a fixed amount means a fall has to be worked out of, and the
    // number of beats it costs scales with how fast the body was already falling.
    //
    // FLAP_MAX_CLIMB stops that impulse compounding. Without a cap, each beat
    // leaves the body a little faster upward than the last and the climb
    // accelerates without bound.
    //
    // Net altitude per beat is FLAP_IMPULSE against GRAVITY * (FLAP_INTERVAL_STEPS
    // / 60), the gravity a single beat has to pay for. There is no separate flight
    // gravity, so the felt weight of flight is how much of the impulse is left
    // after paying that toll -- which makes the two constants one setting. Raising
    // the impulse alone spends it all on recovering from falls while sustained
    // climbing stays at exactly its old speed, because the cap is what sets that.
    //
    // The cap is deliberately far below JUMP_SPEED: the launch off the ground is
    // one big downstroke and is allowed to exceed it, so a standing jump stays the
    // strongest single upward move the character has. "Climbs under its own power,
    // and is not a helicopter" is a property of the margin rather than of any one
    // constant, so tune the pair together or the character becomes one or the other.
    static constexpr fx::v FLAP_IMPULSE = fx::from_int(177);  // cells/s removed from vel_y per beat
    static constexpr fx::v FLAP_MAX_CLIMB = fx::from_int(98);  // cells/s ceiling on upward speed
    static constexpr int FLAP_INTERVAL_STEPS = 14;  // fixed steps between beats

    // Tallest lip the player walks over without jumping. This is the whole of
    // walking over uneven powder: a settled sand slope is a staircase of one-cell
    // steps, and without this the player would have to jump over every grain.
    //
    // It has to scale with the body, but not by holding the old ratio: a lip that
    // is a quarter of the player's own height is climbed instantly and with no
    // animation, which reads as walking into a settled pile doing nothing. About a
    // curb rather than a table, and still several times the one-cell steps a
    // settled slope is actually made of, so powder stays walkable. The step tests
    // in tests/test_player.cpp hold that side of the trade.
    static constexpr int MAX_STEP_HEIGHT = 3;

    // How far the unstuck search looks for open space when the player ends up
    // inside terrain. See resolve_overlap() for why that happens at all. Scaled
    // with the body: the search has to be able to clear a body-width of material.
    static constexpr int MAX_UNSTUCK_RADIUS = 20;

    // --- health, and the two things that take it away ----------------------
    //
    // Health lives on the body, and the damage is the body asking the grid a
    // question. The grid does not know the player exists, so there is no health
    // field on Grid, no damage column on Element, and nothing in the update loop
    // that knows a player is standing there. Both sources below are reads of the
    // world the body is already in, taken in update() where collision takes its own.
    //
    // Two sources and no damage model. A third source wants a table; two want an
    // `if` each.
    //
    // Integer, like everything else on the simulation path: a threshold is a rule,
    // and a run that kills the player on one machine and not on another is a worse
    // defect than a wrongly tuned threshold.
    static constexpr int MAX_HEALTH = 100;

    // Contact heat, not proximity heat. Damage is taken from the hottest cell the
    // body is actually standing in, which is the same question overlaps_solid asks
    // and is answered against the same box. Air is not simulated -- Empty has
    // conductivity 0 in MATERIALS -- so heat travels through matter in contact and
    // nowhere else, and a body in a room with a fire it is not touching is
    // genuinely not being heated by it. Fire is a Gas, so a flame occupies cells the
    // body can stand in, which is what makes this reachable at all.
    //
    // The threshold is water's boiling point on Element's Celsius-flavoured scale,
    // chosen to sit in a specific gap rather than to feel right: above Steam's spawn
    // temperature, so a puff of steam is not a weapon, and far below Fire's and
    // Charred's, so both hurt with the whole margin to spare. Both halves are
    // asserted at the bottom of this file.
    static constexpr uint8_t BURN_TEMPERATURE = 100;

    // 2 damage every 6 fixed steps is 20 a second, so a body left standing in flame
    // dies in five seconds. A rate rather than per-step damage because per-step is
    // 120 a second and burns a full bar down in under one, which reads as an instant
    // death with no chance to react -- and reacting is the whole content of a hazard.
    static constexpr int BURN_DAMAGE = 2;
    static constexpr int BURN_INTERVAL_STEPS = 6;

    // Fall damage is linear in the speed over a safe landing, not in the height
    // fallen, because the body already carries a speed and does not carry a height.
    // Deriving a fall's distance would mean remembering where it started, which is
    // state that exists only to be turned back into the number vel_y already holds.
    //
    // The safe floor has to clear a standing jump's landing with room -- the arc
    // comes back a step of gravity faster than it left -- or the character takes
    // damage for jumping, which is the one thing a movement game may never do. The
    // assert below holds that.
    //
    // At MAX_FALL_SPEED the damage is half the excess, so a terminal-velocity
    // landing is survivable exactly once from full health. In cells that is a fall
    // of about eight body heights, since v^2 = 2*GRAVITY*h.
    static constexpr fx::v SAFE_FALL_SPEED = fx::from_int(240);
    static constexpr int FALL_DAMAGE_DIVISOR = 2;

    // --- the body among loose bodies, and in water ---------------------------
    //
    // What the player weighs and how hard it pushes, in the loose bodies' own
    // units (Material::weight: stone 26 a cell, wood 6, water 10), because what
    // these numbers are for is the player against a raft or a crate.
    //
    // WEIGHT is what a body the player stands on carries, what it pushes back on
    // a body that shoves it with, and what decides how much of a blow it takes
    // (IMPACT_DAMAGE_DIVISOR). 240 is forty cells of wood: a 30x3 plank raft,
    // which floats with a fifth of itself clear, still floats with the player on
    // it, a little lower and tipped toward where it stands.
    static constexpr int WEIGHT = 240;

    // How dense the body is, in the same units per cell: what it floats or sinks
    // by. A little over water, so the player sinks -- slowly, a sixth of its
    // gravity left once it is under -- and walks the bottom of a pool, and swims
    // up out of one by beating its wings (SWIM_IMPULSE). The bird is not a duck;
    // a pool is a place it can drown in time rather than a floor it stands on.
    // The volume this implies, WEIGHT / DENSITY, is twenty cells: the bird, not
    // the collision box around it.
    static constexpr int DENSITY = 12;

    // How hard the player pushes, as the weight whose pressing down it equals
    // (Grid::BODY_WEIGHT_ACCEL turns it into a force). A crate is held back by
    // its grip on the floor times its weight, so whether a push moves one is
    // this against that: a 10x10 wooden crate (600 weight, 60% grip, 360) slides
    // at about half a walk, an 8x6 stone block (1248 at 70%, 874) creeps,
    // and a 16x12 stone (4992) does not move at all. The push eases off as what
    // it pushes nears walking pace, so the player never shoves a thing ahead of
    // itself faster than it walks.
    static constexpr int PUSH_STRENGTH = 1000;

    // A wing beat underwater: the impulse and the climb it is capped at, for a
    // body all the way under; a body partly under beats with the share of each
    // that is, so a bird breaking the surface gets its full flap back as its
    // wings come clear, and climbs out. Under the flap by enough that swimming
    // up a pool is visibly work -- about twenty cells a second against water's
    // drag and the sixth of gravity left -- and over what that sixth costs a
    // beat, so it is a climb at all.
    static constexpr fx::v SWIM_IMPULSE = fx::from_int(70);
    static constexpr fx::v SWIM_MAX_CLIMB = fx::from_int(45);

    // Being struck by a loose body: a slab landing on the player, a log rolling
    // into it, a raft rising under it. A body closing on it slower than
    // IMPACT_SAFE_SPEED does nothing -- a raft nudging the player, a plank
    // settling against it -- and over it the damage is the excess, in whole
    // cells a second, times the share of the two masses that is the body's
    // (a stone slab many times the player's weight hits with nearly all of it,
    // a plank its own weight with half), over IMPACT_DAMAGE_DIVISOR. A stone
    // slab dropped from a body height lands at about 180 cells a second and
    // takes a quarter of the bar; from two and a half, two fifths; one at a
    // body's terminal speed, near 450, most of it.
    static constexpr fx::v IMPACT_SAFE_SPEED = fx::from_int(60);
    static constexpr int IMPACT_DAMAGE_DIVISOR = 4;

    // Steps a knock (knock()) takes the walk key away for. Without it the walk
    // undoes the throw in a step: knocked away at 120 cells a second while
    // holding toward the troll at 112, the player stands where it was hit. A
    // quarter of a second -- the throw reads, and control is back before it lands.
    static constexpr int KNOCK_STAGGER_STEPS = 15;

    // The most pushes the player makes in a step: pushing back on a body that
    // shoved it, its weight on what it stands on, the kick of a jump off it, and
    // the push of walking into one.
    static constexpr int MAX_PUSHES = 4;

    Player(int start_x, int start_y);

    // Asks the grid whether the body, where it now stands, is on the ground, and
    // records the answer. For whoever places a body directly on terrain.
    //
    // A freshly constructed body believes it is in the air -- it has never asked --
    // so without this its first update() sees on_ground go from false to true and
    // counts a landing that never happened. That spends has_landed on step 1 and
    // takes away the free first landing a placed-on-the-ground spawn is meant to
    // keep for its first real fall (see game/boot.h).
    void settle(const Grid& grid) { body.on_ground = body.overlaps(grid, BOX, body.x, body.y + 1); }

    // Advances the player by one fixed step. Call this at the same fixed rate as
    // Grid::update(), and after it, so collision is tested against the world as it
    // now is rather than as it was.
    //
    // There is no `dt` parameter, and its absence is the point. A parameter nobody
    // varies is an invitation to vary it, and the first caller that passed a real
    // frame time would make the simulation a function of framerate again. The rate
    // is fx::STEPS_PER_SECOND, known at compile time, so every per-step amount below
    // is folded by the compiler rather than multiplied at runtime.
    void update(const Grid& grid, const PlayerInput& input);

    // Top-left corner of the body, in cells.
    int cell_x() const { return body.x; }
    int cell_y() const { return body.y; }

    // Centre of the body, in cells. Where tools originate from -- firing from the
    // top-left corner would let the player dig through a wall its own body is flush
    // against on the other side.
    int center_x() const { return body.x + WIDTH / 2; }
    int center_y() const { return body.y + HEIGHT / 2; }

    // The sub-cell remainder of the body's position, in fx cells. For the
    // renderer's interpolation -- nothing in src/physics/ may read these to make a
    // decision, because a fractional position is exactly the float-edge
    // representation the integer scheme below exists to keep out of collision.
    //
    // They exist because discarding the remainder at draw time is visible, not a
    // rounding detail: at any speed with a fraction in it, cell_x() advances on
    // some steps and stalls on others, so the motion stutters even though the
    // simulation is right.
    //
    // Fixed point, like everything else here. The conversion to float happens on
    // the render side (present::player_visual), the same arrangement as
    // Enemy::remainder_x, so src/physics/ holds no float at all.
    fx::v remainder_x() const { return body.rem_x; }
    fx::v remainder_y() const { return body.rem_y; }

    bool is_on_ground() const { return body.on_ground; }

    // Downward speed in fx cells per second -- compare it against the constants
    // above, or against fx::from_int(n), not against a plain number. fx::trunc()
    // gives whole cells per second for a readout.
    //
    // Fixed point rather than float because fall damage reads it for more than a
    // sign: a damage threshold is a rule, and a run that kills the player on one
    // machine and not another is a worse bug than a wrong threshold.
    fx::v velocity_y() const { return body.vel_y; }

    // True on the step a wing beat actually fired, not while the key is held. The
    // animation selector needs the event rather than the input: a flap animation
    // driven off the held key would play continuously between beats, and the whole
    // read of flight at this size is the rhythm of discrete downstrokes. Same shape
    // as DigTool reporting the step a dig connected.
    bool flapped() const { return did_flap; }

    // True on the step resolve_overlap relocated the body out of terrain, rather
    // than the body travelling there. The same one-step event shape as flapped().
    //
    // For the renderer's interpolation: a relocation must be drawn as a jump, not
    // eased across, and only the simulation knows which of the two a move was --
    // a fall at terminal velocity covers more cells per step than a short
    // relocation does, so distance cannot tell them apart (see pacer.h).
    bool teleported() const { return did_teleport; }

    // Read-only, and for the animation selector rather than for anything in here.
    // It distinguishes "the walk key is held" from "the body is actually
    // travelling" -- input held against a wall leaves the first true and the second
    // false, and a walk cycle that plays on the input walks on the spot against
    // every wall in the game. No new state: this is the same field move_x keeps.
    //
    // fx cells per second, like velocity_y(). Out of water it is exactly zero or
    // exactly +/-MOVE_SPEED (in water, slowed by BoxBody::wade), so a caller
    // asking whether the body is travelling compares against 0 rather than
    // against an epsilon. It is the legs' speed only: a knock, a shove and a
    // ride move the body without it, and a body sliding from a knock with no key
    // held is not walking.
    fx::v velocity_x() const { return body.vel_x; }

    // Health remaining, 0 to MAX_HEALTH. Clamped at zero rather than allowed to go
    // negative: "how dead" is not a quantity anything reads, and a negative bar is a
    // rendering bug waiting on a big enough fall.
    int health() const { return hp; }
    bool is_alive() const { return hp > 0; }

    // Damage taken on this step, zero on most of them. The same shape as flapped()
    // -- an event rather than a level, because what a hit indicator or a sound wants
    // is the moment, and deriving the moment from a falling health number at the
    // call site means every consumer keeps its own copy of last step's value.
    int damage_this_step() const { return hurt_this_step; }

    // Damage from something outside the body -- an enemy's swipe. The third source,
    // and the one that is not a read of the grid: the body cannot see an enemy any
    // more than the grid can, so whoever saw the hit says so. Goes through hurt(),
    // so it gets the clamp and the damage_this_step() event like the other two.
    //
    // Called after update() on the same step, which is what lets it land in this
    // step's damage_this_step() rather than being wiped by the next update().
    void take_hit(int amount) { hurt(amount); }

    // A blow's throw, in fx cells per second: `vx` onto the body's own velocity
    // that is not its legs' (BoxBody::push_x), which skids out on the ground and
    // flies on in the air, and `vy` onto its vertical speed -- negative throws it
    // up. Takes the walk key away for KNOCK_STAGGER_STEPS. Called after update()
    // like take_hit().
    //
    // Held, and given to the body by the next update() that moves it, after the
    // dig-out search. A troll's club breaks the ground the player stands on, and
    // the rubble sliding into the crater buries its feet the step after: given
    // at once, the throw was zeroed by the dig-out (which stops a relocated body
    // dead) and the club only ever dropped the player into its own crater.
    void knock(fx::v vx, fx::v vy) {
        knock_x += vx;
        knock_y += vy;
        stagger = KNOCK_STAGGER_STEPS;
    }

    // The body's velocity that is not its legs', fx cells per second: a knock
    // still skidding out, a shove, the speed kept from stepping off a raft. Zero
    // when nothing has moved it.
    fx::v push_velocity() const { return body.push_x; }

    // How the moving body under the feet moved this step, fx cells per second,
    // and whether there is one -- what the body is riding.
    bool is_riding() const { return body.riding; }
    fx::v carry_velocity_x() const { return body.carry_x; }

    // The share of the body under liquid this step, fx 0 to ONE.
    fx::v submerged() const { return immersed; }

    // What the body did to loose bodies this step, for Run to queue
    // (Grid::queue_impulse): this class holds the grid const, for the reason at
    // the top of the class, so it says what it pushed rather than pushing.
    int push_count() const { return push_n; }
    const BoxPush& push(int i) const { return pushes[static_cast<size_t>(i)]; }

    // A loose body that struck the body this step (hit is false on most steps):
    // where, how hard, and whether it pushed it clear or crushed it. Its damage
    // is already in damage_this_step().
    const BoxStrike& struck() const { return strike; }

    // True if a body placed with its top-left at (px, py) would overlap any solid
    // cell. Public because "the player is not inside a wall" is the single most
    // useful thing for a test to assert.
    bool overlaps_solid(const Grid& grid, int px, int py) const;

    // The hottest cell the body currently occupies. Public because it is the input
    // to the burn rule, and a test that could only see the output would have to work
    // backwards from a health number to say why it moved.
    uint8_t hottest_overlap(const Grid& grid) const;

private:
    // Position is an integer cell plus a sub-cell remainder rather than a plain
    // float. Collision then only ever compares whole cells, so a resting player sits
    // at an exact cell instead of a hair inside the floor, and there is no class of
    // float-edge bugs where a box is a fraction into a wall. The remainder carries
    // the fractional part of a move into the next step, which is what keeps motion
    // smooth at speeds below one cell per step.
    //
    // The remainder and the velocities are `fx` rather than float. The integer cell
    // was always the half that mattered for collision; making the other half integer
    // too is what turns "deterministic on this binary" into "deterministic
    // anywhere", which is what the replay check in tests/test_run.cpp proves.
    //
    // The box's movement against the grid is shared with every enemy -- see
    // box_body.h. What the player adds is in update(): flight, burns, fall damage
    // and the dig-out search.
    BoxBody body;
    static constexpr BoxRule BOX{WIDTH, HEIGHT, MAX_STEP_HEIGHT, HEIGHT};

    // Steps remaining before the next wing beat is allowed, and whether one fired
    // this step. Counted in fixed steps rather than seconds for the same reason
    // DigTool's cooldown is: the beat rate has to be identical on every machine.
    int flap_timer = 0;
    bool did_flap = false;
    bool did_teleport = false;

    // burn_timer is steps until the next burn tick, and is cleared rather than
    // decremented when the body is not in anything hot, so leaving a fire and
    // stepping straight back into it costs a tick immediately instead of resuming a
    // countdown the player cannot see. hurt_this_step is rebuilt every step, like
    // did_flap.
    //
    // has_landed is false until the body's feet have touched anything once, which is
    // what makes the spawn drop free -- see the fall-damage block in update(). It is
    // a fact about the run having started, not about the body.
    int hp = MAX_HEALTH;
    int burn_timer = 0;
    int hurt_this_step = 0;
    bool has_landed = false;

    // Steps the walk key is still taken away for, after a knock; and a knock
    // not yet given to the body (see knock()).
    int stagger = 0;
    fx::v knock_x = 0;
    fx::v knock_y = 0;
    // This step's share under liquid, the strike a body made, and the pushes the
    // body made, all rebuilt every step like did_flap.
    fx::v immersed = 0;
    BoxStrike strike{};
    std::array<BoxPush, MAX_PUSHES> pushes{};
    int push_n = 0;
    void add_push(int x, int y, int64_t jx, int64_t jy) {
        if (push_n < MAX_PUSHES) pushes[static_cast<size_t>(push_n++)] = BoxPush{x, y, jx, jy};
    }

    // The damage a strike does, by the rule at IMPACT_SAFE_SPEED.
    int impact_damage(const BoxStrike& s) const;

    // After the move: the body's weight on what it stands on (and the blow of
    // landing on it), the kick of a jump off one, and walking into one.
    void press_bodies(const Grid& grid, const PlayerInput& input, bool was_on_ground,
                      fx::v impact_speed);

    // Takes `amount` off the health, clamped at zero, and records it for
    // damage_this_step(). One writer for both sources, so a third one added later
    // cannot forget the clamp or the event.
    void hurt(int amount);

    // How many of the body's cells are inside solid material at a position.
    // overlaps_solid answers whether the body is stuck; this answers how badly,
    // which is what makes one escape comparable to another.
    int overlap_depth(const Grid& grid, int px, int py) const;

    // Whether the body can actually travel `dx, dy` to reach an open position,
    // rather than only fit once it is there. See resolve_overlap().
    bool escape_is_reachable(const Grid& grid, int dx, int dy) const;

    // Whether moving the box up one cell would push it into static matter that its
    // top row is not already inside -- what stops the deep-burial climb in
    // resolve_overlap from grinding up through a ceiling.
    bool would_enter_static(const Grid& grid) const;

    // Returns true if the body was overlapping terrain this step, in which case the
    // caller should skip normal physics.
    bool resolve_overlap(const Grid& grid);
};

// Three relationships the constants above have to hold, asserted rather than
// written down -- the same move element.h and reaction.h make about their data
// tables. TUNING.md invites these numbers to be changed by feel, and a prose
// sentence about how two of them relate goes stale the first time one is
// retuned alone.

// Headroom. rem_y holds at most a whole cell plus one step of the fastest motion
// in the game before the whole part is taken out of it, so the ceiling is
// nowhere near int32_t's -- but that is a claim about MAX_FALL_SPEED, and
// MAX_FALL_SPEED is a knob. A retune that overflowed this would show up as a
// body teleporting on the frame it hit terminal velocity.
static_assert(fx::ONE + fx::per_step(Player::MAX_FALL_SPEED) < INT32_MAX / 256,
              "MAX_FALL_SPEED has been raised far enough to threaten the fixed-point "
              "range; widen fx::v before raising it further");

// Flight climbs at all. A beat has to pay GRAVITY for the whole interval it
// covers, and what is left over is the climb, so raising FLAP_INTERVAL_STEPS or
// GRAVITY without touching FLAP_IMPULSE eventually makes the wingbeat a slower
// fall rather than a climb. That is a legitimate design choice; this refuses to
// let it happen silently.
static_assert(Player::FLAP_IMPULSE > fx::per_step(Player::GRAVITY) * Player::FLAP_INTERVAL_STEPS,
              "a wingbeat no longer outweighs the gravity it has to pay for, so holding "
              "the key would sink rather than climb");

// The standing jump stays the strongest single upward move. The whole reason
// the ground beat sets vel_y outright and the air beat is capped.
static_assert(Player::FLAP_MAX_CLIMB < Player::JUMP_SPEED,
              "sustained flight is now at least as fast as a standing jump, which makes "
              "the character a helicopter - see the flight comment above");

// --- and three more for the damage thresholds -------------------------------
//
// Same argument as the three above: each of these is only correct relative to a
// constant living somewhere else, two of them in a different file.

// Jumping never hurts. A standing jump comes back down at JUMP_SPEED plus the
// step of gravity applied on the way past the apex, so the safe-landing floor
// has to clear that sum and not merely JUMP_SPEED. Raising JUMP_SPEED without
// raising this would make the character damage itself by playing the game
// correctly, and it would present as "falling is broken" rather than as a jump
// constant.
static_assert(Player::SAFE_FALL_SPEED > Player::JUMP_SPEED + fx::per_step(Player::GRAVITY),
              "a standing jump now lands hard enough to take fall damage; raise "
              "SAFE_FALL_SPEED or lower JUMP_SPEED");

// The worst possible landing is survivable from full health, and it costs
// something. The upper bound is the design -- one free terminal-velocity
// mistake, the second one kills -- and the lower bound stops the rule quietly
// becoming decorative if MAX_FALL_SPEED is ever lowered below the safe
// threshold.
static_assert(Player::MAX_FALL_SPEED > Player::SAFE_FALL_SPEED,
              "terminal velocity is now below the safe landing speed, so no fall can "
              "ever do damage and the rule is dead code");
static_assert(fx::trunc(Player::MAX_FALL_SPEED - Player::SAFE_FALL_SPEED) /
                  Player::FALL_DAMAGE_DIVISOR <= Player::MAX_HEALTH,
              "a terminal-velocity landing now kills outright from full health; that is "
              "a design change, not a tuning one - see SAFE_FALL_SPEED");

// --- and for the body in water and among loose bodies ------------------------

// The player sinks. player_test stands a body on the floor of a pool a body
// deep, and the swim stroke is written for a body that has to climb; a density
// under water's would float it off the floor and turn the stroke into a
// rocket.
static_assert(Player::DENSITY > material_of(ElementType::Water).weight,
              "the player now floats in water; see DENSITY");

// A stroke underwater is a climb, not a slower sink: it has to outweigh the
// gravity water leaves over a beat's interval, the flight rule above in water.
static_assert(Player::SWIM_IMPULSE >
                  fx::per_step(Player::GRAVITY) * Player::FLAP_INTERVAL_STEPS *
                      (Player::DENSITY - material_of(ElementType::Water).weight) / Player::DENSITY,
              "a wing beat underwater no longer outweighs the sink it has to pay for");

// Swimming is slower than flying, which is the whole of what makes water a
// place rather than more air.
static_assert(Player::SWIM_IMPULSE < Player::FLAP_IMPULSE && Player::SWIM_MAX_CLIMB < Player::FLAP_MAX_CLIMB,
              "swimming is now at least as strong as flying");

// A falling slab hurts and settling against one does not: the threshold sits
// between a body's sleep drift and what a body gains falling one body height
// (v^2 = 2 g h, about 180 cells a second under the bodies' gravity).
static_assert(Player::IMPACT_SAFE_SPEED < fx::from_int(180),
              "a slab dropped from a body height no longer hurts the player it lands on");

// Steam does not burn and Fire does. The burn threshold sits in a gap between
// two numbers in MATERIALS, and nothing about editing that row suggests reading
// this one: raising Steam's spawn temperature would silently turn every doused
// flame into a hazard that damages the player through a cloud.
static_assert(Player::BURN_TEMPERATURE > material_of(ElementType::Steam).spawn_temperature,
              "steam now spawns hot enough to burn the player, so putting out a fire "
              "hurts you - see Steam's row in MATERIALS");
static_assert(Player::BURN_TEMPERATURE < material_of(ElementType::Fire).spawn_temperature &&
                  Player::BURN_TEMPERATURE < material_of(ElementType::Charred).heat_source,
              "fire or burning wood is no longer hot enough to hurt the player, which is "
              "the one thing S0's burn rule exists to do");
