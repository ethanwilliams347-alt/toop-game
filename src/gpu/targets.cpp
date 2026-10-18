#include "gpu/targets.h"
#include "gpu/gl.h"

namespace gpu {

bool RenderTarget::ensure(int width, int height, int internal, bool linear, std::string& why) {
    if (tex && width == w && height == h) return true;
    release();
    if (width <= 0 || height <= 0) {
        why = "a render target has no area";
        return false;
    }
    gl::GenTextures(1, &tex);
    gl::BindTexture(GL_TEXTURE_2D, tex);
    // The upload format is irrelevant (no pixels are passed), but it has to be
    // one the internal format accepts: one channel for GL_R16F, four otherwise.
    const GLenum format = internal == GL_R16F ? GL_RED : GL_RGBA;
    gl::TexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, format, GL_FLOAT, nullptr);
    const GLint f = linear ? GL_LINEAR : GL_NEAREST;
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl::TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl::GenFramebuffers(1, &fbo);
    gl::BindFramebuffer(GL_FRAMEBUFFER, fbo);
    gl::FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (gl::CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        release();
        why = "a light-pass render target is incomplete";
        return false;
    }
    w = width;
    h = height;
    return true;
}

void RenderTarget::release() {
    if (fbo) gl::DeleteFramebuffers(1, &fbo);
    if (tex) gl::DeleteTextures(1, &tex);
    fbo = tex = 0;
    w = h = 0;
}

}  // namespace gpu
