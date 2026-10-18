// Every OpenGL entry point the gpu layer calls, once, as an X-macro:
//
//   GL_FUNC(return type, name without the gl prefix, (parameter list))
//
// gpu/gl.h expands it into a function pointer per line (gl::Clear, gl::BindTexture,
// ...) and gpu/gl.cpp expands it again to fill each one from SDL_GL_GetProcAddress.
// That is the whole loader: no GLAD or GLEW, so no new dependency, and nothing is
// linked against opengl32.lib -- SDL's GetProcAddress falls back to the DLL's own
// exports for the GL 1.1 names that wglGetProcAddress refuses to return.
//
// A name used anywhere in src/gpu/ has to be listed here first; the loader
// refuses a context that lacks any of them, so a driver missing one is a clean
// fall back to the SDL renderer at startup rather than a null call mid-frame.
// Only what is called is listed -- 3.3 core has hundreds more.

// --- GL 1.x ---
GL_FUNC(void, Viewport, (GLint x, GLint y, GLsizei w, GLsizei h))
GL_FUNC(void, ClearColor, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))
GL_FUNC(void, Clear, (GLbitfield mask))
GL_FUNC(void, Enable, (GLenum cap))
GL_FUNC(void, Disable, (GLenum cap))
GL_FUNC(GLenum, GetError, (void))
GL_FUNC(const GLubyte*, GetString, (GLenum name))
GL_FUNC(void, GenTextures, (GLsizei n, GLuint* textures))
GL_FUNC(void, DeleteTextures, (GLsizei n, const GLuint* textures))
GL_FUNC(void, BindTexture, (GLenum target, GLuint texture))
GL_FUNC(void, TexParameteri, (GLenum target, GLenum pname, GLint param))
GL_FUNC(void, TexImage2D, (GLenum target, GLint level, GLint internalformat, GLsizei w,
                           GLsizei h, GLint border, GLenum format, GLenum type,
                           const void* pixels))
GL_FUNC(void, TexSubImage2D, (GLenum target, GLint level, GLint x, GLint y, GLsizei w,
                              GLsizei h, GLenum format, GLenum type, const void* pixels))
GL_FUNC(void, PixelStorei, (GLenum pname, GLint param))
GL_FUNC(void, ReadPixels, (GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type,
                           void* pixels))
GL_FUNC(void, DrawArrays, (GLenum mode, GLint first, GLsizei count))
GL_FUNC(void, ActiveTexture, (GLenum texture))
GL_FUNC(void, BlendFuncSeparate, (GLenum src_rgb, GLenum dst_rgb, GLenum src_a, GLenum dst_a))

// --- buffers and vertex arrays ---
GL_FUNC(void, GenBuffers, (GLsizei n, GLuint* buffers))
GL_FUNC(void, DeleteBuffers, (GLsizei n, const GLuint* buffers))
GL_FUNC(void, BindBuffer, (GLenum target, GLuint buffer))
GL_FUNC(void, BufferData, (GLenum target, GLsizeiptr size, const void* data, GLenum usage))
GL_FUNC(void, GenVertexArrays, (GLsizei n, GLuint* arrays))
GL_FUNC(void, DeleteVertexArrays, (GLsizei n, const GLuint* arrays))
GL_FUNC(void, BindVertexArray, (GLuint array))
GL_FUNC(void, EnableVertexAttribArray, (GLuint index))
GL_FUNC(void, VertexAttribPointer, (GLuint index, GLint size, GLenum type, GLboolean normalized,
                                    GLsizei stride, const void* pointer))

// --- shaders ---
GL_FUNC(GLuint, CreateShader, (GLenum type))
GL_FUNC(void, ShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string,
                             const GLint* length))
GL_FUNC(void, CompileShader, (GLuint shader))
GL_FUNC(void, GetShaderiv, (GLuint shader, GLenum pname, GLint* params))
GL_FUNC(void, GetShaderInfoLog, (GLuint shader, GLsizei max, GLsizei* length, GLchar* log))
GL_FUNC(void, DeleteShader, (GLuint shader))
GL_FUNC(GLuint, CreateProgram, (void))
GL_FUNC(void, AttachShader, (GLuint program, GLuint shader))
GL_FUNC(void, LinkProgram, (GLuint program))
GL_FUNC(void, GetProgramiv, (GLuint program, GLenum pname, GLint* params))
GL_FUNC(void, GetProgramInfoLog, (GLuint program, GLsizei max, GLsizei* length, GLchar* log))
GL_FUNC(void, DeleteProgram, (GLuint program))
GL_FUNC(void, UseProgram, (GLuint program))
GL_FUNC(GLint, GetUniformLocation, (GLuint program, const GLchar* name))
GL_FUNC(void, Uniform1i, (GLint location, GLint v0))
GL_FUNC(void, Uniform2f, (GLint location, GLfloat v0, GLfloat v1))

// --- framebuffers ---
GL_FUNC(void, GenFramebuffers, (GLsizei n, GLuint* framebuffers))
GL_FUNC(void, DeleteFramebuffers, (GLsizei n, const GLuint* framebuffers))
GL_FUNC(void, BindFramebuffer, (GLenum target, GLuint framebuffer))
GL_FUNC(void, FramebufferTexture2D, (GLenum target, GLenum attachment, GLenum textarget,
                                     GLuint texture, GLint level))
GL_FUNC(GLenum, CheckFramebufferStatus, (GLenum target))
GL_FUNC(void, BlitFramebuffer, (GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0,
                                GLint dx1, GLint dy1, GLbitfield mask, GLenum filter))
