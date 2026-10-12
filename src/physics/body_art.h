#pragma once
#include <cstdint>

// What every enemy's art is, whatever the species: a grid of characters, one
// per pixel and one pixel per world cell, plus the handful of numbers that say
// which of those pixels play a part in the body as well as having a colour.
//
// Pulled out of enemy_art.h when the troll arrived, so that the ghoul and the
// troll are two tables read by one set of rules rather than two copies of the
// rules -- and, more to the point, so that both tables are checked by the same
// static_asserts. Every check below is one an art edit can break without the edit
// looking wrong, and every one of them would present at runtime as a behaviour
// bug rather than as an art bug. enemy_art.h argues why the art lives in
// src/physics/ at all.
namespace body_art {

struct Ink {
    char ch;
    uint32_t argb;
};

struct Art {
    const char* const* rows;
    int w;
    int h;
    const Ink* palette;
    int palette_count;

    // The characters that carry a role on top of a colour. HEAD: lose every one
    // of these pixels and the body is dead. HEART: likewise, and also the root the
    // severing fill grows from -- a limb is attached exactly when a chain of
    // surviving pixels joins it to what is left of the heart.
    const char* head;
    const char* heart;

    // Which columns the collision box covers, in frame space: [box_left,
    // w - box_left). Stated by the art because the art is what has to agree with
    // it; Species checks it against the box it is anchored to.
    int box_left;

    // The first row the arms occupy. Above it, pixels outside the box's columns
    // are shoulder or hood, not a limb, and are not something it attacks with.
    int arm_top;

    // The rows a body stands on. Lose every pixel in them inside the box -- both
    // legs cut through -- and there is nothing left to stand on.
    int foot_rows;

    // The arm by letter, for a body whose arm is not where the box would put one.
    // The fish's grows out of the top of its head and reaches forward over the
    // snout: part of it is above the box, part inside the box's columns, and the
    // head and tail it passes are outside them -- no rule about columns says which
    // of those pixels are arm. So the art names them, and from then on they are
    // the arm everywhere the geometric rule would have been asked (has_arms, the
    // rig's parts). Null for every body whose arms do hang outside the box, which
    // keeps the geometric rule, and its reason, where it fits.
    //
    // A lettered arm is one arm, the front one -- see rig::part_of. `arm_box` is
    // where its letters are in the rest frame, [x0, x1) x [y0, y1): computed by
    // with_arm_box below rather than typed, because a bound that missed a pixel
    // would make that pixel undrawable and unhittable while still counting.
    const char* arm = nullptr;
    struct Rect {
        int x0, y0, x1, y1;
    };
    Rect arm_box{0, 0, 0, 0};

    constexpr int box_right() const { return w - box_left; }  // one past the last box column
    constexpr char at(int x, int y) const { return rows[y][x]; }

    // The pixel's colour, or 0 (transparent) for '.' and for anything not in the
    // palette -- the second case cannot survive to runtime, see well_formed.
    constexpr uint32_t color_at(int x, int y) const {
        const char c = at(x, y);
        for (int i = 0; i < palette_count; ++i)
            if (palette[i].ch == c) return palette[i].argb;
        return 0;
    }

    constexpr bool is_body(int x, int y) const { return at(x, y) != '.'; }
    constexpr bool is_head(int x, int y) const { return contains(head, at(x, y)); }
    constexpr bool is_heart(int x, int y) const { return contains(heart, at(x, y)); }

    // Arms and feet are geometry rather than letters, because both are "where
    // the pixel is", and a letter could disagree with that.
    constexpr bool is_foot(int x, int y) const {
        return is_body(x, y) && y >= h - foot_rows && x >= box_left && x < box_right();
    }
    constexpr bool is_arm(int x, int y) const {
        if (arm != nullptr) return is_body(x, y) && y >= arm_top && contains(arm, at(x, y));
        return is_body(x, y) && y >= arm_top && (x < box_left || x >= box_right());
    }

    static constexpr bool contains(const char* set, char c) {
        for (int i = 0; set[i] != '\0'; ++i)
            if (set[i] == c) return true;
        return false;
    }
};

// The art with its lettered arm's bounding box filled in. Unchanged for an art
// without one.
constexpr Art with_arm_box(Art a) {
    if (a.arm == nullptr) return a;
    Art::Rect r{a.w, a.h, 0, 0};
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            if (a.is_arm(x, y)) {
                r.x0 = x < r.x0 ? x : r.x0;
                r.y0 = y < r.y0 ? y : r.y0;
                r.x1 = x + 1 > r.x1 ? x + 1 : r.x1;
                r.y1 = y + 1 > r.y1 ? y + 1 : r.y1;
            }
    a.arm_box = r;
    return a;
}

// --- the checks every art has to pass -------------------------------------

constexpr int length(const char* s) {
    int n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

// Every row the declared width, and every non-'.' character in the palette.
constexpr bool well_formed(const Art& a) {
    for (int y = 0; y < a.h; ++y) {
        if (length(a.rows[y]) != a.w) return false;
        for (int x = 0; x < a.w; ++x)
            if (a.is_body(x, y) && a.color_at(x, y) == 0) return false;
    }
    return true;
}

constexpr int count_where(const Art& a, bool (Art::*pred)(int, int) const) {
    int n = 0;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            if ((a.*pred)(x, y)) ++n;
    return n;
}

// Every pixel reachable from the heart through 8-connected body pixels. If this
// failed, a freshly spawned body would lose the unreachable pixels to the
// severing pass on the first hit anywhere -- a stray hand dropping off when the
// arrow landed in the other leg.
//
// N is the art's pixel count, a template argument only because a constexpr
// function needs its scratch arrays sized at compile time. The troll's art
// makes this the most expensive constant expression in the build; see the
// /constexpr:steps note in CMakeLists.txt.
template <int N> constexpr bool connected(const Art& a) {
    if (a.w * a.h != N) return false;
    bool seen[N] = {};
    int queue[N] = {};
    int head = 0, tail = 0;
    for (int i = 0; i < N; ++i) {
        if (a.is_heart(i % a.w, i / a.w)) {
            seen[i] = true;
            queue[tail++] = i;
        }
    }
    while (head < tail) {
        const int i = queue[head++];
        const int x = i % a.w, y = i / a.w;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= a.w || ny < 0 || ny >= a.h) continue;
                const int n = ny * a.w + nx;
                if (seen[n] || !a.is_body(nx, ny)) continue;
                seen[n] = true;
                queue[tail++] = n;
            }
        }
    }
    for (int i = 0; i < N; ++i)
        if (a.is_body(i % a.w, i / a.w) && !seen[i]) return false;
    return true;
}

// Something in the bottom row, or the body is drawn standing a cell above every
// floor.
constexpr bool stands_on_bottom_row(const Art& a) {
    for (int x = 0; x < a.w; ++x)
        if (a.is_body(x, a.h - 1)) return true;
    return false;
}

}  // namespace body_art
