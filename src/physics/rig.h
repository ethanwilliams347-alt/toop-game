#pragma once
#include <array>
#include <cstdint>
#include "body_art.h"
#include "fixed.h"
#include "fixed_trig.h"

// The skeleton an enemy's pixels hang on, and the arithmetic that poses them.
//
// The body's art is one picture -- the rest pose -- and the damage mask lives in
// that picture's coordinates, so a pixel shot off is shot off for good whatever
// the body is doing. What moves is where each pixel is drawn and hit. A body is
// cut into six rigid parts by where a pixel sits in the art (the same geometry
// body_art already uses for "arm" and "foot"), each part turns about its own
// joint, and the arms and head ride on the torso. A pose is six angles and a
// nudge; a pixel's place in the world is its rest position put through its
// part's turn.
//
// Why bones and not a second drawn frame: a drawn walk cycle is a second set of
// pixels, and the mask would mark pixels the new frame draws somewhere else -- a
// leg shot off on frame one would grow back on frame two. Turning the rest pixels
// keeps one body with one mask, so a hole in the shin stays in the shin as the
// leg swings, and whatever an arrow takes out is gone from every pose at once.
//
// Why turning and not sliding: a limb slid sideways reads as a limb sliding.
// A swing is a turn about the shoulder or the hip, and a club raised over the
// head is a turn of 150 degrees, which no sliding can fake.
//
// The two directions are not used alike, and that is the part that has to be
// right. Drawing and hitting go BACKWARDS: for a world cell, which rest pixel
// lands there? Turned backwards, every cell asks exactly one question with one
// answer, so a turned arm has no holes in it and an arrow and the eye always
// agree on what is in a cell -- the renderer and Enemy::pixel_at both call the
// same function. Going FORWARDS -- where does this rest pixel land? -- is only for
// placing the grain a lost pixel becomes and for measuring a bite, where being
// within a cell of the drawn spot is exact enough; see posed_matches_forward in
// tests/test_enemy.cpp for the bound.
//
// Integer and fx throughout, with fx::sincos, because where a pixel is drawn is
// where it is hit, which is simulation state: a pose that differed by one bit
// between machines would put an arrow through a different arm and fork a replay.
namespace rig {

// Back to front. pixel_at searches front to back and the renderer paints back to
// front, and both read this order, so the arm in front of the body is the one
// drawn over it and the one an arrow meets first. "Front" is the side the art
// faces (+x in frame space): the leading hand, which for the troll holds the club.
enum Part : uint8_t { RearArm, RearLeg, FrontLeg, Body, Head, FrontArm, PART_COUNT };

struct Joint {
    int x;
    int y;
};

// An angle in degrees, as fx radians. Every angle in a Rig is written this way so
// the table reads as a pose, and the conversion is exact integer arithmetic.
constexpr fx::v deg(int d) { return d * fx::HALF_PI / 90; }

// Where a species' parts meet and how far they move. Lengths are frame cells,
// times are fixed steps, angles are fx radians (written with deg()).
//
// Angles turn CLOCKWISE on screen, drawn facing right, which is the one sense in
// which every positive number below reads naturally: a body leaning forward, a
// club swung back over the head, a foot kicked back. So an arm or a leg swung
// FORWARD -- hand or foot toward the way it faces -- is negative.
struct Rig {
    // Legs: pixels inside the box's columns from this row down, split into the
    // rear leg (x < leg_split) and the front one. The rows above it are the body.
    int hip_row;
    int leg_split;
    // Head: the rows above this one. 0 for a species with no separate head.
    int neck_row;

    // The body turns about the waist; everything else about its own joint. The
    // arms and the head ride on the body, so their joints are where they sit in
    // the rest pose and they go where the body takes them.
    Joint waist;
    Joint neck;
    Joint rear_shoulder;
    Joint front_shoulder;
    Joint rear_hip;
    Joint front_hip;

    // --- the walk ----------------------------------------------------------
    // Cells walked per full cycle of both legs. The gait advances by distance
    // actually covered, not by time, so the feet never skate: a body pushed
    // against a wall stands still, and one chasing walks faster by the same
    // stride rather than by a separate faster cycle.
    int stride;
    fx::v leg_swing;   // each leg's angle at the far end of a stride
    fx::v arm_swing;   // each arm's, swung against its own side's leg
    int bob;           // cells the body sinks at the far end of a stride
    fx::v chase_lean;  // the body leans into a chase
    // While chasing, both arms are held at this angle instead of swinging --
    // the ghoul reaches for you. 0 leaves the arms swinging.
    fx::v chase_arms;

