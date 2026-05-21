#include "gfx/water.h"

#include <cstdint>
#include <vector>

namespace gfx {

WaterPlane::~WaterPlane() { destroy(); }

void WaterPlane::destroy() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
    index_count_ = 0;
}

bool WaterPlane::init(float size_blocks, int segments) {
    destroy();
    if (segments < 1) segments = 1;

    const int verts_per_side = segments + 1;
    const float half = size_blocks * 0.5f;
    const float step = size_blocks / static_cast<float>(segments);

    // Position-only vertex stream. Y is left at 0 here; the vertex shader
    // overwrites it with sea_level + wave displacement so this CPU buffer
    // doesn't need a normal/uv channel.
    std::vector<float> positions;
    positions.reserve(static_cast<std::size_t>(verts_per_side) * verts_per_side * 3);
    for (int j = 0; j < verts_per_side; ++j) {
        float z = -half + step * static_cast<float>(j);
        for (int i = 0; i < verts_per_side; ++i) {
            float x = -half + step * static_cast<float>(i);
            positions.push_back(x);
            positions.push_back(0.0f);
            positions.push_back(z);
        }
    }

    // Two triangles per cell, CCW-wound looking down +Y so back-face
    // culling (if enabled) keeps the top visible from above.
    std::vector<std::uint32_t> indices;
    indices.reserve(static_cast<std::size_t>(segments) * segments * 6);
    for (int j = 0; j < segments; ++j) {
        for (int i = 0; i < segments; ++i) {
            std::uint32_t v00 = static_cast<std::uint32_t>(j * verts_per_side + i);
            std::uint32_t v10 = v00 + 1;
            std::uint32_t v01 = v00 + verts_per_side;
            std::uint32_t v11 = v01 + 1;
            indices.push_back(v00);
            indices.push_back(v01);
            indices.push_back(v11);
            indices.push_back(v00);
            indices.push_back(v11);
            indices.push_back(v10);
        }
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenBuffers(1, &ebo_);

    glBindVertexArray(vao_);

    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(positions.size() * sizeof(float)),
                 positions.data(), GL_STATIC_DRAW);

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(indices.size() * sizeof(std::uint32_t)),
                 indices.data(), GL_STATIC_DRAW);

    // location 0: position (vec3). Matches the water.vert layout.
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          3 * sizeof(float),
                          reinterpret_cast<void*>(0));

    glBindVertexArray(0);

    index_count_ = indices.size();
    return true;
}

void WaterPlane::draw() const {
    if (!vao_ || index_count_ == 0) return;
    glBindVertexArray(vao_);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count_),
                   GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);
}

}  // namespace gfx
