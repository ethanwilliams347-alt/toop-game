#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include "fixed.h"
#include "fixed_trig.h"
#include "int_math.h"
#include "rig.h"

// Limbs that bend along their length: a chain of short rigid links, each turned
// about its own joint.
//
// rig.h turns a whole limb about one joint, which is right for a leg with a
// knee nobody can see and wrong for a tentacle, whose whole point is that it
// curls. A tentacle here is the same idea cut finer: its rest pixels are split
// between the links of a chain by where they sit along it, and each link is a
// rig::Xform -- a turn and a shift -- that carries its rest link onto where the
// chain has it now. So everything rig.h argues still holds per link. One body,
// one mask: a hole shot in the third link stays in the third link however the
// tentacle curls. Drawing and hitting still go backwards, cell to rest pixel,
// through one function, so what is drawn in a cell is what an arrow finds there.
//
// Where the chain is comes from two passes, after the tentacle-arm showcase this
// was modelled on (github.com/Smitner-Studio/tentacle-arm-showcase):
//
//  1. FABRIK (reach): the tip is put on its target and the chain is pulled after
//     it link by link, then the root is put back on its joint and the chain
//     pushed out from there. A few rounds of that and both ends are where they
//     should be with every link its own length. It starts from where the chain
//     was last step, which is what makes a tentacle trail and settle rather
//     than snap: the chain has a memory, and the solve only moves it as far as
//     the target moved.
//  2. A wave (wave): a sine travelling from root to tip, pushing each joint
//     sideways to its own link, which is the part that reads as alive. Applied
//     to a copy, not to the remembered chain, so it never accumulates, and
//     followed by one more round of reach so the root and tip stay put and only
//     the middle ripples.
//
// The showcase also runs a constraint pass to restore link lengths. FABRIK
// already sets every link to its length exactly, so that pass would only undo
// rounding, and is left out.
//
// Integer and fx throughout, for the reason rig.h gives: where a pixel is drawn
// is where it is hit, so a chain that differed by a bit between machines would
// fork a replay. Lengths come from int_math::isqrt, never a float sqrt.
//
// The cost of a link-per-Xform skin is the outside of a bend: two neighbouring
// links turned apart leave a notch between them that neither covers. pixel_of in
// enemy.cpp fills it from the neighbouring link (see link_of below), which puts
// the filled cell within about two cells of where its pixel really is, as long
// as no joint bends sharply (limit_bend) and the filler comes from near the
// joint, where the notch is. That is a cell looser than rig.h's forward map
// promises, and enemy_test holds a tentacled species to it.
namespace tentacle {

// Joints per chain, root and tip included: the most links the art generator
// gives a tentacle (the angler's lure, tools/angler_art.py), plus one.
inline constexpr int MAX_JOINTS = 13;
inline constexpr int MAX_LINKS = MAX_JOINTS - 1;
// Two legs and a lure. A species may have fewer.
inline constexpr int MAX_TENTACLES = 3;

// What drives the tip. A Leg's tip is a foot: planted on the ground while the
// other one steps, and swung to a new spot ahead in its turn. A Lure's tip
// floats on a spring toward wherever the body wants it -- hanging in front of
// the mouth, held out at the player, drawn back over the head for a strike.
enum class Role : uint8_t { Leg, Lure };

using Points = std::array<rig::Point, MAX_JOINTS>;
using Lengths = std::array<fx::v, MAX_LINKS>;

// --- second-order dynamics ----------------------------------------------------
//
// One way of making a thing follow a target that every follower here shares --
// the bulb, the body's lean and sink -- after t3ssel8r's "Giving Personality to
// Procedural Animations using Math". A follower is a damped spring with one
// extra term, and its three constants come from three numbers someone tuning
// it can think in:
//
//   period  steps for one swing at its natural speed: how quick it is;
//   zeta    damping: under 1 it overshoots and swings back, 1 just settles,
//           over 1 creeps in;
//   r       response: 0 starts gently, 1 moves with the target the moment it
//           moves, over 1 overshoots straight away, under 0 dips the wrong way
//           first -- anticipation, for free.
//
// The video's update with T fixed at one step, which turns its k1 = zeta/(pi f),
// k2 = 1/(2 pi f)^2 and k3 = r zeta/(2 pi f) into the period's 1/w = period/2pi
// times constants. k2 is held to at least what keeps the step stable at T = 1
// -- the video's own clamp -- so no tuning makes a follower blow up.
struct Response {
    fx::v k1, k2, k3;
};

constexpr Response response(int period, fx::v zeta, fx::v r) {
    // 1/w in fx: period / (2 pi), with 2 pi as four fx half-turns.
    const fx::v inv_w =
        static_cast<fx::v>(int64_t{period} * fx::ONE * fx::ONE / (4 * int64_t{fx::HALF_PI}));
    Response k{2 * fx::mul(zeta, inv_w), fx::mul(inv_w, inv_w), fx::mul(fx::mul(r, zeta), inv_w)};
    const fx::v stable = fx::ONE / 2 + k.k1 / 2;
    if (k.k2 < stable) k.k2 = stable;
    if (k.k2 < k.k1) k.k2 = k.k1;
    return k;
}

// One axis of a follower: where it is, how fast it is going, and where its
// target was last step (for the target's own speed, which r scales).
struct Follow {
    fx::v pos = 0;
    fx::v vel = 0;
    fx::v last = 0;
};

// One step toward `target`. Speed first and then position (semi-implicit), as
// the bulb's hand-written spring was, so a follower with r = 0 is that spring.
inline void follow(Follow& f, fx::v target, const Response& k) {
    const int64_t moved = target - f.last;
    f.last = target;
    const int64_t pull =
        int64_t{target} + fx::mul(k.k3, static_cast<fx::v>(moved)) - f.pos - fx::mul(k.k1, f.vel);
    f.vel += static_cast<fx::v>(pull * fx::ONE / k.k2);
    f.pos += f.vel;
}

// Put a follower at rest on `at`: nothing to catch up, no speed.
inline void settle(Follow& f, fx::v at) { f = {at, 0, at}; }

// One tentacle's rest geometry: which rig part its pixels are, and the chain
// they were drawn along, in frame cells on pixel centres. `radius` bounds how
// far any of its pixels is from the chain, so a posed link's pixels lie within
// that much of the link -- what the bounds round each link are built from.
struct Chain {
    rig::Part part;
    Role role;
    int joints;
    std::array<rig::Joint, MAX_JOINTS> rest;
    int radius;

