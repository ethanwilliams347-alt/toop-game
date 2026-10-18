#pragma once
#include <SDL.h>
#include <SDL_opengl.h>
#include <string>

// The OpenGL 3.3 core entry points the gpu layer uses, as function pointers in
// namespace gpu::gl -- gl::Clear rather than glClear, so they cannot collide with
// the GL 1.1 prototypes SDL_opengl.h declares (and nothing links those). The list
// is gpu/gl_funcs.inl; see there for why the loader is hand-written.
//
// Private to src/gpu/: this header drags in SDL_opengl.h and, on Windows,
// windows.h. gpu/gl_canvas.h, which the shell includes, does not include it.
namespace gpu::gl {

// A return type and a parameter list cannot be parenthesised; see gl.cpp.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define GL_FUNC(ret, name, params) extern ret(APIENTRY* name) params;
#include "gpu/gl_funcs.inl"
#undef GL_FUNC
// NOLINTEND(bugprone-macro-parentheses)

// Fills every pointer above from the current context. Returns false, with the
// first missing name in `missing`, if the driver lacks any -- every one is
// required, so a partial load is a failed load. Needs a current context.
bool load(std::string& missing);

} // namespace gpu::gl
