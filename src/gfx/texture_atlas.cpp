#include "gfx/texture_atlas.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace gfx {

namespace {

struct Rgb { std::uint8_t r, g, b; };

float hash2(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B1u
                    + static_cast<std::uint32_t>(y) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13;
    return (h & 0x00FFFFFFu) / 16777216.0f;
}

Rgb mix(Rgb a, Rgb b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return {
        static_cast<std::uint8_t>(a.r + (b.r - a.r) * t),
        static_cast<std::uint8_t>(a.g + (b.g - a.g) * t),
        static_cast<std::uint8_t>(a.b + (b.b - a.b) * t),
    };
}

Rgb jitter(Rgb base, int x, int y, int seed, float amount) {
    float n = hash2(x + seed * 31, y + seed * 17) - 0.5f;
    int dr = static_cast<int>(base.r + n * amount * 255.0f);
    int dg = static_cast<int>(base.g + n * amount * 255.0f);
    int db = static_cast<int>(base.b + n * amount * 255.0f);
    return {
        static_cast<std::uint8_t>(std::clamp(dr, 0, 255)),
        static_cast<std::uint8_t>(std::clamp(dg, 0, 255)),
        static_cast<std::uint8_t>(std::clamp(db, 0, 255)),
    };
}

// Multi-octave value noise sampled at integer pixel coords; smoother than
// the raw hash, closer to what a generated texture would look like.
float fbm(int x, int y, int seed) {
    float total = 0.0f, amp = 0.5f, freq = 1.0f;
    for (int o = 0; o < 4; ++o) {
        total += hash2(static_cast<int>(x * freq) + seed,
                       static_cast<int>(y * freq) + seed * 3) * amp;
        freq *= 2.0f;
        amp  *= 0.5f;
    }
    return total;
}

Rgb paint_tile(int block_id, int x, int y) {
    switch (block_id) {
    case 1: {  // Stone
        Rgb base{140, 140, 145};
        float n = fbm(x, y, 11);
        Rgb tone = mix(base, {95, 95, 100}, n * 0.4f);
        if (hash2(x + 7, y + 13) > 0.93f) tone = mix(tone, {70, 70, 75}, 0.7f);
        return tone;
    }
    case 2: {  // Dirt
        Rgb base{128, 86, 50};
        float n = fbm(x, y, 23);
        Rgb tone = mix(base, {88, 56, 30}, n * 0.5f);
        if (hash2(x, y) > 0.86f) tone = mix(tone, {64, 38, 20}, 0.5f);
        return tone;
    }
    case 3: {  // Grass
        Rgb base{86, 158, 70};
        float n = fbm(x, y, 5);
        Rgb tone = mix(base, {58, 120, 48}, n * 0.45f);
        if (hash2(x * 3, y * 5) > 0.84f) tone = mix(tone, {38, 88, 32}, 0.7f);
        // Faint dirt poking through.
        if (hash2(x + 9, y + 11) > 0.95f) tone = mix(tone, {120, 80, 50}, 0.5f);
        return tone;
    }
    case 4: {  // Sand
        Rgb base{226, 206, 142};
        float n = fbm(x, y, 41);
        Rgb tone = mix(base, {196, 174, 110}, n * 0.3f);
        return tone;
    }
    case 5: {  // Wood
        Rgb base{108, 70, 32};
        float ring = std::sin((x + y) * 0.9f) * 0.5f + 0.5f;
        Rgb tone = mix(base, {72, 46, 20}, ring * 0.4f);
        float n = fbm(x, y, 59);
        tone = mix(tone, {130, 86, 44}, n * 0.15f);
        return tone;
    }
    case 6: {  // Leaves
        Rgb base{56, 118, 50};
        float n = fbm(x, y, 71);
        Rgb tone = mix(base, {32, 78, 30}, n * 0.55f);
        if (hash2(x * 7, y * 11) > 0.94f) tone = mix(tone, {120, 180, 90}, 0.6f);
        return tone;
    }
    case 7: {  // Snow
        Rgb base{242, 244, 250};
        float n = fbm(x, y, 89);
        Rgb tone = mix(base, {218, 226, 240}, n * 0.18f);
        if (hash2(x * 5, y * 3) > 0.97f) tone = mix(tone, {255, 255, 255}, 0.7f);
        return tone;
    }
    default:
        return {255, 0, 255};
    }
}

const char* png_name_for(int block_id) {
    switch (block_id) {
    case 1: return "stone.png";
    case 2: return "dirt.png";
    case 3: return "grass.png";
    case 4: return "sand.png";
    case 5: return "wood.png";
    case 6: return "leaves.png";
    case 7: return "snow.png";
    default: return nullptr;
    }
}

// If textures/<block>.png exists, load it (resized via stb if not exactly
// the tile size). Returns true on success and writes the tile into pixels.
bool try_load_png_tile(int block_id, std::uint8_t* tile_out) {
    const char* fname = png_name_for(block_id);
    if (!fname) return false;
    std::filesystem::path p = std::filesystem::path("textures") / fname;
    if (!std::filesystem::exists(p)) return false;

    int w, h, ch;
    stbi_uc* data = stbi_load(p.string().c_str(), &w, &h, &ch, 4);
    if (!data) {
        std::fprintf(stderr, "[atlas] failed to load %s: %s\n",
                     p.string().c_str(), stbi_failure_reason());
        return false;
    }

    // Nearest-neighbor scale to kAtlasTilePx; we expect the PNG to already
    // be 16x16 or a power-of-two multiple of it, but we don't require it.
    for (int py = 0; py < kAtlasTilePx; ++py) {
        for (int px = 0; px < kAtlasTilePx; ++px) {
            int sx = (px * w) / kAtlasTilePx;
            int sy = (py * h) / kAtlasTilePx;
            const stbi_uc* src = data + (sy * w + sx) * 4;
            std::uint8_t* dst = tile_out + (py * kAtlasTilePx + px) * 4;
            dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        }
    }
    stbi_image_free(data);
    return true;
}

void paint_tile_procedural(int block_id, std::uint8_t* tile_out) {
    for (int py = 0; py < kAtlasTilePx; ++py) {
        for (int px = 0; px < kAtlasTilePx; ++px) {
            Rgb c = paint_tile(block_id, px, py);
            std::uint8_t* dst = tile_out + (py * kAtlasTilePx + px) * 4;
            dst[0] = c.r; dst[1] = c.g; dst[2] = c.b; dst[3] = 255;
        }
    }
}

}  // namespace

