#pragma once
#include <string>

// One offscreen render target: a texture and the framebuffer that draws into
// it. The light pipeline (gpu/pipeline.cpp) keeps a handful of these at
// different sizes and formats -- block-resolution ping-pong buffers for the
// propagated light, one texel per cell for the sun, the window's size in
// half-float for the lit frame -- and they all want the same three things:
// made at a size, remade only when the size changes, and released.
//
// GL names are plain unsigned ints here so this header does not drag in
// SDL_opengl.h (gpu/gl.h is private to the .cpp files; see there).
namespace gpu {

struct RenderTarget {
    unsigned tex = 0;
    unsigned fbo = 0;
    int w = 0, h = 0;

    // (Re)makes the target at w x h if it is not that size already. `internal`
    // is the GL internal format (GL_RGBA32F, GL_RGBA16F, GL_R16F) and `linear`
    // whether it is sampled with linear filtering. False, with `why` set, if
    // the framebuffer is incomplete -- the target is then left released, and
    // the caller draws the frame unlit rather than through a broken target.
    bool ensure(int width, int height, int internal, bool linear, std::string& why);
    void release();
};

}  // namespace gpu