    // --- standing ----------------------------------------------------------
    // One breath every breathe_steps, which heaves the body and sways the arms
    // by `breathe`. Always running, so nothing is ever perfectly still.
    int breathe_steps;
    fx::v breathe;

    // --- the attack --------------------------------------------------------
    // The front arm goes from `raise` to `strike` over strike_steps. For a slam
    // the raise happens over the wind-up and the strike is the last strike_steps
    // of it, so the club arrives as the blow lands; for a swipe -- which lands
    // the step the boxes touch, with no wind-up -- the stroke is the
    // follow-through that plays as it lands. The stroke always turns clockwise
    // -- over the top and down the front -- going a whole turn round if it
    // has to, so `strike` can be written as the angle the arm ends at.
    fx::v raise;
    fx::v strike;
    int strike_steps;
    fx::v windup_lean;  // leaning into the wind-up
    fx::v strike_lean;  // and over the blow

    // --- being hit ---------------------------------------------------------
    // Rocks back by `flinch` when an arrow takes pixels, easing back over
    // flinch_steps.
    int flinch_steps;
    fx::v flinch;

    // How far a pose can take a pixel outside the rest frame, any side, in cells.
    // The renderer's atlas slot is the frame plus this much all round; a pose
    // reaching past it is clipped on screen (never in the simulation).
    // enemy_test walks, chases, jumps, attacks and flinches each species and
    // checks every pixel stays inside it.
    int pad;
};

// Which part a rest pixel belongs to. Geometry, as body_art's is_arm/is_foot are:
// a pixel is in the arm because of where it is, and a letter could disagree.
//
// A lettered arm (body_art::Art::arm) is always the front arm, wherever its
// pixels sit: the one body that letters its arm has one, it reaches the way the
// body faces, and it is drawn over everything else.
constexpr Part part_of(const body_art::Art& a, const Rig& r, int x, int y) {
    if (a.is_arm(x, y)) return a.arm == nullptr && x < a.box_left ? RearArm : FrontArm;
    if (y >= r.hip_row && x >= a.box_left && x < a.box_right())
        return x < r.leg_split ? RearLeg : FrontLeg;
    if (y < r.neck_row) return Head;
    return Body;
}

// The rest-frame rectangle each part can occupy, [x0, x1) x [y0, y1). A bound,
// not an outline: part_of decides membership, this only says where not to look.
struct Box {
    int x0, y0, x1, y1;
};
constexpr Box rest_box(const body_art::Art& a, const Rig& r, Part p) {
    if (a.arm != nullptr && (p == RearArm || p == FrontArm)) {
        if (p == RearArm) return {0, 0, 0, 0};
        return {a.arm_box.x0, a.arm_box.y0, a.arm_box.x1, a.arm_box.y1};
    }
    switch (p) {
        case RearArm:  return {0, a.arm_top, a.box_left, a.h};
        case FrontArm: return {a.box_right(), a.arm_top, a.w, a.h};
        case RearLeg:  return {a.box_left, r.hip_row, r.leg_split, a.h};
        case FrontLeg: return {r.leg_split, r.hip_row, a.box_right(), a.h};
        case Head:     return {0, 0, a.w, r.neck_row};
        default:       return {0, 0, a.w, a.h};
    }
}

constexpr Joint pivot_of(const Rig& r, Part p) {
    switch (p) {
        case RearArm:  return r.rear_shoulder;
        case FrontArm: return r.front_shoulder;
        case RearLeg:  return r.rear_hip;
        case FrontLeg: return r.front_hip;
        case Head:     return r.neck;
        default:       return r.waist;
    }
}

// Arms and head ride on the body; the legs and the body do not ride on anything.
constexpr bool rides_body(Part p) { return p == RearArm || p == FrontArm || p == Head; }

// One part's turn about its joint, then a shift: sin and cos of the angle,
// precomputed once per step, and the shift in fx cells.
struct Xform {
    fx::v c = fx::ONE;
    fx::v s = 0;
    fx::v tx = 0;
    fx::v ty = 0;
};

inline Xform turn(fx::v angle, fx::v tx = 0, fx::v ty = 0) {
    const fx::SinCos sc = fx::sincos(angle);
    return Xform{sc.c, sc.s, tx, ty};
}

// A point in fx frame cells. Pixel (x, y)'s centre is (x, y) exactly: joints sit
// on pixel centres, so the rest pose maps every pixel to itself with no rounding.
struct Point {
    fx::v x;
    fx::v y;
};

// Forwards: rest point to posed, about joint j. Clockwise on screen with y down
// is the textbook rotation matrix.
inline Point apply(const Xform& t, Joint j, Point p) {
    const fx::v dx = p.x - fx::from_int(j.x);
    const fx::v dy = p.y - fx::from_int(j.y);
    return {fx::from_int(j.x) + fx::mul(dx, t.c) - fx::mul(dy, t.s) + t.tx,
            fx::from_int(j.y) + fx::mul(dx, t.s) + fx::mul(dy, t.c) + t.ty};
}

// Backwards: posed point to rest -- the same turn by minus the angle.
inline Point unapply(const Xform& t, Joint j, Point p) {
    const fx::v dx = p.x - t.tx - fx::from_int(j.x);
    const fx::v dy = p.y - t.ty - fx::from_int(j.y);
    return {fx::from_int(j.x) + fx::mul(dx, t.c) + fx::mul(dy, t.s),
            fx::from_int(j.y) - fx::mul(dx, t.s) + fx::mul(dy, t.c)};
}

// Nearest whole cell, halves rounding up. Floor of (v + 1/2) by division, not
// >>, for the reason fixed.h gives; and floor rather than fx::trunc because a
// pixel a hair either side of zero must round the same way it does anywhere
// else, or the column through a joint doubles up.
constexpr int nearest(fx::v v) {
    const fx::v h = v + fx::ONE / 2;
    return h >= 0 ? h / fx::ONE : -((-h + fx::ONE - 1) / fx::ONE);
}
static_assert(nearest(fx::ONE / 2) == 1 && nearest(-fx::ONE / 2) == 0 &&
                  nearest(-fx::ONE / 2 - 1) == -1 && nearest(fx::from_int(-3)) == -3,
              "rig::nearest must round to the nearest cell, halves up, on both sides "
              "of zero alike");

// A cell-to-cell map flattened to x' = x*xx + y*xy + x0 (and likewise y'), with
// the six numbers in fx and the inputs whole cells. Flattened once per step so
// that mapping a pixel is four integer multiplies: the turn itself goes through
// fx::mul, whose 64-bit divide is the expensive part, and a body asks where its
// pixels are thousands of times a step (every pixel, every step, for heat).
struct Affine {
    fx::v xx = fx::ONE, xy = 0, x0 = 0;
    fx::v yx = 0, yy = fx::ONE, y0 = 0;

