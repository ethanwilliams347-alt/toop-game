// The step loop allocates nothing in steady state.
//
// ARCHITECTURE.md states it as an invariant, and until this suite it was held by
// reading: a local std::vector, std::string or std::function anywhere under
// Run::step compiles, passes every other test and costs a heap round trip per
// step. This counts every global allocation the process makes and asserts that
// a run replaying steps it has already been through makes none.
//
// "Already been through" is the steady state the invariant promises. Persistent
// scratch vectors grow to the largest size a world has needed and then stay
// there (grid.cpp's constructor reserves every one with a hard bound, and lists
// the few without one), so the first pass through a scenario may allocate as
// those high-water marks are reached. The second pass, on the same seed and the
// same inputs, needs nothing they do not already hold -- any allocation in it is
// one that happens every step.
//
// Global operator new is replaced for this executable only. Every allocation in
// the process goes through it, so the count covers the simulation and anything it
// reaches, not only the containers this file can see.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include "game/run.h"
#include "test_util.h"

namespace {
std::atomic<bool> counting{false};
std::atomic<long> allocations{0};

void* counted_alloc(std::size_t n) {
    if (counting.load(std::memory_order_relaxed))
        allocations.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) n = 1;
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
}  // namespace

// The replaceable global allocation functions. The aligned overloads are left
// to the runtime: nothing in the engine over-aligns a heap allocation, and their
// replacement differs between platforms.
void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    if (counting.load(std::memory_order_relaxed))
        allocations.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n ? n : 1);
}
void* operator new[](std::size_t n, const std::nothrow_t& t) noexcept { return operator new(n, t); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }

namespace {

constexpr int W = 256;
constexpr int H = 160;
constexpr uint64_t SEED = 0xA110CA7Eull;
constexpr int STEPS = 400;

// A world that exercises every system a step runs: a slab knocked loose so it
// falls and fractures, a wooden beam set alight, a pool, and enemies to shoot at
// and to be attacked by. Rebuilt from scratch each pass, so both passes start
// from the same world.
void build(Run& run) {
    Grid& g = run.grid;
    for (int x = 0; x < W; ++x)
        for (int y = H - 12; y < H; ++y) g.set_element(x, y, ElementType::Wall);
    // A pool in a basin.
    for (int x = 150; x < 220; ++x)
        for (int y = H - 30; y < H - 12; ++y)
            g.set_element(x, y, (x == 150 || x == 219) ? ElementType::Wall : ElementType::Water);
    // A slab on a single post; the post is removed below so it comes down.
    for (int x = 20; x < 90; ++x)
        for (int y = 60; y < 66; ++y) g.set_element(x, y, ElementType::Wall);
    g.set_element(55, 66, ElementType::Wall);
    g.set_element(55, 66, ElementType::Empty);
    // A burning beam.
    for (int x = 100; x < 140; ++x)
        for (int y = 90; y < 93; ++y) g.set_element(x, y, ElementType::Wood);
    for (int x = 110; x < 114; ++x) g.set_element(x, 89, ElementType::Fire);

    run.player = Player(120, H - 12 - Player::HEIGHT);
    run.player.settle(g);
    run.spawn_enemy(60, H - 12 - species::GHOUL.height, species::GHOUL);
    run.spawn_enemy(200, H - 30 - species::GHOUL.height - 40, species::GHOUL);
}

// The inputs for step i: walking, shooting, digging, pouring sand into the pool
// with the brush, and one spawn -- every path through Run::step.
Input input_for(int i) {
    Input in;
    in.right = (i / 40) % 2 == 0;
    in.left = !in.right;
    in.jump = i % 50 == 0;
    in.shoot = true;
    in.cursor_x = 60 + (i % 100);
    in.cursor_y = H - 20;
    in.dig = i % 7 == 0;
    in.brush_active = i % 3 == 0;
    in.brush_type = ElementType::Sand;
    in.brush_size = 3;
    in.cursor_x = in.brush_active ? 180 : in.cursor_x;
    if (i == 100) in.command = Command::spawn(species::TROLL);
    return in;
}

// What a pass exercised, so the zero below is a zero over the systems it claims
// to cover rather than over a world where nothing happened.
struct Coverage {
    long allocations = 0;
    int loosed = 0;
    int hits = 0;
    int digs = 0;
    int falling_steps = 0;
    int most_enemies = 0;
};

Coverage run_pass(Run& run) {
    run.reset(SEED);
    build(run);
    Coverage c;
    allocations.store(0);
    counting.store(true);
    for (int i = 0; i < STEPS; ++i) {
        run.step(input_for(i));
        // Read inside the counted window on purpose: these are the accessors a
        // frame calls, and they must not allocate either.
        c.loosed += run.events().count(Event::Kind::ArrowLoosed);
        c.hits += run.events().count(Event::Kind::ArrowHit);
        c.digs += run.events().count(Event::Kind::Dug);
        if (run.grid.has_pending_support_checks()) ++c.falling_steps;
        if (run.enemies_alive() > c.most_enemies) c.most_enemies = run.enemies_alive();
    }
    counting.store(false);
    c.allocations = allocations.load();
    return c;
}

}  // namespace

int main() {
    // The counter itself, so a zero below means nothing was allocated rather than
    // that nothing was counted.
    {
        counting.store(true);
        const auto probe = std::make_unique<int>(7);
        counting.store(false);
        check("the replaced operator new counts allocations",
              allocations.load() >= 1 && *probe == 7);
    }

    const auto owner = std::make_unique<Run>(W, H, SEED);
    Run& run = *owner;
    const Coverage first = run_pass(run);
    const Coverage second = run_pass(run);

    check("the scenario looses arrows", first.loosed > 0, std::to_string(first.loosed));
    check("...lands some on an enemy", first.hits > 0, std::to_string(first.hits));
    check("...digs", first.digs > 0, std::to_string(first.digs));
    check("...brings a structure down", first.falling_steps > 0,
          std::to_string(first.falling_steps) + " steps with a piece in the air");
    check("...and spawns a body mid-run", first.most_enemies >= 3,
          std::to_string(first.most_enemies) + " enemies at most");
    check("both passes are the same world, so the second needs nothing new",
          second.loosed == first.loosed && second.digs == first.digs &&
              second.falling_steps == first.falling_steps);
    check("a step replaying a world it has already reached allocates nothing",
          second.allocations == 0,
          std::to_string(second.allocations) + " allocations in " + std::to_string(STEPS) +
              " steps (the first pass made " + std::to_string(first.allocations) + ")");
    return report();
}
