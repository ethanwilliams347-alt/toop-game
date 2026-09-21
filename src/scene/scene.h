#pragma once
#include "physics/material.h"
#include "physics/grid.h"
#include <vector>
#include <cstdint>

struct Scene {
    int width = 0;
    int height = 0;
    std::vector<ElementType> materials;
    std::vector<uint32_t> albedo;
};

// Stamps `scene` into `grid` and returns how many cells it named a material
// for -- zero meaning the scene is empty or malformed. The count is the return
// value because "parsed without error" and "actually put something in the
// world" are otherwise indistinguishable to a caller. See src/scene/legend.h.
int load_scene(Grid& grid, const Scene& scene, int offset_x = 0, int offset_y = 0);
