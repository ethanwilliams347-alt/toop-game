// How the angler moves, in numbers and frames. Not a registered test, for the
// same reason the other probes are not: whether a step reads as a step or a
// strike as a strike is feel, and a test pinning today's feel would be a copy of
// the constants rather than a check on them. enemy_test holds the angler to the
// rules that are not taste (feet never skate, never both up, the bulb hits what
// it touches).
//
//   angler_probe <scenario> [frames.raw]
//
// Scenarios: walk (flat, then up a step and down a slope), chase (runs after a
// target that doubles back behind it), strike (a target in reach), sever (the
// rear leg shot off mid-walk). Each prints a line of numbers per few steps and a
// summary, and, given a path, writes every frame drawn from Enemy::pixel_at --
// what an arrow would hit -- as a sequence of {int w, int h, w*h BGRA}. Turn that
// into a GIF and a contact sheet with tools/angler_frames.py.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "physics/enemy.h"
#include "physics/arrow.h"

namespace {

constexpr int WW = 620, WH = 180, FLOOR = 160;
constexpr int CW = 240, CH = 170;
constexpr uint32_t SKY = 0xFF201830u;

struct Frames {
    std::FILE* f = nullptr;
    void add(const Grid& g, const Enemy& e, int tx, int ty, bool show_target, int cx) {
        if (!f) return;
        std::vector<uint32_t> px(static_cast<size_t>(CW) * CH, SKY);
        const int x0 = cx - CW / 2, y0 = FLOOR + 8 - CH;
        const Species& k = e.species();
        for (int y = 0; y < CH; ++y)
            for (int x = 0; x < CW; ++x) {
                const int wx = x0 + x, wy = y0 + y;
                uint32_t c = SKY;
                const Element el = g.get_element(wx, wy);
                if (el.type == ElementType::Wall)
                    c = 0xFF505058u;
                else if (el.type != ElementType::Empty)
                    c = el.color | 0xFF000000u;
                if (const int i = e.pixel_at(wx, wy); i >= 0)
                    c = k.art->color_at(i % k.frame_w(), i / k.frame_w());
                if (show_target && wx >= tx && wx < tx + Player::WIDTH && wy >= ty &&
                    wy < ty + Player::HEIGHT &&
                    (wx == tx || wx == tx + Player::WIDTH - 1 || wy == ty ||
                     wy == ty + Player::HEIGHT - 1))
                    c = 0xFFFFD040u;
                px[static_cast<size_t>(y) * CW + x] = c;
            }
        const int hdr[2] = {CW, CH};
        std::fwrite(hdr, sizeof hdr, 1, f);
        std::fwrite(px.data(), 4, px.size(), f);
    }
};

// A floor, and for the walk a raised step and a slope down off its far end.
Grid world(bool terrain) {
    Grid g(WW, WH);
    for (int y = FLOOR; y < WH; ++y)
        for (int x = 0; x < WW; ++x) g.set_element(x, y, ElementType::Wall);
    if (terrain) {
        for (int x = 230; x < 360; ++x)
            for (int y = FLOOR - 5; y < FLOOR; ++y) g.set_element(x, y, ElementType::Wall);
        // One in two down from the step's top to the floor.
        for (int x = 360; x < 370; ++x)
            for (int y = FLOOR - 5 + (x - 360) / 2; y < FLOOR; ++y)
                g.set_element(x, y, ElementType::Wall);
    }
    return g;
}

// Where a rest pixel of the angler is drawn in the world now.
void at(const Enemy& e, rig::Joint j, int& x, int& y) { e.world_of(j.x, j.y, x, y); }

// Per-step tallies the scenarios share: which feet are off the ground, how
// often both are, how far a planted foot moved between steps.
struct Feet {
    const tentacle::Set& set = *species::ANGLER.tentacles;
    int prev[2][2] = {};
    bool had_prev = false;
    int rear_up = 0, front_up = 0, both_up = 0, lifts = 0, skates = 0;
    bool was_up[2] = {false, false};

    void tally(const Grid& g, const Enemy& e) {
        bool up[2];
        int pos[2][2];
        for (int k = 0; k < 2; ++k) {
            const tentacle::Chain& c = set.chain[k];
            if (!e.has_pixel(c.rest[c.links()].x, c.rest[c.links()].y)) {
                up[k] = false;
                continue;
            }
            at(e, c.rest[c.links()], pos[k][0], pos[k][1]);
            // Up: nothing solid within two cells below the foot.
            up[k] = !is_solid(g.get_element(pos[k][0], pos[k][1] + 1).type) &&
                    !is_solid(g.get_element(pos[k][0], pos[k][1] + 2).type) &&
                    !is_solid(g.get_element(pos[k][0], pos[k][1] + 3).type);
            if (up[k] && !was_up[k]) ++lifts;
            if (had_prev && !up[k] && !was_up[k] &&
                (std::abs(pos[k][0] - prev[k][0]) > 1 || std::abs(pos[k][1] - prev[k][1]) > 1))
                ++skates;
            prev[k][0] = pos[k][0];
            prev[k][1] = pos[k][1];
            was_up[k] = up[k];
        }
        had_prev = true;
        if (up[0] && !up[1]) ++rear_up;
        if (up[1] && !up[0]) ++front_up;
        if (up[0] && up[1]) ++both_up;
    }
    void report() const {
        std::printf("feet: lifts=%d rear-up=%d front-up=%d both-up=%d skates=%d\n", lifts, rear_up,
                    front_up, both_up, skates);
    }
};

}  // namespace