GLuint generate_block_atlas() {
    constexpr int kAtlasBytes = kAtlasSizePx * kAtlasSizePx * 4;
    std::array<std::uint8_t, kAtlasBytes> pixels{};

    int loaded_pngs = 0;
    for (int tile = 0; tile < kAtlasTilesDim * kAtlasTilesDim; ++tile) {
        std::array<std::uint8_t, kAtlasTilePx * kAtlasTilePx * 4> tile_buf{};

        if (try_load_png_tile(tile, tile_buf.data())) {
            ++loaded_pngs;
        } else {
            paint_tile_procedural(tile, tile_buf.data());
        }

        const int tx = tile % kAtlasTilesDim;
        const int ty = tile / kAtlasTilesDim;
        for (int py = 0; py < kAtlasTilePx; ++py) {
            const int row_dst = (ty * kAtlasTilePx + py) * kAtlasSizePx * 4
                              + tx * kAtlasTilePx * 4;
            std::memcpy(pixels.data() + row_dst,
                        tile_buf.data() + py * kAtlasTilePx * 4,
                        kAtlasTilePx * 4);
        }
    }
    if (loaded_pngs > 0) {
        std::printf("[atlas] loaded %d/%d block textures from textures/\n",
                    loaded_pngs, kAtlasTilesDim * kAtlasTilesDim - 1);
    }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kAtlasSizePx, kAtlasSizePx,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

}  // namespace gfx
