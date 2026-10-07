#pragma once
#include <cstdint>

// The enemy's body, one character per pixel and one pixel per world cell.
//
// In src/physics/ rather than as a BMP in assets/, and that is the unusual
// choice here, so it gets argued. For the player the sprite is a picture of the
// body: Player is a box, and what the sheet draws over it is presentation that
// the simulation never reads. For an enemy the picture IS the body. Which pixels
// exist decides where an arrow connects, which pixels an arrow removes decide
// where sand appears in the grid, and the colour of each pixel is the colour of
// the grain it turns into. All three are simulation state, so the table they come
// from has to be something the simulation can read without a file, without SDL
// and identically on every machine -- a BMP that failed to load would be a world
// with different physics in it, not a world drawn wrong.
//
// An ASCII grid is also the form tools/player_sheet.py argues for, and for the
// same reason: this is a single pose, and a single pose reviewed as text is a
// one-pixel change reviewed as a one-character change.
//
// One pose, deliberately. The damage mask lives in frame space, so a walk cycle
// whose legs move between frames would let a hit leg grow back on the next frame
// -- the mask would be marking pixels the new frame draws somewhere else. A
// second frame is fine the day it only differs in pixels that cannot be hit.
namespace enemy_art {

// Frame size, matching the player's sheet: the enemy is the same kind of thing
// as the player at the same scale, and the collision box (Enemy::WIDTH/HEIGHT)
// is the player's. The arms hang outside the box exactly as the player's
// sleeves do -- a sleeve over a wall is art, an arm over a wall is still an arm
// an arrow can hit.
inline constexpr int W = 14;
inline constexpr int H = 26;

// The body. '.' is nothing; every other character is a palette entry below.
//
// Three letters carry a role as well as a colour, which is why the torso and the
// eyes are spelt with letters that otherwise duplicate a neighbour's colour:
//
//  - 'E', the eyes, are the HEAD. Lose both and the thing is dead. A head shot
//    is one arrow because the bite of one arrow spans both.
//  - 'm' and 'l', the chest, are the HEART. Lose all of it and the thing is dead.
//    It is also the root the severing fill grows from, so a limb is "attached"
//    exactly when a chain of surviving pixels joins it to what is left of the
//    chest -- cut through a shoulder and the whole arm comes off, not just the
//    bite.
//
// The arms are everything outside the collision box's columns from the
// shoulders down, and the feet are what is inside those columns in the bottom
// FOOT_ROWS rows. Both are geometry rather than letters, because both are "where
// the pixel is", and a letter could disagree with that.
//
// The column of '.' between each arm and the ribs, and between the legs below
// the hip, is load-bearing, not decoration. A limb drawn flush against its
// neighbour is joined to it down its whole length, so the severing fill finds it
// still attached below any cut and a shot through the elbow takes a bite and
// nothing else. With the gap, an arm hangs from the shoulder alone and a leg
// from the hip alone -- which is what makes them come off.
inline constexpr const char* ROWS[H] = {
    "..............",
    "....KKKKKK....",
    "...KDMMMMDK...",
    "..KDMLGGLMDK..",
    "..KMLBBBBLMK..",
    "..KBBBBBBBBK..",
    "..KBKEBBKEBK..",
    "..KBbBbbBbBK..",
    "..KDBKKKKBDK..",
    "...KDBbbBDK...",
    "..KKDDMMDDKK..",
    ".KDMMLllLMMDK.",
    "KDMLKMlllMKLMK",
    "KML.KmlmmK.LMK",
    "KML.KmmmmK.LMK",
    "KMD.KMmmMK.DMK",
    "KLD.KDmmDK.DLK",
    "KMD.KDMMDK.DMK",
    "KBb.KDDDDK.bBK",
    "BKB..KMMK..BKB",
    "B.B..MDKDM..B.",
    "....KMD.DMK...",
    "....KDD.DDK...",
    "....KMD.DMK...",
    "....KDD.DDK...",
    "....KKK.KKK...",
};

// The game's palette (assets/palette.gpl) where it has the colour -- the moss is
// the forest's tree_* ramp and the mask is the owl's light feather tone -- so the
// creature reads as made of the same world it walks through, and the sand it
// leaves behind sits in that world's colours rather than the Sand row's.
//
// The eyes are the one colour from outside it, and the one that has to be: two
// amber points are what a player reads first at four screen pixels per cell, and
// they are what tells you where the head is in a dark scene.
struct Ink {
    char ch;
    uint32_t argb;
};
inline constexpr Ink PALETTE[] = {
    {'K', 0xFF101410},  // outline, a hair off black so it is still moss
    {'D', 0xFF182016},  // tree_shadow
    {'M', 0xFF2A3824},  // tree_mid
    {'L', 0xFF475939},  // tree_lit
    {'G', 0xFF69783A},  // rim_grass, the moss on the crown
    {'B', 0xFFB7B096},  // the bone mask
    {'b', 0xFF7D7755},  // its shadow
    {'E', 0xFFFFB040},  // eyes -- HEAD
    {'m', 0xFF2A3824},  // chest, tree_mid -- HEART
    {'l', 0xFF475939},  // chest, tree_lit -- HEART
};
inline constexpr int PALETTE_COUNT = static_cast<int>(sizeof(PALETTE) / sizeof(PALETTE[0]));

// The rows a body stands on. Lose every pixel in them -- both legs cut through
// anywhere, so both feet come away -- and there is nothing left to stand on.
inline constexpr int FOOT_ROWS = 2;

// The first row the arms occupy. Above it, the pixels outside the box's columns
// are the sides of the hood, not a limb, and a hood is not something you swipe
// with.
inline constexpr int ARM_TOP = 11;

// Which columns the collision box covers, in frame space. The same arithmetic as
// Enemy::OFFSET_X, stated here because the art is what has to agree with it and
// this header cannot include the one that includes it.
inline constexpr int BOX_LEFT = 3;
inline constexpr int BOX_RIGHT = W - BOX_LEFT;  // one past the last box column

// The pixel's colour, or 0 (transparent) for '.' and for anything not in the
// palette -- the second case cannot survive to runtime, see art_is_well_formed.
constexpr uint32_t color_at(int x, int y) {
    const char c = ROWS[y][x];
    for (int i = 0; i < PALETTE_COUNT; ++i)
        if (PALETTE[i].ch == c) return PALETTE[i].argb;
    return 0;
}

constexpr bool is_body(int x, int y) { return ROWS[y][x] != '.'; }
constexpr bool is_head(int x, int y) { return ROWS[y][x] == 'E'; }
constexpr bool is_heart(int x, int y) { return ROWS[y][x] == 'm' || ROWS[y][x] == 'l'; }
constexpr bool is_foot(int x, int y) {
    return is_body(x, y) && y >= H - FOOT_ROWS && x >= BOX_LEFT && x < BOX_RIGHT;
}
constexpr bool is_arm(int x, int y) {
    return is_body(x, y) && y >= ARM_TOP && (x < BOX_LEFT || x >= BOX_RIGHT);
}

// --- the art has to be a body, and the compiler checks that it is ----------
//
// Every relationship below is one an edit to the grid can break without the
// edit looking wrong, and every one of them would present at runtime as a
// behaviour bug rather than as an art bug.
namespace detail {

constexpr int length(const char* s) {
    int n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

constexpr bool art_is_well_formed() {
    for (int y = 0; y < H; ++y) {
        if (length(ROWS[y]) != W) return false;
        for (int x = 0; x < W; ++x)
            if (is_body(x, y) && color_at(x, y) == 0) return false;
    }
    return true;
}

constexpr int count_where(bool (*pred)(int, int)) {
    int n = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (pred(x, y)) ++n;
    return n;
}

// Every pixel reachable from the heart through 8-connected body pixels. If this
// failed, a freshly spawned enemy would lose the unreachable pixels to the
// severing pass on the first hit anywhere -- a stray hand dropping off when the
// arrow landed in the other leg.
constexpr bool art_is_connected() {
    bool seen[W * H] = {};
    int queue[W * H] = {};
    int head = 0, tail = 0;
    for (int i = 0; i < W * H; ++i) {
        if (is_heart(i % W, i / W)) {
            seen[i] = true;
            queue[tail++] = i;
        }
    }
    while (head < tail) {
        const int i = queue[head++];
        const int x = i % W, y = i / W;
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if (nx < 0 || nx >= W || ny < 0 || ny >= H) continue;
                const int n = ny * W + nx;
                if (seen[n] || !is_body(nx, ny)) continue;
                seen[n] = true;
                queue[tail++] = n;
            }
        }
    }
    for (int i = 0; i < W * H; ++i)
        if (is_body(i % W, i / W) && !seen[i]) return false;
    return true;
}

constexpr bool feet_reach_bottom_row() {
    for (int x = 0; x < W; ++x)
        if (is_body(x, H - 1)) return true;
    return false;
}

}  // namespace detail

static_assert(detail::art_is_well_formed(),
              "enemy_art::ROWS has a row of the wrong width or a character with no "
              "PALETTE entry");
static_assert(detail::count_where(is_head) > 0 && detail::count_where(is_heart) > 0,
              "the enemy art needs at least one HEAD ('E') and one HEART ('m'/'l') "
              "pixel, or it is born dead");
static_assert(detail::count_where(is_foot) > 0,
              "the enemy art has nothing in its FOOT_ROWS inside the box, so it is "
              "born footless and collapses on its first hit");
static_assert(detail::count_where(is_arm) > 0,
              "the enemy art has no pixels outside the box below ARM_TOP, so it is "
              "born armless and can never swipe");
static_assert(detail::art_is_connected(),
              "some enemy pixel is not 8-connected to the heart; it would fall off "
              "as sand the first time the enemy is hit anywhere");
static_assert(detail::feet_reach_bottom_row(),
              "the enemy art floats: its bottom row is empty, so it would be drawn "
              "standing a cell above every floor");

inline constexpr int PIXEL_COUNT = detail::count_where(is_body);

}  // namespace enemy_art
