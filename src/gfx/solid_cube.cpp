#include "gfx/solid_cube.h"

#include <glm/glm.hpp>

#include <cmath>
#include <vector>

namespace gfx {

SolidCube::~SolidCube() { destroy(); }

void SolidCube::destroy() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
    vbo_ = 0;
    vao_ = 0;
}

bool SolidCube::init() {
    destroy();

    // Six faces, each built from its own normal: two vectors spanning the
    // face come from a cross product, so there is no 36-entry table to
    // get one sign wrong in.
    static const glm::vec3 kNormals[6] = {
        { 0,  0,  1}, { 0,  0, -1}, { 1,  0,  0},
        {-1,  0,  0}, { 0,  1,  0}, { 0, -1,  0},
    };

    std::vector<float> verts;
    verts.reserve(36 * 6);
    for (const glm::vec3& n : kNormals) {
        const glm::vec3 up = (std::fabs(n.y) > 0.5f) ? glm::vec3(0, 0, 1)
                                                     : glm::vec3(0, 1, 0);
        const glm::vec3 t = glm::normalize(glm::cross(up, n));
        const glm::vec3 b = glm::cross(n, t);
        const glm::vec3 c = n * 0.5f;
        const glm::vec3 quad[4] = {
            c - t * 0.5f - b * 0.5f, c + t * 0.5f - b * 0.5f,
            c + t * 0.5f + b * 0.5f, c - t * 0.5f + b * 0.5f,
        };
        for (int k : {0, 1, 2, 0, 2, 3}) {
            verts.insert(verts.end(),
                         {quad[k].x, quad[k].y, quad[k].z, n.x, n.y, n.z});
        }
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    if (vao_ == 0 || vbo_ == 0) {
        destroy();
        return false;
    }

    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(), GL_STATIC_DRAW);
    constexpr GLsizei kStride = 6 * sizeof(float);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, kStride, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, kStride,
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glBindVertexArray(0);
    return true;
}

void SolidCube::draw() const {
    if (vao_ == 0) return;
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
}

}  // namespace gfx
