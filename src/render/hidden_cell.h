#pragma once
#include <cstdint>
#include "physics/material.h"

// frame::HiddenCell on its own, without the rest of render/frame.h, because
// frame.h includes SDL and render/cell_pack.h reads these from slop_render, which
// is SDL-free by rule. frame.h includes this, so every existing user still finds
// the type where it was.
namespace frame {

// A cell of a loose body, left out of the cell texture, and what to draw there
// instead. The sprite is drawn where the body continuously is and its cells sit
// at the nearest whole-cell pose, so the two can be up to a cell apart, and
// wherever the sprite does not cover a cell the cell shows. Transparent, in a
// pool, that sliver is a gap of backdrop around a stone sinking through water;
// `fill` is the liquid or grain beside the body instead, so it reads as what the
// body is pushing through. 0 (transparent) in air.
//
// `fill_type` is the material `fill` was read from, Empty in air. The colour is
// all the SDL path needs; a shader that draws from per-cell material data
// (render/cell_pack.h) has to be told what the cell is made of instead, and
// guessing that back from a jittered colour is not reliable. Defaulted, so a
// record written as {index, fill} still means "air" for it.
struct HiddenCell {
    int index;
    uint32_t fill;
    ElementType fill_type = ElementType::Empty;
};

} // namespace frame
