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
    void set_vec3(const char* name, const glm::vec3& v) const;
    void set_mat4(const char* name, const glm::mat4& m) const;

private:
    void destroy();
    GLint uniform_loc(const char* name) const;

    GLuint program_ = 0;
};

}  // namespace gfx