    Point at(int x, int y) const { return {x * xx + y * xy + x0, x * yx + y * yy + y0}; }

    // The map that sends (0,0), (1,0) and (0,1) where `f` sends them. Exact for
    // an affine f, up to the last bit of each fx::mul inside it.
    template <class F> static Affine through(F f) {
        const Point o = f(0, 0), ex = f(1, 0), ey = f(0, 1);
        return {ex.x - o.x, ey.x - o.x, o.x, ex.y - o.y, ey.y - o.y, o.y};
    }
};

// A whole pose: one Xform per part. The arms' and head's are relative to the
// body, so they go through their own and then the body's. flatten() turns the
// result into one Affine per part each way, which is what everything reads.
struct Pose {
    std::array<Xform, PART_COUNT> part{};
    std::array<Affine, PART_COUNT> fwd{};
    std::array<Affine, PART_COUNT> bwd{};

    void flatten(const Rig& r) {
        for (int i = 0; i < PART_COUNT; ++i) {
            const Part p = static_cast<Part>(i);
            const Xform& own = part[p];
            const Xform& body = part[Body];
            const Joint j = pivot_of(r, p);
            const bool rides = rides_body(p);
            fwd[i] = Affine::through([&](int x, int y) {
                Point q = apply(own, j, {fx::from_int(x), fx::from_int(y)});
                return rides ? apply(body, r.waist, q) : q;
            });
            bwd[i] = Affine::through([&](int x, int y) {
                Point q{fx::from_int(x), fx::from_int(y)};
                if (rides) q = unapply(body, r.waist, q);
                return unapply(own, j, q);
            });
        }
    }

    Point forward(Part p, int x, int y) const { return fwd[p].at(x, y); }
    Point backward(Part p, int x, int y) const { return bwd[p].at(x, y); }
};

}  // namespace rig
