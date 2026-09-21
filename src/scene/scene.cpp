#include "scene.h"

int load_scene(Grid& grid, const Scene& scene, int offset_x, int offset_y) {
    if (scene.width <= 0 || scene.height <= 0) return 0;

    // Widened before multiplying, not after. `width * height` as int overflows at
    // ~46k square, so a cast on the far side of the multiply lets the very check
    // meant to reject a bad size wrap to a small positive number.
    const size_t expected = static_cast<size_t>(scene.width) * static_cast<size_t>(scene.height);
    if (scene.materials.size() != expected) return 0;
    if (scene.albedo.size() != expected) return 0;

    int placed = 0;

    for (int y = 0; y < scene.height; ++y) {
        for (int x = 0; x < scene.width; ++x) {
            int scene_idx = y * scene.width + x;
            int grid_x = x + offset_x;
            int grid_y = y + offset_y;

            ElementType type = scene.materials[scene_idx];

            // Empty carries no authored colour -- the material map's legend has no
            // entry for it, so its albedo pixel is whatever the art file happens to
            // hold there. Skipping it also means stamping a scene onto a grid that
            // already has something in it does not clear untouched background back
            // to black.
            if (type == ElementType::Empty) continue;

            uint32_t color = scene.albedo[scene_idx];
            grid.paint(grid_x, grid_y, type, color);
            ++placed;
        }
    }

    // Counted rather than assumed. This is the number that tells a caller the
    // difference between "the scene loaded" and "the scene parsed" -- a palette
    // change that makes every pixel resolve to Empty is otherwise silent.
    //
    // It counts cells the loader asked for, including any that fell outside the
    // grid and were dropped by paint()'s bounds check.
    return placed;
}