int main(int argc, char** argv) {
    const std::string which = argc > 1 ? argv[1] : "walk";
    Frames out;
    if (argc > 2) out.f = std::fopen(argv[2], "wb");
    const Species& k = species::ANGLER;
    const tentacle::Set& set = *k.tentacles;
    const tentacle::Chain& lure = set.chain[2];
    const rig::Joint bulb = lure.rest[lure.links()];
    auto chains = std::make_unique<Enemy::Chains>();
    auto e = std::make_unique<Enemy>();
    Feet feet;

    if (which == "walk") {
        Grid g = world(true);
        e->spawn(80, FLOOR - k.height, k, chains.get());
        for (int i = 0; i < 900; ++i) {
            g.update();
            e->update(g, -10000, -10000, false);
            feet.tally(g, *e);
            if (i % 2 == 0) out.add(g, *e, 0, 0, false, e->center_x());
            if (i % 30 == 0) {
                int rx = 0, ry = 0, fx_ = 0, fy = 0;
                at(*e, set.chain[0].rest[set.chain[0].links()], rx, ry);
                at(*e, set.chain[1].rest[set.chain[1].links()], fx_, fy);
                std::printf("step %3d  box %3d,%3d  rear foot %3d,%3d  front foot %3d,%3d\n", i,
                            e->cell_x(), e->cell_y(), rx, ry, fx_, fy);
            }
        }
        feet.report();
    } else if (which == "chase") {
        Grid g = world(false);
        e->spawn(80, FLOOR - k.height, k, chains.get());
        int tx = 400;
        const int ty = FLOOR - Player::HEIGHT;
        for (int i = 0; i < 400; ++i) {
            g.update();
            if (i == 200) tx = 60;  // the target runs back behind it: it turns
            e->update(g, tx, ty, true);
            feet.tally(g, *e);
            if (i % 2 == 0) out.add(g, *e, tx, ty, true, e->center_x());
        }
        feet.report();
    } else if (which == "strike") {
        Grid g = world(false);
        e->spawn(200, FLOOR - k.height, k, chains.get());
        const int tx = 200 + k.width + 6, ty = FLOOR - Player::HEIGHT;
        int wound = -1, hits = 0;
        for (int i = 0; i < 120; ++i) {
            g.update();
            const int before = e->windup_left();
            const bool landed = e->update(g, tx, ty, true);
            if (before == 0 && e->windup_left() > 0 && wound < 0) wound = i;
            int bx = 0, by = 0;
            at(*e, bulb, bx, by);
            if (wound >= 0 && i - wound <= k.windup_steps + 12)
                std::printf("t=%3d windup=%2d bulb %4d,%4d%s\n", i - wound, e->windup_left(), bx,
                            by, landed ? "  HIT" : "");
            hits += landed ? 1 : 0;
            out.add(g, *e, tx, ty, true, 250);
        }
        std::printf("strike: wound up at step %d, %d hit(s)\n", wound, hits);
    } else if (which == "sever") {
        Grid g = world(false);
        e->spawn(80, FLOOR - k.height, k, chains.get());
        const tentacle::Chain& rear = set.chain[0];
        const rig::Joint mid = rear.rest[rear.links() / 2];
        for (int i = 0; i < 500; ++i) {
            g.update();
            if (i == 120) {
                int x = 0, y = 0;
                at(*e, {mid.x - 1, mid.y}, x, y);
                e->shatter(g, x, y, Quiver::BITE_RADIUS);
                at(*e, {mid.x + 2, mid.y}, x, y);
                e->shatter(g, x, y, Quiver::BITE_RADIUS);
                std::printf("step 120: shot the rear leg; %d pixels left, feet=%d\n",
                            e->pixel_count(), e->has_feet() ? 1 : 0);
            }
            e->update(g, -10000, -10000, false);
            feet.tally(g, *e);
            if (i % 2 == 0) out.add(g, *e, 0, 0, false, e->center_x());
        }
        std::printf("box ended at %d\n", e->cell_x());
        feet.report();
    } else {
        std::fprintf(stderr, "unknown scenario %s (walk | chase | strike | sever)\n",
                     which.c_str());
        if (out.f) std::fclose(out.f);
        return 2;
    }
    if (out.f) std::fclose(out.f);
    return 0;
}
