#include "gpu/gl.h"

namespace gpu::gl {

// The X-macro's arguments are a return type and a parameter list, which cannot
// be parenthesised without ceasing to be either -- hence the NOLINTs below.
// NOLINTBEGIN(bugprone-macro-parentheses)
#define GL_FUNC(ret, name, params) ret(APIENTRY* name) params = nullptr;
#include "gpu/gl_funcs.inl"
#undef GL_FUNC

bool load(std::string& missing) {
    // Through a void* and then the pointer's own type: SDL hands back an object
    // pointer, and a function pointer cast from it is the documented way every GL
    // loader does this. NOLINT because the check exists to flag exactly this cast.
#define GL_FUNC(ret, name, params)                                                                 \
    {                                                                                              \
        void* p = SDL_GL_GetProcAddress("gl" #name);                                               \
        if (!p) {                                                                                  \
            missing = "gl" #name;                                                                  \
            return false;                                                                          \
        }                                                                                          \
        name = reinterpret_cast<ret(APIENTRY*) params>(p); /* NOLINT(*-reinterpret-cast) */        \
    }
#include "gpu/gl_funcs.inl"
#undef GL_FUNC
    return true;
}
// NOLINTEND(bugprone-macro-parentheses)

} // namespace gpu::gl
