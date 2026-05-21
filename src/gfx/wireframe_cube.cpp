#include "gfx/wireframe_cube.h"

namespace gfx {

WireframeCube::~WireframeCube() { destroy(); }

void WireframeCube::destroy() {
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
}

bool WireframeCube::init() {
    destroy();

    // 8 cube corners in [0, 1]^3.
    static const float c[8][3] = {
        {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 0.0f},
        {1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 1.0f},
    };
    // 12 edges as endpoint index pairs: bottom ring, top ring, verticals.
    static const int e[12][2] = {
        {0,1},{1,2},{2,3},{3,0},
        {4,5},{5,6},{6,7},{7,4},
        {0,4},{1,5},{2,6},{3,7},
    };

    float verts[12 * 2 * 3];
    int n = 0;
    for (int i = 0; i < 12; ++i) {
        for (int j = 0; j < 2; ++j) {
            verts[n++] = c[e[i][j]][0];
            verts[n++] = c[e[i][j]][1];
            verts[n++] = c[e[i][j]][2];
        }
    }

    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
                          3 * sizeof(float), reinterpret_cast<void*>(0));
    glBindVertexArray(0);
    return true;
}

void WireframeCube::draw() const {
    if (!vao_) return;
    glBindVertexArray(vao_);
    glDrawArrays(GL_LINES, 0, 12 * 2);
    glBindVertexArray(0);
}

}  // namespace gfx
