#pragma once

// GENERATED FILE - do not edit.
//   python tools/generate_backdrop.py --header
//
// These four numbers per layer used to exist twice - once in the C++ that
// draws the layer and once in the Python that sizes its image - with a
// comment in each asking a human to keep them in step. The failure mode of
// that arrangement is a seam at the pan limit: a layer runs out of image
// before the camera runs out of world, and nothing says so until somebody
// walks to the edge of the map.
//
// tools/generate_backdrop.py is the source. It derives both the factors and
// the sizes, so a change on that side cannot leave this side stale.
//
// `width`/`height` are what the generator would write for this layer at
// these factors. main.cpp compares them against what it actually loaded and
// warns on a mismatch, which turns the seam from a pixel nobody reaches
// into a line at startup.
namespace backdrop_layers {

// A wrapping layer's width/height is its tile size and is exact, where a
// pan-sized layer's is a minimum. main.cpp's warning reads it the second
// way for both, which is the right direction for the case that matters: a
// tile smaller than generated repeats sooner than the art was drawn for.
struct Layer {
    float parallax_x;
    float parallax_y;
    int width;  // the BMP size generate_backdrop.py produces at these factors
    int height;
};

// assets/backdrop_sky.bmp
inline constexpr Layer SKY{0.04f, 0.02f, 3678, 1512};
// assets/backdrop_mountains.bmp
inline constexpr Layer MOUNTAINS{0.15f, 0.06f, 4311, 1642};
// assets/backdrop_ground.bmp  (a tile - this layer wraps)
inline constexpr Layer GROUND{0.28f, 0.11f, 256, 256};

// The ground plane is drawn as strips between two depths, so it needs a
// second x factor that no other layer has. GROUND above carries the far edge
// (the horizon); this is the near one. Both are derivations off the
// geometric ladder - see tools/generate_backdrop.py, where the argument
// lives.
inline constexpr float GROUND_NEAR_X = 0.52f;

// Where the plane's far edge goes, as a row of the mountains BMP rather
// than a fraction of the window. A horizon authored independently of where
// the mountain silhouette sits contradicts it at every camera position, and
// the plane is opaque and drawn after the mountains, so it covers the whole
// band. Deriving the horizon from the silhouette makes that
// unrepresentable rather than unlikely.
//
// This is the deepest row the skyline reaches, so the whole jagged edge is
// above the plane and the solid band below it is what the plane may cover.
//
// Generated because it is a fact about the art: mountain_skyline() is a pure
// function of its seed and the two composition fractions, so a change to
// either moves this number without anyone having to remember to.
//
// A fraction of the layer's height and not a row index, which is not
// cosmetic: the renderer multiplies it by whatever mountains texture was
// actually loaded, so the horizon lands on the silhouette for any mountain
// image - including the small synthetic one the golden-frame fixture
// builds. Stated as a row it is a number only one image can satisfy.
// Shipped BMP: 1642 rows, skyline 285..623.
inline constexpr float MOUNTAINS_SKYLINE_MAX = 0.379415f;

// The inputs the sizes above are derived from, so a reader can tell whether
// a mismatch is a stale asset or a changed display table.
inline constexpr int GRID_WIDTH = 1920;
inline constexpr int GRID_HEIGHT = 1080;
inline constexpr int SCALE = 4;

} // namespace backdrop_layers
