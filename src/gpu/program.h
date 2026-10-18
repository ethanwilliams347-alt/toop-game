#pragma once
#include <string>

// Compiling and linking a shader program from two files in a shader folder,
// shared by GlCanvas (sprite, cells) and Pipeline (the light passes).
namespace gpu {

// Reads `dir`/`vert` and `dir`/`frag`, compiles and links them. On success puts
// the program in `out` and returns true; on any failure returns false with the
// file and the driver's log in `log`, and `out` is untouched.
bool build_program(const std::string& dir, const char* vert, const char* frag, unsigned& out,
                   std::string& log);

}  // namespace gpu
