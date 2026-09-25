#include "gfx/screenshot.h"

#include <glad/gl.h>

// stb is vendored third-party; its implementation trips -Wextra/-Wpedantic
// (missing-field-initializers, deprecated sprintf). Silence here so those
// warnings don't bury real ones from our own code.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <stb_image_write.h>
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <chrono>
#include <cstdio>
#include <cctype>
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

std::string save_screenshot(int w, int h, const std::string& dir,
                            const std::string& filename) {
    if (w <= 0 || h <= 0) return {};

    // Where the file actually lands, which is not always `dir`.
    //
    // operator/ resolves an ABSOLUTE filename by discarding dir entirely,
    // and a relative one carrying its own subdirectory ("docs/media/x.jpg")
    // lands under dir at a path deeper than dir itself. Creating dir alone
    // left that parent missing, stbi_write_png returned a bare failure, and
    // --shot-file docs/media/x.jpg printed "FAILED" with no reason while the
    // same path spelled absolutely worked.
    const std::filesystem::path out = std::filesystem::path(dir) /
        (filename.empty() ? timestamp_filename() : filename);

    std::error_code ec;
    if (!out.parent_path().empty()) {
        std::filesystem::create_directories(out.parent_path(), ec);
        if (ec) {
            std::fprintf(stderr, "[screenshot] mkdir %s failed: %s\n",
                         out.parent_path().string().c_str(),
                         ec.message().c_str());
            return {};
        }
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

    // Honour the extension the caller asked for. Writing PNG bytes into a
    // file named .jpg is the kind of thing that works until something reads
    // it by extension, and every capture in docs/media is a .jpg.
    std::string ext = out.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(c));
    const bool jpeg = (ext == ".jpg" || ext == ".jpeg");

    const int ok = jpeg
        ? stbi_write_jpg(out.string().c_str(), w, h, channels,
                         flipped.data(), 92)
        : stbi_write_png(out.string().c_str(), w, h, channels,
                         flipped.data(), static_cast<int>(row));
    if (!ok) {
        std::fprintf(stderr, "[screenshot] write %s failed\n", out.string().c_str());
        return {};
    }
    return out.string();
}

}  // namespace gfx
