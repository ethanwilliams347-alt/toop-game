#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

// What happened during one Run::step, for anything outside the simulation that
// wants to react to it: the HUD's kill count, a screen shake when a troll's club
// lands, a flash when the player is hurt, a sound.
//
// Before this, the run told the outside world what happened by side channels:
// kills were counted by snapshotting every enemy's `alive` before and after the
// step, a swipe landing was the bool Enemy::update returned, a dig was the bool
// Run::step returned. Every new reaction would have needed another snapshot or
// another return value threaded through Run. Now the step writes down what
// happened, once, and readers read.
//
// Output only. Nothing in the simulation reads an event, so events cannot change
// what a step does and cannot fork a replay -- a renderer that shakes the camera
// on a Slam is still a renderer, never a simulation input.
//
// A fixed array, cleared (by count, not by touching the storage) at the start of
// every step, because Run::step is in the step loop and allocates nothing. If a
// step ever produces more than CAPACITY events the rest are counted in
// `dropped` rather than written; the capacity is set from the pools that produce
// them so that cannot happen in play, and run_test checks it does not.
struct Event {
    enum class Kind : uint8_t {
        ArrowLoosed,     // x, y: where it left the bow
        ArrowHit,        // x, y: the cell it hit; a: enemy slot; b: pixels it took
        ArrowStuck,      // x, y: the cell it stuck in
        EnemyKilled,     // x, y: the body's centre; a: enemy slot; b: species index
        SlamWindup,      // x, y: where the club will land; a: enemy slot
        Slam,            // x, y: where the club landed; a: enemy slot
        PlayerHurt,      // x, y: the body's centre; a: damage; b: species index, or -1
                         //       for anything that is not an enemy (fire, a fall)
        PlayerDied,      // x, y: the body's centre
        Dug,             // x, y: the cursor the dig was aimed at
        ObjectiveReached,// x, y: the objective
    };
    Kind kind;
    int x = 0;
    int y = 0;
    int a = 0;
    int b = 0;
};

class EventLog {
public:
    // Every arrow can hit or stick once a step and the bow can loose one; every
    // enemy can die, wind up and slam; the player gets a handful. Rounded up well
    // past the sum of the pools (48 arrows, 24 enemies), so `dropped` stays 0.
    static constexpr int CAPACITY = 256;

    void clear() {
        count_ = 0;
        dropped_ = 0;
    }
    void push(const Event& e) {
        if (count_ < CAPACITY)
            events_[static_cast<size_t>(count_++)] = e;
        else
            ++dropped_;
    }

    int size() const { return count_; }
    int dropped() const { return dropped_; }
    const Event& operator[](int i) const { return events_[static_cast<size_t>(i)]; }
    const Event* begin() const { return events_.data(); }
    const Event* end() const { return events_.data() + count_; }

    int count(Event::Kind kind) const {
        int n = 0;
        for (const Event& e : *this)
            if (e.kind == kind) ++n;
        return n;
    }

private:
    std::array<Event, CAPACITY> events_{};
    int count_ = 0;
    int dropped_ = 0;
};
