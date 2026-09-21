#include "render/player_anim.h"

namespace player_anim {

using player_sprite::Anim;

const Anim& select(const Conditions& c) {
    if (!c.on_ground) {
        // Two poses on one sheet row, chosen by the sign of the fall rather than
        // advanced by a clock. A long drop holds the falling pose instead of
        // cycling through a rise it is plainly not doing, which is what a
        // two-frame looping "air" animation would do.
        //
        // Zero counts as rising, so the apex of a jump does not flicker between
        // the two poses on the one step the velocity passes through it.
        return c.vel_y <= 0 ? player_sprite::RISE : player_sprite::FALL;
    }
    return c.moving ? player_sprite::WALK : player_sprite::IDLE;
}

namespace {

void begin(State& s, const Anim& a, bool oneshot) {
    // Re-selecting the animation already playing must not restart it, or a walk
    // cycle resets to frame zero every step and the figure stands still with one leg
    // twitching. Identity is the pointer, which works because every Anim is a
    // distinct constexpr object in the generated header.
    if (s.anim == &a && s.oneshot == oneshot) return;
    s.anim = &a;
    s.frame = 0;
    s.elapsed = 0;
    s.oneshot = oneshot;
}

}  // namespace

void update(State& s, const Conditions& c, int steps) {
    if (c.dig_progress >= 0.0f) {
        // The dig is the one animation with no clock of its own. Its frame is a
        // function of how far through its swing the tool is, so the two cannot
        // disagree.
        //
        // Not a one-shot either. A one-shot is a thing that has to end, and
        // ending is the tool's business: the swing stops when dig_progress goes
        // negative, and while the button is held the tool starts another one.
        //
        // DIG.wait is dead data. The swing's speed is DigTool::SWING_STEPS; the
        // sheet's wait column is read for every other animation and ignored for
        // this one, so editing it here changes nothing. Only `frames` is still
        // load-bearing, as the number of columns to spread the swing across.
        const int frames = player_sprite::DIG.frames;
        int frame = static_cast<int>(c.dig_progress * static_cast<float>(frames));

        // dig_progress is documented as never reaching 1, but a clamp is cheap
        // and the alternative is reading one column off the end of the sheet if
        // that ever stops being true.
        if (frame >= frames) frame = frames - 1;

        s.anim = &player_sprite::DIG;
        s.frame = frame;
        s.elapsed = 0;
        s.oneshot = false;
        return;
    }

    if (c.flapped && !c.on_ground) {
        // Same latch-and-restart as the dig, and for the same reason: each beat
        // is its own downstroke, not a continuation of the last. Because FLY's
        // own length exceeds the interval between beats, sustained flight is this
        // branch firing over and over rather than an animation left to loop,
        // which is what keeps the wings in phase with the impulses the physics is
        // applying.
        //
        // !on_ground is doing real work. The launch off the ground is a beat too
        // and sets `flapped`, but on the step it fires the feet are still down;
        // without the guard, tapping the key while standing plays a wing beat in
        // place, which reads as the bird flapping at the floor.
        s.anim = &player_sprite::FLY;
        s.frame = 0;
        s.elapsed = 0;
        s.oneshot = true;
    } else if (s.oneshot && s.anim == &player_sprite::FLY && c.on_ground) {
        // Landing cancels a beat still in flight. A one-shot normally owns the
        // clock until it finishes, which is wrong for a wing beat, whose entire
        // premise is that the body is in the air.
        s.oneshot = false;
        begin(s, select(c), false);
    } else if (!s.oneshot) {
        begin(s, select(c), false);
    }

    for (int i = 0; i < steps; ++i) {
        // A wait of zero means this animation does not advance on a clock -- the
        // single-frame poses. Guarding it here rather than forbidding it in the
        // table keeps a zero from becoming an infinite loop below.
        if (s.anim->wait <= 0 || s.anim->frames <= 1) {
            if (!s.oneshot) continue;
            // A one-shot that cannot advance would never finish. Treat it as
            // complete rather than hanging on frame zero forever.
            s.oneshot = false;
            begin(s, select(c), false);
            continue;
        }

        if (++s.elapsed < s.anim->wait) continue;
        s.elapsed = 0;

        if (++s.frame < s.anim->frames) continue;

        if (s.anim->loop) {
            s.frame = 0;
        } else {
            // Done. Hand control back to the conditions, which is what makes this a
            // one-shot rather than an animation that has to be cleared by whoever
            // set it.
            s.oneshot = false;
            s.frame = 0;
            begin(s, select(c), false);
        }
    }
}

}  // namespace player_anim
