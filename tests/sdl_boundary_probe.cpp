// The SDL boundary probe. Not part of any normal build: sdl_boundary_test
// compiles this file with slop_engine's include directories, and passes only
// when the include below fails to resolve. If it ever compiles, something has
// given the simulation SDL's headers -- see the test in CMakeLists.txt.
#include <SDL.h>
