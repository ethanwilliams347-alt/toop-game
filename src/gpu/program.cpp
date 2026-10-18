#include "gpu/program.h"
#include <fstream>
#include <sstream>
#include "gpu/gl.h"

namespace gpu {
namespace {

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

// Compiles one stage. On failure the shader is deleted and the log says why.
GLuint compile(GLenum type, const std::string& source, const std::string& name, std::string& log) {
    const GLuint s = gl::CreateShader(type);
    const GLchar* text = source.c_str();
    gl::ShaderSource(s, 1, &text, nullptr);
    gl::CompileShader(s);
    GLint ok = 0;
    gl::GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[2048] = {};
        gl::GetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        log = name + ": " + buf;
        gl::DeleteShader(s);
        return 0;
    }
    return s;
}

}  // namespace

bool build_program(const std::string& dir, const char* vert, const char* frag, unsigned& out,
                   std::string& log) {
    const std::string vpath = dir + "/" + vert;
    const std::string fpath = dir + "/" + frag;
    std::string vsrc, fsrc;
    if (!read_file(vpath, vsrc)) {
        log = "cannot read " + vpath;
        return false;
    }
    if (!read_file(fpath, fsrc)) {
        log = "cannot read " + fpath;
        return false;
    }
    const GLuint vs = compile(GL_VERTEX_SHADER, vsrc, vpath, log);
    if (!vs) return false;
    const GLuint fs = compile(GL_FRAGMENT_SHADER, fsrc, fpath, log);
    if (!fs) {
        gl::DeleteShader(vs);
        return false;
    }
    const GLuint p = gl::CreateProgram();
    gl::AttachShader(p, vs);
    gl::AttachShader(p, fs);
    gl::LinkProgram(p);
    gl::DeleteShader(vs);  // flagged; freed with the program
    gl::DeleteShader(fs);
    GLint ok = 0;
    gl::GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char buf[2048] = {};
        gl::GetProgramInfoLog(p, sizeof(buf), nullptr, buf);
        log = fpath + " program: " + buf;
        gl::DeleteProgram(p);
        return false;
    }
    out = p;
    return true;
}

}  // namespace gpu