    constexpr int links() const { return joints - 1; }
};

// A Chain from the generator's table of {x, y} pairs (angler_art.h).
template <int N>
constexpr Chain chain_of(rig::Part part, Role role, const int (&pts)[N][2], int radius) {
    static_assert(N >= 3 && N <= MAX_JOINTS,
                  "a chain needs at least two links, and fits MAX_JOINTS");
    Chain c{part, role, N, {}, radius};
    for (int i = 0; i < N; ++i) c.rest[i] = {pts[i][0], pts[i][1]};
    return c;
}

// How a species' tentacles move. Lengths in cells, times in fixed steps.
struct Motion {
    // --- the wave ----------------------------------------------------------
    // One wave every wave_steps, wave_amplitude cells either side at its
    // fullest -- midway along, tapering to nothing at the root and the tip --
    // and wave_spread radians of phase from one joint to the next, which is how
    // much of a wavelength one tentacle holds at once.
    int wave_steps;
    fx::v wave_amplitude;
    fx::v wave_spread;

    // Rounds of reach per step. Two or three gets the tip onto its target
    // within a sub-cell; more is the showcase's "diminishing returns".
    int iterations;
    // The share of the way each middle joint relaxes toward straight before
    // the solve (smooth), and the furthest any joint may bend (limit_bend).
    fx::v smoothing;
    fx::v max_bend;

