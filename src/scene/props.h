#pragma once
#include <string>
#include <vector>

// Which non-simulated sprites a scene has, and where along the ground each one
// stands.
//
// A text file rather than a second BMP. A material map is a value for every cell
// in the world; a prop list is a handful of records. The scene BMPs are
// grid-sized, so a parallel prop map would be megabytes of black to carry a few
// meaningful pixels -- and it still could not say which sprite each pixel meant
// without a legend row per prop, which is scene/legend.h's frozen table growing
// a row every time an artist draws something. Per-cell data gets an image, a
// list gets a list.
//
// There is no y coordinate, and that is the format's one real opinion. Props are
// planted by scanning the terrain actually underneath them (boot::plant_props)
// rather than by an authored ground line, because the ground is not one number:
// a floor slab and an authored slope rising above it are both "the ground". A y
// in the file would be a number the loader ignores, and a number the loader
// ignores is one an author will eventually spend an afternoon tuning.
//
// Format, one record per line, `#` to end-of-line is a comment:
//
//     # sprite   x
//     tree_a     145
//     tree_b     187.5
//
// `sprite` names assets/<sprite>.bmp. `x` is a world cell column, the sprite's
// bottom-centre. Blank lines are fine. Anything else is an error with a line
// number, never a silently skipped row.
struct PropDef {
    std::string sprite; // asset stem; resolves to assets/<sprite>.bmp
    float x = 0.0f;     // world cell, the sprite's bottom-centre column
    int line = 0;       // 1-based source line, for diagnostics
};

// Parses a prop list. Returns the records; on any malformed line the list comes
// back empty and `error` (if non-null) holds a message naming the line.
//
// All-or-nothing rather than skip-the-bad-row. A prop list that drops the line
// it could not read renders the scene wrong and says nothing, which is the same
// silent failure an unmatched legend colour produces. scene/legend.h states the
// rule this follows: an unmatched value must not be quietly conflated with
// "nothing here".
//
// A missing file is not an error and yields an empty list: a scene with no props
// is a legitimate scene, and it is distinguishable from a broken one by `error`
// being left untouched.
std::vector<PropDef> load_prop_list(const std::string& path, std::string* error = nullptr);

// True if `sprite` is a name this loader will turn into a path. Rejects empty
// names, path separators and `..`, so a prop list can never reach outside
// assets/ -- data that names a path is data that can name any path. Exposed for
// the test, which is the only reason it is not static.
bool prop_sprite_name_ok(const std::string& sprite);
