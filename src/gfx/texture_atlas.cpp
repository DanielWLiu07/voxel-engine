#include "gfx/texture_atlas.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace gfx {

namespace {

struct Rgb { std::uint8_t r, g, b; };

// xxhash-style integer hash, range [0, 1).
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

// Per-block tile painter. (x, y) are in-tile pixel coords, 0..15.
Rgb paint_tile(int block_id, int x, int y) {
    switch (block_id) {
    case 1: {  // Stone: gray base with darker speckles
        Rgb base{140, 140, 145};
        Rgb tone = jitter(base, x, y, 11, 0.12f);
        if (hash2(x + 7, y + 13) > 0.92f) tone = mix(tone, {90, 90, 95}, 0.6f);
        return tone;
    }
    case 2: {  // Dirt: brown base with pebble-like grain
        Rgb base{128, 86, 50};
        Rgb tone = jitter(base, x, y, 23, 0.18f);
        if (hash2(x, y) > 0.85f) tone = mix(tone, {72, 48, 28}, 0.5f);
        return tone;
    }
    case 3: {  // Grass top: green base, occasional darker tuft
        Rgb base{86, 158, 70};
        Rgb tone = jitter(base, x, y, 5, 0.14f);
        if (hash2(x * 3, y * 5) > 0.86f) tone = mix(tone, {50, 110, 42}, 0.7f);
        return tone;
    }
    case 4: {  // Sand: pale yellow, fine grain
        Rgb base{226, 206, 142};
        return jitter(base, x, y, 41, 0.08f);
    }
    case 5: {  // Wood: brown with horizontal grain bands
        Rgb base{108, 70, 32};
        // Concentric rings on cross-section + grain stripes on sides.
        float ring = std::sin((x + y) * 0.9f) * 0.5f + 0.5f;
        Rgb tone = mix(base, {78, 50, 22}, ring * 0.35f);
        return jitter(tone, x, y, 59, 0.08f);
    }
    case 6: {  // Leaves: green clusters with darker gaps
        Rgb base{56, 118, 50};
        Rgb tone = jitter(base, x, y, 71, 0.18f);
        if (hash2(x + 1, y + 2) > 0.65f) tone = mix(tone, {34, 78, 32}, 0.4f);
        // Occasional brighter highlights for depth.
        if (hash2(x * 7, y * 11) > 0.94f) tone = mix(tone, {120, 180, 90}, 0.5f);
        return tone;
    }
    case 7: {  // Snow: near-white with a faint blue cast and sparkle pixels
        Rgb base{242, 244, 250};
        Rgb tone = jitter(base, x, y, 89, 0.04f);
        if (hash2(x * 5, y * 3) > 0.97f) tone = mix(tone, {255, 255, 255}, 0.6f);
        return tone;
    }
    default:   // Air or unknown: magenta (debug)
        return {255, 0, 255};
    }
}

}  // namespace

GLuint generate_block_atlas() {
    std::array<std::uint8_t, kAtlasSizePx * kAtlasSizePx * 4> pixels{};

    for (int tile = 0; tile < kAtlasTilesDim * kAtlasTilesDim; ++tile) {
        const int tx = tile % kAtlasTilesDim;
        const int ty = tile / kAtlasTilesDim;
        for (int py = 0; py < kAtlasTilePx; ++py) {
            for (int px = 0; px < kAtlasTilePx; ++px) {
                Rgb c = paint_tile(tile, px, py);
                const int img_x = tx * kAtlasTilePx + px;
                const int img_y = ty * kAtlasTilePx + py;
                const int i = (img_y * kAtlasSizePx + img_x) * 4;
                pixels[i + 0] = c.r;
                pixels[i + 1] = c.g;
                pixels[i + 2] = c.b;
                pixels[i + 3] = 255;
            }
        }
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