    // --- the legs ----------------------------------------------------------
    //
    // A foot steps when it needs to, not on a clock: each leg has a home --
    // under its rest spot, led ahead by how fast the body is going -- and a
    // planted foot stays put until home has moved step_trigger cells away from
    // it, or settle_trigger once the body has stopped, so a body that halts
    // mid-stride tidies its feet under it. Only one foot is ever in the air, and
    // of two that want to step, the one further from home goes first; the
    // other waits, which is what makes the two fall into turns on their own.
    //
    // A swing takes swing_steps whatever the speed, so a running body takes
    // longer strides rather than quicker ones, and lands step_lead of a
    // swing's travel ahead of home's rest spot, so the foot is put down where
    // the body will be rather than where it was. Home's ground is searched
    // foot_probe columns either side, nearest first, and a solid foothold is
    // preferred to loose powder within that.
    int swing_steps;
    int step_trigger;
    int settle_trigger;
    fx::v step_lead;
    int foot_probe;
    // A foot rises this many cells at the middle of its swing, and further when
    // it is stepping up -- by half the climb.
    int step_lift;
    // Each step a leg's middle joints are pushed this far outward -- the rear
    // leg back, the front one forward -- before the chain is solved. Not
    // enough to move anything by itself: it decides which way a tentacle with
    // slack in it bows, so the two never fold through each other or flip from
    // one side to the other between steps.
    fx::v bow;

    // --- the body on its feet ----------------------------------------------
    //
    // The body is carried by its feet rather than by its box: it sinks and
    // rises with the average height of the feet against where they stand at
    // rest, up to body_sink cells either way, and tilts toward the lower foot
    // by the slope between them, up to body_tilt -- so it leans down a slope
    // and dips into a hollow one foot is standing in. Both follow on
    // body_follow, and each foot put down knocks the sink's speed by
    // footfall_dip cells a step, so a step lands as a small give in the body.
    Response body_follow;
    int body_sink;
    fx::v body_tilt;
    fx::v footfall_dip;

    // --- the lure ----------------------------------------------------------
    // How the bulb follows where it wants to be (see Response). Loose and
    // underdamped, so it sways past where it is going and swings back.
    Response lure_follow;
    // How far the bulb drifts with each breath, and how much further forward
    // it is held once it has seen you.
    int lure_sway;
    int lure_reach;
    // The wind-up: where the bulb is drawn back to, in rest frame cells -- up
    // over the head toward the tail -- and the point its strike arcs over on
    // the way down to the ground in front.
    rig::Joint lure_raise;
    rig::Joint lure_arc;

    // --- the strike ----------------------------------------------------------
    //
    // The bulb is what hits. It reaches the top of the wind-up lure_hold steps
    // early and holds there, trembling -- the last, plainest part of the
    // telegraph -- before it whips. The whip sends one bend down the stalk
    // from root to tip, lure_crack cells across at its fullest, as an octopus
    // arm reaches by rolling a bend along itself; on impact the stalk thrashes,
    // its wave lure_flourish cells fuller and dying away.
    //
    // It hurts only while it is moving to hurt: through the whip and for
    // lure_live steps after it lands, and only what its path touches -- the
    // bulb swept from where it was drawn last step to where it is drawn now,
    // bulb_radius cells round, so a bulb that moves further in a step than
    // anything is wide still finds what it passed through.
    int lure_hold;
    fx::v lure_crack;
    fx::v lure_flourish;
    int lure_live;
    int bulb_radius;

    // --- after a sever ------------------------------------------------------
    //
    // A tentacle whose tip is gone -- cut through, or the foot shot off -- no
    // longer stands, and the body walks on what it has left, deciding afresh
    // each step. On one leg it hops: stands hop_rest steps gathering itself,
    // then jumps at hop_speed cells a second and covers its ground in the air.
    // On none it drags itself along on the lure: the lure becomes a leg (and
    // so can no longer strike), the body slumps legless_sink cells down its box
    // onto the ground, and it moves at crawl_share of its speed.
    int hop_rest;
    fx::v hop_speed;
    int legless_sink;
    fx::v crawl_share;

    // A tentacle cut off does not turn straight to sand: the piece falls and
    // lies where it lands, a chain of its own, wriggling for piece_steps --
    // a wave piece_wriggle cells either side, one every piece_wriggle_steps,
    // dying away with it -- and then it crumbles.
    int piece_steps;
    fx::v piece_wriggle;
    int piece_wriggle_steps;
};

// A species' tentacles: up to three chains and how they move. The chains'
// parts are distinct -- a part is one tentacle or none.
struct Set {
    std::array<Chain, MAX_TENTACLES> chain;
    int count;
    Motion motion;

