#pragma once

#include <glad/gl.h>
#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace gfx {

struct VertexPNT {
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
    float     ao;        // 0 = fully occluded, 1 = no occlusion (vertex AO)
    float     block_id;  // BlockId cast to float; shader uses it as a palette index
};

class Mesh {
public:
    Mesh() = default;
    ~Mesh();

    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept;
    Mesh& operator=(Mesh&& other) noexcept;

    void upload(std::span<const VertexPNT> vertices, std::span<const std::uint32_t> indices);
    void draw() const;

    std::size_t index_count() const { return index_count_; }

private:
    void destroy();

    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    GLuint ebo_ = 0;
    std::size_t index_count_ = 0;
};

}  // namespace gfx
