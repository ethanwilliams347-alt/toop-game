#pragma once
#include "game/run.h"
#include <cstdint>
#include <string>
#include <vector>

// A recorded session: the seed, the world it started in, and one Input per fixed
// step.
//
// This is what makes a benchmark row a played frame by construction rather than
// by assertion. Every other scenario in grid_bench is hand-built, and whether
// any of them counts as realistic is unwinnable from a taxonomy of synthetic
// scenarios. A run is already a seed plus a replayable list of inputs, and
// tests/test_run.cpp already proves a recorded sequence replays byte-identically
// -- this is that list, written to a file.
//
// One record per fixed step, never per rendered frame. That is why a log
// recorded on a 165 Hz machine replays identically on a 60 Hz one: there is no
// sampling left in it to diverge. A format storing "the input for a frame" plus
// a step count would put the frame rate back into the simulation through the
// instrument built to measure it.
//
// What invalidates a log -- a stale one replays into a world that no longer
// matches and silently measures nothing:
//
//  - the fixture scene changes, so the world the recording started in is not the
//    world the replay starts in;
//  - input handling changes -- a new field on Input, a re-bound key, a changed
//    brush size step -- so the same bytes mean something different;
//  - the simulation changes, which is the interesting case rather than a
//    failure: a change that alters physics will legitimately replay to a
//    different end state, and the row is still valid. That is why the end-state
//    check below reports rather than refuses.
//
// The first two are caught by the fingerprints and the version; the third cannot
// be distinguished from them automatically, and the replay says so in its output
// rather than pretending to know.
namespace input_log {

// Bumped whenever the record layout or the meaning of a field changes. A log
// written by an older version is refused, not best-guessed: reading a struct
// that has gained a field with the old layout produces inputs that are plausible
// and wrong.
//
// 2: the bow (`shoot`) and the enemy spawn key (`spawn_enemy`) joined Input, as
//    button bits 32 and 64. A version-1 log would read back correctly -- neither
//    bit was ever set -- but a version-2 log read by a version-1 build would drop
//    every shot and every spawn and replay as a different session, so the bump is
//    for the reader that cannot know what it is missing.
//
// 3: the troll spawn key (`spawn_troll`), as button bit 128 -- the last free bit
//    in the byte, so the next button grows the record. Bumped for the reason 2
//    was.
//
// 4: the header names the scene the session was played in. Without it a log
//    could not say which world it belonged to, and the replay bench guessed --
//    wrongly, once the fixture it guessed stopped being a scene.
constexpr uint32_t FORMAT_VERSION = 4;

// The longest scene name a log stores. Scene names are short identifiers
// (scene_list::scene_name_ok); the cap is what lets read() refuse a corrupt
// length rather than allocate whatever it says.
constexpr uint32_t MAX_SCENE_NAME = 64;

struct Header {
    uint32_t version = FORMAT_VERSION;
    int32_t grid_w = 0;
    int32_t grid_h = 0;
    uint64_t seed = 0;
    // The scene_list name of the scene the session was played in. The replay
    // rebuilds the world from it with level::start, exactly as the game did.
    std::string scene;

    // The world the recording started in, captured after the scene was stamped and
    // before the first step. `scene_cells` is load_scene's own return value, and
    // `start_fingerprint` is every cell of the grid. Both are checked on replay,
    // because they fail differently: a changed legend moves the fingerprint while
    // leaving the count alone, and a changed scene file moves both.
    int32_t scene_cells = 0;
    uint64_t start_fingerprint = 0;

    // Where the recorded session actually ended up. Checked on replay and reported
    // rather than enforced -- see the note above about the third way a log goes
    // stale.
    uint64_t end_fingerprint = 0;
    int32_t end_player_x = 0;
    int32_t end_player_y = 0;
};

struct Log {
    Header header;
    std::vector<Input> steps;
};

// Every cell of the grid, hashed. Uses the same four fields test_run.cpp's
// worlds_match compares, so "the fingerprints agree" and "the suites' notion of
// an identical world" mean the same thing.
uint64_t fingerprint(const Grid& grid);

// The grid's fingerprint plus everything else in a Run that a level puts there:
// the player's position and health, every live enemy's species, position and
// pixel count, and the objective. This is what the header's start and end
// fingerprints hold.
//
// The grid alone was not enough. A replay that rebuilt the right terrain with the
// wrong enemies -- a stale level file, a planter that changed -- matched the
// grid fingerprint exactly and then replayed a different fight without a word.
uint64_t fingerprint(const Run& run);

bool write(const char* path, const Log& log, std::string* error);
bool read(const char* path, Log& log, std::string* error);

} // namespace input_log
