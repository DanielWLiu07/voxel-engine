#include "world/noise4d.h"

#include <cmath>

namespace world {

namespace {

// The 32 four-dimensional gradients: every permutation of (0, ±1, ±1, ±1).
// Four choices of which axis is zeroed times eight sign combinations. This
// is Perlin's improved-noise gradient set extended to 4D - the vectors all
// have the same length, so no direction is favoured, which is what keeps
// the field free of axis-aligned artifacts.
constexpr signed char kGrad4[32][4] = {
    { 0,  1,  1,  1}, { 0,  1,  1, -1}, { 0,  1, -1,  1}, { 0,  1, -1, -1},
    { 0, -1,  1,  1}, { 0, -1,  1, -1}, { 0, -1, -1,  1}, { 0, -1, -1, -1},
    { 1,  0,  1,  1}, { 1,  0,  1, -1}, { 1,  0, -1,  1}, { 1,  0, -1, -1},
    {-1,  0,  1,  1}, {-1,  0,  1, -1}, {-1,  0, -1,  1}, {-1,  0, -1, -1},
    { 1,  1,  0,  1}, { 1,  1,  0, -1}, { 1, -1,  0,  1}, { 1, -1,  0, -1},
    {-1,  1,  0,  1}, {-1,  1,  0, -1}, {-1, -1,  0,  1}, {-1, -1,  0, -1},
    { 1,  1,  1,  0}, { 1,  1, -1,  0}, { 1, -1,  1,  0}, { 1, -1, -1,  0},
    {-1,  1,  1,  0}, {-1,  1, -1,  0}, {-1, -1,  1,  0}, {-1, -1, -1,  0},
};

// Every gradient must have exactly three non-zero components, each ±1, and
// no two rows may be equal. That is what makes the set equal-length and
// evenly spread, which is what keeps the field free of directional bias.
//
// Checked at compile time because it is not checkable at runtime in any
// useful way: fault injection showed that zeroing one of the 32 rows moves
// the field's standard deviation from 0.23719 to 0.23337, a 1.6% shift.
// Gating on that would need a bound so tight it would break on legitimate
// tuning, so the structural property is asserted structurally instead.
constexpr bool grad4_table_is_well_formed() {
    for (const auto& g : kGrad4) {
        int nonzero = 0;
        for (const signed char c : g) {
            if (c < -1 || c > 1) return false;
            if (c != 0) ++nonzero;
        }
        if (nonzero != 3) return false;
    }
    for (int i = 0; i < 32; ++i) {
        for (int j = i + 1; j < 32; ++j) {
            bool identical = true;
            for (int k = 0; k < 4; ++k) {
                if (kGrad4[i][k] != kGrad4[j][k]) identical = false;
            }
            if (identical) return false;
        }
    }
    return true;
}
static_assert(grad4_table_is_well_formed(),
              "every 4D gradient must be a distinct permutation of "
              "(0, +/-1, +/-1, +/-1); a zero, duplicated, or over-long row "
              "biases the field in a direction and is nearly invisible in "
              "the output statistics");

// Integer hash over a lattice corner. Same shape as terrain_gen's hash2d
// (multiply by odd constants, xor-shift, multiply again) widened to four
// axes. The seed is mixed in rather than appended so two nearby seeds do
// not produce two nearby fields.
std::uint32_t hash4(std::int32_t x, std::int32_t y, std::int32_t z,
                    std::int32_t w, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x9E3779B1u
                    + static_cast<std::uint32_t>(y) * 0x85EBCA77u
                    + static_cast<std::uint32_t>(z) * 0xC2B2AE3Du
                    + static_cast<std::uint32_t>(w) * 0x27D4EB2Fu
                    + seed * 0x165667B1u;
    h ^= h >> 15;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h;
}

// Perlin's quintic fade, 6t^5 - 15t^4 + 10t^3. Chosen over the older cubic
// because its first AND second derivatives vanish at 0 and 1, so the field
// is C2-continuous across cell boundaries. In a voxel world a C1 seam
// shows up as a faint grid of ridges at every 1-unit interval once the
// terrain is lit; this is what prevents it.
constexpr float fade(float t) {
    return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

constexpr float lerp(float a, float b, float t) { return a + t * (b - a); }

// Dot product of the corner's gradient with the distance vector to it.
float grad_dot(std::uint32_t h, float dx, float dy, float dz, float dw) {
    const signed char* g = kGrad4[h & 31u];
    return static_cast<float>(g[0]) * dx + static_cast<float>(g[1]) * dy
         + static_cast<float>(g[2]) * dz + static_cast<float>(g[3]) * dw;
}

// Scale factor bringing the raw field into [-1, 1], with margin.
//
// Every figure here is one the committed `noise4d_range` prints. That
// matters more than usual, because the previous version of this comment
// quoted a raw peak of 1.25719 that no committed program produced - it
// came from a scratch sweep with a different seed family - inside the
// very fix that was correcting a figure fitted too tightly. The rule is
// never to quote a number whose input is not in the repo, and it has now
// been broken here twice.
//
// `noise4d_range` sweeps 200,000,000 samples across 400 seeds drawn from
// two families, plus 0 and UINT32_MAX, and prints:
//
//     NOISE4D_RANGE samples=200000000 peak=0.9190 mean=0.00001 stddev=0.2196
//     normalized peak 0.9190 (must stay <= 1.0), implying a raw peak of 1.2254
//
// 0.75 is deliberately NOT 1/1.2254 = 0.816. Different sweeps find
// different maxima - changing the seed family or the samples-per-seed
// moves the observed peak - and widening any sweep can only push it up.
// A constant fitted to the last sweep's maximum is therefore guaranteed
// to be wrong eventually, which is exactly what happened: 0.81 was fitted
// to a raw peak of 1.2244 from 20M samples over 8 seeds, and a wider
// sweep then produced a normalized sample above 1.0. The bound was never
// a bound; the test had simply not met a violating sample.
//
// 0.75 leaves the printed peak at 0.919, roughly 8% of headroom for
// extremes no sweep has reached. The unit test asserts the [-1, 1] bound,
// and it is now a bound rather than a coin flip on the next sample.
constexpr float kNormalize = 0.75f;

}  // namespace

float Noise4D::sample(float x, float y, float z, float w) const {
    const float fx = std::floor(x), fy = std::floor(y);
    const float fz = std::floor(z), fw = std::floor(w);
    const auto ix = static_cast<std::int32_t>(fx);
    const auto iy = static_cast<std::int32_t>(fy);
    const auto iz = static_cast<std::int32_t>(fz);
    const auto iw = static_cast<std::int32_t>(fw);

    // Position within the cell, and the faded weights for interpolation.
    const float dx = x - fx, dy = y - fy, dz = z - fz, dw = w - fw;
    const float u = fade(dx), v = fade(dy), s = fade(dz), t = fade(dw);

    // 16 corners of the hypercube. Indexed so bit 0 is +x, 1 is +y, 2 is
    // +z, 3 is +w; the distance vector to each corner is the offset from
    // the sample to that corner, hence the (d - 1) on the far side.
    float corner[16];
    for (int c = 0; c < 16; ++c) {
        const int ox = (c >> 0) & 1, oy = (c >> 1) & 1;
        const int oz = (c >> 2) & 1, ow = (c >> 3) & 1;
        corner[c] = grad_dot(
            hash4(ix + ox, iy + oy, iz + oz, iw + ow, seed_),
            dx - static_cast<float>(ox), dy - static_cast<float>(oy),
            dz - static_cast<float>(oz), dw - static_cast<float>(ow));
    }

    // Quadrilinear interpolation: collapse x, then y, then z, then w.
    float a[8];
    for (int i = 0; i < 8; ++i) a[i] = lerp(corner[i * 2], corner[i * 2 + 1], u);
    float b[4];
    for (int i = 0; i < 4; ++i) b[i] = lerp(a[i * 2], a[i * 2 + 1], v);
    float c2[2];
    for (int i = 0; i < 2; ++i) c2[i] = lerp(b[i * 2], b[i * 2 + 1], s);
    return lerp(c2[0], c2[1], t) * kNormalize;
}

float Noise4D::fbm(float x, float y, float z, float w, int octaves,
                   float frequency, float lacunarity, float gain) const {
    float sum = 0.0f;
    float amplitude = 1.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += amplitude * sample(x * frequency, y * frequency,
                                  z * frequency, w * frequency);
        norm += amplitude;
        frequency *= lacunarity;
        amplitude *= gain;
    }
    // Dividing by the amplitude sum, not by a hardcoded 2 - 1/2^n, so a
    // caller that passes an unusual gain still gets the single-octave
    // range back rather than a field that quietly shrinks.
    return norm > 0.0f ? sum / norm : 0.0f;
}

}  // namespace world
