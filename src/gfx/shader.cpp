#include "gfx/shader.h"

#include <glm/gtc/type_ptr.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace gfx {

namespace {

std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[shader] cannot open %s\n", path.c_str());
        return {};
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

GLuint compile_stage(GLenum stage, const std::string& src, const std::string& path) {
    GLuint sh = glCreateShader(stage);
    const char* c = src.c_str();
    glShaderSource(sh, 1, &c, nullptr);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len), '\0');
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        std::fprintf(stderr, "[shader] compile failed (%s):\n%s\n", path.c_str(), log.c_str());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

}  // namespace

Shader::~Shader() { destroy(); }

Shader::Shader(Shader&& other) noexcept : program_(other.program_) {
    other.program_ = 0;
}

Shader& Shader::operator=(Shader&& other) noexcept {
    if (this != &other) {
        destroy();
        program_ = other.program_;
        other.program_ = 0;
    }
    return *this;
}

void Shader::destroy() {
    if (program_) {
        glDeleteProgram(program_);
        program_ = 0;
    }
}

bool Shader::load(const std::string& vert_path, const std::string& frag_path) {
    destroy();

    std::string vsrc = read_file(vert_path);
    std::string fsrc = read_file(frag_path);
    if (vsrc.empty() || fsrc.empty()) return false;

    GLuint vs = compile_stage(GL_VERTEX_SHADER, vsrc, vert_path);
    if (!vs) return false;
    GLuint fs = compile_stage(GL_FRAGMENT_SHADER, fsrc, frag_path);
    if (!fs) { glDeleteShader(vs); return false; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<size_t>(len), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        std::fprintf(stderr, "[shader] link failed:\n%s\n", log.c_str());
        glDeleteProgram(prog);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    glDetachShader(prog, vs);
    glDetachShader(prog, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    program_ = prog;
    return true;
}

void Shader::use() const { glUseProgram(program_); }

GLint Shader::uniform_loc(const char* name) const {
    return glGetUniformLocation(program_, name);
}

void Shader::set_int(const char* name, int v) const {
    glUniform1i(uniform_loc(name), v);
}

void Shader::set_float(const char* name, float v) const {
    glUniform1f(uniform_loc(name), v);
}

void Shader::set_vec3(const char* name, const glm::vec3& v) const {
    glUniform3fv(uniform_loc(name), 1, glm::value_ptr(v));
}

void Shader::set_mat4(const char* name, const glm::mat4& m) const {
    glUniformMatrix4fv(uniform_loc(name), 1, GL_FALSE, glm::value_ptr(m));
}

}  // namespace gfx
