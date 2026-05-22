#include "gfx/screenshot.h"

#include <glad/gl.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

namespace gfx {

namespace {

std::string timestamp_filename() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof(buf), "voxel_%Y%m%d_%H%M%S.png", &tm);
    return buf;
}

}  // namespace

std::string save_screenshot(int w, int h, const std::string& dir) {
    if (w <= 0 || h <= 0) return {};

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        std::fprintf(stderr, "[screenshot] mkdir %s failed: %s\n",
                     dir.c_str(), ec.message().c_str());
        return {};
    }

    const int channels = 3;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * channels);

    GLint prev_pack = 4;
    glGetIntegerv(GL_PACK_ALIGNMENT, &prev_pack);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_PACK_ALIGNMENT, prev_pack);

    // glReadPixels gives bottom-left origin; PNG is top-left. Flip rows.
    std::vector<unsigned char> flipped(pixels.size());
    const size_t row = static_cast<size_t>(w) * channels;
    for (int y = 0; y < h; ++y) {
        std::memcpy(&flipped[static_cast<size_t>(y) * row],
                    &pixels[static_cast<size_t>(h - 1 - y) * row], row);
    }

    std::filesystem::path out = std::filesystem::path(dir) / timestamp_filename();
    if (!stbi_write_png(out.string().c_str(), w, h, channels,
                        flipped.data(), static_cast<int>(row))) {
        std::fprintf(stderr, "[screenshot] write %s failed\n", out.string().c_str());
        return {};
    }
    return out.string();
}

}  // namespace gfx