    // Which chain a part is, or -1 for a part turned whole by the rig.
    constexpr int of(rig::Part part) const {
        for (int i = 0; i < count; ++i)
            if (chain[i].part == part) return i;
        return -1;
    }
};

// The length of (dx, dy), both fx, as fx.
inline fx::v length(fx::v dx, fx::v dy) {
    return static_cast<fx::v>(int_math::isqrt(int64_t{dx} * dx + int64_t{dy} * dy));
}

// Rest link i's length in fx. From whole cells, so the square is exact and only
// the root rounds.
inline fx::v rest_length(const Chain& c, int i) {
    const int64_t dx = c.rest[i + 1].x - c.rest[i].x;
    const int64_t dy = c.rest[i + 1].y - c.rest[i].y;
    return static_cast<fx::v>(int_math::isqrt((dx * dx + dy * dy) * fx::ONE * fx::ONE));
}

inline Lengths rest_lengths(const Chain& c) {
    Lengths len{};
    for (int i = 0; i < c.links(); ++i) len[i] = rest_length(c, i);
    return len;
}

inline fx::v total_length(const Chain& c) {
    fx::v sum = 0;
    for (int i = 0; i < c.links(); ++i) sum += rest_length(c, i);
    return sum;
}

inline Points rest_points(const Chain& c) {
    Points p{};
    for (int i = 0; i < c.joints; ++i)
        p[i] = {fx::from_int(c.rest[i].x), fx::from_int(c.rest[i].y)};
    return p;
}

// `b` moved along the line from `a` so that it is exactly `len` from it. When
// the two coincide there is no line, and it goes straight down -- the way a
// tentacle with nothing pulling it would hang.
inline rig::Point place(rig::Point a, rig::Point b, fx::v len) {
    const fx::v dx = b.x - a.x, dy = b.y - a.y;
    const fx::v d = length(dx, dy);
    if (d == 0) return {a.x, a.y + len};
    return {a.x + static_cast<fx::v>(int64_t{dx} * len / d),
            a.y + static_cast<fx::v>(int64_t{dy} * len / d)};
}

// FABRIK: `rounds` passes of tip-to-root then root-to-tip, from wherever the
// chain is now. Out of reach, it ends pointing straight at the target from the
// root -- the tentacle at full stretch -- which is the right answer as well as
// what the passes converge to.
inline void reach(Points& p, const Lengths& len, int links, rig::Point root, rig::Point target,
                  int rounds) {
    for (int r = 0; r < rounds; ++r) {
        p[links] = target;
        for (int i = links - 1; i >= 0; --i) p[i] = place(p[i + 1], p[i], len[i]);
        p[0] = root;
        for (int i = 0; i < links; ++i) p[i + 1] = place(p[i], p[i + 1], len[i]);
    }
}

// sin(pi * i / links): 0 at the root and the tip, 1 midway. The envelope for
// anything done to the middle of a chain that must leave its ends alone.
inline fx::v midway(int i, int links) { return fx::sin(2 * fx::HALF_PI * i / links); }

// Each middle joint pushed by (dx, dy) times its envelope.
inline void nudge(Points& p, int links, fx::v dx, fx::v dy) {
    for (int i = 1; i < links; ++i) {
        const fx::v k = midway(i, links);
        p[i].x += fx::mul(dx, k);
        p[i].y += fx::mul(dy, k);
    }
}

// The travelling wave: joint i pushed sideways to the link that ends at it, by
// `amplitude` times sin(phase + i * spread) times its envelope. Positive is to
// the link's right as it runs root to tip (clockwise, with y down).
inline void wave(Points& p, int links, fx::v amplitude, fx::v phase, fx::v spread) {
    for (int i = 1; i < links; ++i) {
        const fx::v push =
            fx::mul(fx::mul(amplitude, fx::sin(phase + spread * i)), midway(i, links));
        const fx::v dx = p[i].x - p[i - 1].x, dy = p[i].y - p[i - 1].y;
        const fx::v d = length(dx, dy);
        if (d == 0) continue;
        p[i].x += static_cast<fx::v>(int64_t{-dy} * push / d);
        p[i].y += static_cast<fx::v>(int64_t{dx} * push / d);
    }
}

// One bend travelling down the chain: the joints within two of `front` -- 0
// at the root, ONE at the tip -- pushed sideways to their link, `amplitude`
// cells at `front` and tapering to nothing two joints either side. Stepped
// from 0 to ONE over a strike, it rolls a crook from the root out to the tip.
// Positive is to the link's right, as for wave.
inline void pulse(Points& p, int links, fx::v amplitude, fx::v front) {
    const fx::v at = front * links;
    const fx::v width = fx::from_int(2);
    for (int i = 1; i < links; ++i) {
        const fx::v d = std::abs(fx::from_int(i) - at);
        if (d >= width) continue;
        const fx::v push = static_cast<fx::v>(int64_t{amplitude} * (width - d) / width);
        const fx::v dx = p[i].x - p[i - 1].x, dy = p[i].y - p[i - 1].y;
        const fx::v len = length(dx, dy);
        if (len == 0) continue;
        p[i].x += static_cast<fx::v>(int64_t{-dy} * push / len);
        p[i].y += static_cast<fx::v>(int64_t{dx} * push / len);
    }
}

// Whether a ball of radius `r` moved from `a` to `b` touches the box of whole
// cells [x0, x1) x [y0, y1), all in fx. The path is walked in steps of at most
// half a cell and each stop tested exactly -- the nearest point of the box
// within r -- so a ball that crossed the box between one step's end and the
// next still touches it. Half a cell, against a box at least a cell wide and
// a radius of a cell or more, cannot step over anything.
inline bool sweep_touches(rig::Point a, rig::Point b, fx::v r, int x0, int y0, int x1, int y1) {
    // A cell's area runs half a cell either side of its centre.
    const fx::v left = fx::from_int(x0) - fx::ONE / 2, right = fx::from_int(x1) - fx::ONE / 2;
    const fx::v top = fx::from_int(y0) - fx::ONE / 2, bottom = fx::from_int(y1) - fx::ONE / 2;
    const fx::v dist = length(b.x - a.x, b.y - a.y);
    const int stops = 1 + static_cast<int>(dist / (fx::ONE / 2));
    for (int i = 0; i <= stops; ++i) {
        const fx::v x = a.x + static_cast<fx::v>(int64_t{b.x - a.x} * i / stops);
        const fx::v y = a.y + static_cast<fx::v>(int64_t{b.y - a.y} * i / stops);
        const fx::v nx = std::clamp(x, left, right), ny = std::clamp(y, top, bottom);
        const int64_t ex = x - nx, ey = y - ny;
        if (ex * ex + ey * ey <= int64_t{r} * r) return true;
    }
    return false;
}

// Each middle joint moved by the share `k` of the way toward the midpoint of
// its neighbours. A chain whose target moves back toward its root has slack to
// take up, and the solve takes it up wherever the last pose leaves room -- often
// as a zigzag, which a link-per-turn skin draws as branches and stray pixels.
// Relaxing toward straight before every solve irons a fold out over a few steps
// instead of letting the next solve fold it tighter. Leaves the ends alone.
inline void smooth(Points& p, int links, fx::v k) {
    const Points q = p;
    for (int i = 1; i < links; ++i) {
        p[i].x += fx::mul((q[i - 1].x + q[i + 1].x) / 2 - q[i].x, k);
        p[i].y += fx::mul((q[i - 1].y + q[i + 1].y) / 2 - q[i].y, k);
    }
}

// q turned about o by (c, s).
inline rig::Point turn_about(rig::Point o, rig::Point q, fx::v c, fx::v s) {
    const fx::v dx = q.x - o.x, dy = q.y - o.y;
    return {o.x + fx::mul(dx, c) - fx::mul(dy, s), o.y + fx::mul(dx, s) + fx::mul(dy, c)};
}

// No joint bent further than the angle whose cosine and sine these are: root
// to tip, a joint bent too far has the rest of the chain turned back about it
// until it is bent exactly that far. Smoothing makes a sharp fold rare; this
// makes it impossible, which is what bounds the notch at the outside of a bend
// that tentacle_pixel in enemy.cpp fills -- the further a joint bends, the
// further the filled cell is from where its pixel really is. The tip may end a
// little off its target, which a tentacle at its limit would.
inline void limit_bend(Points& p, int links, fx::v max_cos, fx::v max_sin) {
    for (int i = 1; i < links; ++i) {
        const int64_t ax = p[i].x - p[i - 1].x, ay = p[i].y - p[i - 1].y;
        const int64_t bx = p[i + 1].x - p[i].x, by = p[i + 1].y - p[i].y;
        const int64_t la = length(static_cast<fx::v>(ax), static_cast<fx::v>(ay));
        const int64_t lb = length(static_cast<fx::v>(bx), static_cast<fx::v>(by));
        if (la == 0 || lb == 0) continue;
        // cos(bend) = dot / (la * lb), compared without dividing.
        if ((ax * bx + ay * by) * fx::ONE >= max_cos * la * lb) continue;
        // The direction the next link may have: this one's, turned by the
        // limit toward the side it was bending, as a unit vector in fx.
        const int64_t s = (ax * by - ay * bx) >= 0 ? max_sin : -max_sin;
        const int64_t wx = (ax * max_cos - ay * s) / la;
        const int64_t wy = (ax * s + ay * max_cos) / la;
        // The turn that takes the next link there, applied to all of the rest.
        const fx::v c2 = static_cast<fx::v>((bx * wx + by * wy) / lb);
        const fx::v s2 = static_cast<fx::v>((bx * wy - by * wx) / lb);
        for (int j = i + 1; j <= links; ++j) p[j] = turn_about(p[i], p[j], c2, s2);
    }
}

// The turn and shift that carry rest link i onto the posed link from `a` to
// `b`: turned by the angle between the two directions, about the rest root
// joint, then moved so that joint lands on `a`.
//
// The cosine and sine are the two directions' dot and cross products over the
// product of their lengths, and that product is one isqrt of the product of
// the squares rather than two isqrts multiplied. In the rest pose that makes
// the turn exact: the posed direction is the rest one times ONE, the product of
// squares is a perfect square, and the cosine comes out ONE to the bit -- so a
// body at rest maps every tentacle pixel to itself, as rig.h promises for every
// other part.
inline rig::Xform link_turn(const Chain& c, int i, rig::Point a, rig::Point b) {
    const int64_t ux = c.rest[i + 1].x - c.rest[i].x, uy = c.rest[i + 1].y - c.rest[i].y;
    const int64_t wx = b.x - a.x, wy = b.y - a.y;
    const int64_t norm = int_math::isqrt((ux * ux + uy * uy) * (wx * wx + wy * wy));
    rig::Xform t;
    if (norm != 0) {
        t.c = static_cast<fx::v>((ux * wx + uy * wy) * fx::ONE / norm);
        t.s = static_cast<fx::v>((ux * wy - uy * wx) * fx::ONE / norm);
    }
    t.tx = a.x - fx::from_int(c.rest[i].x);
    t.ty = a.y - fx::from_int(c.rest[i].y);
    return t;
}

// Which rest link a rest pixel belongs to: the one it is nearest, measured to
// the link's segment, the lower link on a tie. The same measure the generator
// takes the chain's radius with, so every pixel is within `radius` of its own
// link.
//
// Nearest, not cut by lines through the joints: the first version split the
// chain by the bisector at each joint, which is right for a gently curving
// tentacle and wrong for the lure, which hooks back under itself -- the bulb
// at its tip lies on the root side of the first joint's bisector, and every
// pixel of the last link was given to the first.
//
// Exact integer arithmetic, no division: a squared distance to a segment is a
// fraction (the perpendicular part is cross^2 / |u|^2), so each is kept as a
// numerator over |u|^2 and two are compared by cross-multiplying. A pixel's
// link decides which turn places it, which is where it is hit.
constexpr int link_of(const Chain& c, int x, int y) {
    int best = 0;
    int64_t best_num = -1, best_den = 1;
    for (int i = 0; i < c.links(); ++i) {
        const int64_t ux = c.rest[i + 1].x - c.rest[i].x, uy = c.rest[i + 1].y - c.rest[i].y;
        const int64_t px = x - c.rest[i].x, py = y - c.rest[i].y;
        const int64_t den = ux * ux + uy * uy;
        const int64_t along = px * ux + py * uy;
        int64_t num;
        if (along <= 0) {
            num = (px * px + py * py) * den;
        } else if (along >= den) {
            const int64_t qx = px - ux, qy = py - uy;
            num = (qx * qx + qy * qy) * den;
        } else {
            const int64_t cross = px * uy - py * ux;
            num = cross * cross;
        }
        if (best_num < 0 || num * best_den < best_num * den) {
            best = i;
            best_num = num;
            best_den = den;
        }
    }
    return best;
}

}  // namespace tentacle
