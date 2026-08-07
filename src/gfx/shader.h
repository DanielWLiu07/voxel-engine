#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <string>

namespace gfx {

class Shader {
public:
    Shader() = default;
    ~Shader();

    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;

    bool load(const std::string& vert_path, const std::string& frag_path);

    void use() const;
    GLuint id() const { return program_; }

    void set_int(const char* name, int v) const;
    void set_float(const char* name, float v) const;
    void set_vec2(const char* name, const glm::vec2& v) const;
    void set_vec3(const char* name, const glm::vec3& v) const;
    void set_mat4(const char* name, const glm::mat4& m) const;

    // Array uniforms (u_light_vp[N], u_cascade_far[N], u_palette[N]).
    // Every setter here tolerates a missing uniform the same way the
    // scalar ones do: a compiler that optimized the uniform away returns
    // location -1, and glUniform* on -1 is defined as a no-op. That is
    // what lets one shared draw path feed shaders that do not all declare
    // the same uniforms (the shadow and wireframe passes skip most).
    void set_mat4_array(const char* name, const glm::mat4* m, int count) const;
    void set_float_array(const char* name, const float* v, int count) const;
    void set_vec3_array(const char* name, const glm::vec3* v, int count) const;

private:
    void destroy();
    GLint uniform_loc(const char* name) const;

    GLuint program_ = 0;
};

}  // namespace gfx
