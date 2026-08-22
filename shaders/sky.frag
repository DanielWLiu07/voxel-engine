#version 410 core

// The whole sky is one fullscreen triangle and this shader. There is no
// skybox texture, no cloud mesh, no star billboards - clouds, stars, the
// moon and the sun are all evaluated from the view direction, so the sky
// costs zero bytes of VRAM and zero draw calls beyond this one.
//
// It is drawn AFTER the terrain with depth testing on, so these functions
// only run on the pixels the world did not already cover.

in  vec3 v_view_dir;
out vec4 frag_color;

uniform vec3  u_sky_top;      // zenith color
uniform vec3  u_sky_horizon;  // horizon haze
uniform vec3  u_sun_dir;      // points toward sun, normalized
uniform vec3  u_sun_color;
uniform vec3  u_moon_dir;     // points toward moon, normalized
uniform float u_star_fade;    // 0 by day, 1 at night
uniform float u_time;         // seconds, for cloud drift and twinkle
uniform mat3  u_star_rot;     // rotates the fixed stars onto the night sky

// ---------------------------------------------------------------- noise

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float hash31(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float vnoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);          // smoothstep interpolant
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// Five octaves is enough for a cloud edge that survives a 1440p still.
// `octaves` fades the fine ones out where the projection is compressed
// near the horizon, and the running normalization keeps the output in
// [0, 1] as they go, so the coverage threshold downstream does not have
// to move with the level of detail.
float fbm(vec2 p, float octaves) {
    float v = 0.0;
    float a = 0.5;
    float norm = 0.0;
    for (int i = 0; i < 5; ++i) {
        float w = clamp(octaves - float(i), 0.0, 1.0);
        v    += a * w * vnoise(p);
        norm += a * w;
        p = p * 2.03 + 19.7;              // rotate-ish to hide axis alignment
        a *= 0.5;
    }
    return v / max(norm, 1e-4);
}

// ---------------------------------------------------------------- stars

// Stars live in a 3D cell grid that the unit sphere passes through. One
// candidate star per cell, kept clear of the cell walls so the containing
// cell is the only one that ever has to be sampled - 4 hashes, not 108.
float starfield(vec3 d) {
    vec3 p  = d * 210.0;
    vec3 ip = floor(p);
    vec3 fp = fract(p);

    float h = hash31(ip);
    if (h < 0.978) return 0.0;            // most cells are empty

    vec3 off = vec3(hash31(ip + 11.0), hash31(ip + 27.0), hash31(ip + 43.0));
    off = 0.25 + 0.5 * off;               // never within 0.25 of a wall
    float dist = length(fp - off);

    float mag     = fract(h * 91.7);
    float twinkle = 0.78 + 0.22 * sin(u_time * 1.7 + mag * 41.0);
    return smoothstep(0.22, 0.0, dist) * (0.3 + 0.7 * mag * mag) * twinkle;
}

void main() {
    vec3 dir = normalize(v_view_dir);

    // Gradient by elevation: 0 at the horizon, 1 at the zenith.
    float elev = clamp(dir.y * 1.2 + 0.1, 0.0, 1.0);
    elev = pow(elev, 0.55);
    vec3 sky = mix(u_sky_horizon, u_sky_top, elev);

    // ------------------------------------------------------------ night
    if (u_star_fade > 0.001) {
        vec3 sdir = u_star_rot * dir;

        // A faint band of unresolved stars across one great circle. Low
        // amplitude on purpose: it should read as depth, not as a feature.
        float band = exp(-abs(sdir.z) * 5.0);
        float haze = fbm(sdir.xy * 3.0 + 8.0, 5.0) * band * 0.05;

        float stars = starfield(sdir);
        sky += (vec3(0.72, 0.78, 1.0) * stars * 1.6 +
                vec3(0.55, 0.60, 0.85) * haze) * u_star_fade;

        // Moon: a disc with a crescent bitten out of it by a second disc
        // offset along the moon's own tangent, plus a soft halo.
        float mc = max(dot(dir, u_moon_dir), 0.0);
        vec3  mt = normalize(cross(u_moon_dir, vec3(0.0, 1.0, 0.0)) +
                             vec3(1e-4, 0.0, 0.0));
        // The bite has to be offset by a real fraction of the disc's own
        // angular radius (~0.045 rad here) or it lands concentric and
        // hollows the moon into a ring instead of cutting a crescent.
        vec3  shifted = normalize(u_moon_dir + mt * 0.030);
        float moon_disc = smoothstep(0.99900, 0.99930, mc);
        float bite      = smoothstep(0.99900, 0.99930, dot(dir, shifted));
        float halo      = pow(mc, 2500.0) * 0.30 + pow(mc, 260.0) * 0.05;
        sky += vec3(0.98, 0.97, 0.90) *
               (moon_disc * (1.0 - bite * 0.93) * 1.9 + halo) * u_star_fade;
    }

    // -------------------------------------------------------------- sun
    float sun_cos  = max(dot(dir, normalize(u_sun_dir)), 0.0);
    float glow     = pow(sun_cos, 32.0) * 0.35;
    float sun_disc = smoothstep(0.9985, 0.999, sun_cos);
    sky += u_sun_color * (glow + sun_disc * 4.0);

    // ----------------------------------------------------------- clouds
    // The deck is a shell overhead, and the view ray is intersected with
    // it. A flat plane would be simpler - divide xz by dir.y - but that
    // projection runs to infinity as the ray levels out, which is exactly
    // where a landscape shot puts most of its sky. The shell bottoms out
    // at a finite distance instead, so the deck reaches the horizon and
    // compresses there the way a real one does rather than aliasing away.
    if (dir.y > -0.01) {
        const float kShellR = 60.0;       // shell radius, in deck heights
        float b       = kShellR * max(dir.y, 0.0);
        float shell_t = -b + sqrt(b * b + 2.0 * kShellR + 1.0);

        vec2 cp = dir.xz * shell_t * 0.85 + vec2(u_time * 0.0035, 0.0);
        // Detail is dropped in step with that compression: shell_t rises
        // to ~11 at the horizon, so the octaves whose period has fallen
        // below a pixel are faded out instead of sparkling.
        float oct = clamp(5.0 - log2(max(shell_t, 1.0)), 2.0, 5.0);

        float n = fbm(cp, oct);
        // Coverage threshold, then a second sample displaced toward the
        // sun. Where the density is falling off in the sun's direction the
        // cloud is facing the light, so the difference is a free normal.
        float cover = smoothstep(0.44, 0.68, n);
        float ns    = fbm(cp + normalize(u_sun_dir.xz + vec2(1e-3)) * 0.30, oct);
        float lit   = clamp((n - ns) * 3.4 + 0.5, 0.0, 1.0);

        // Still fade the last couple of degrees: the ground meets the sky
        // there and a hard cloud edge against the fog reads as a seam.
        cover *= smoothstep(-0.01, 0.05, dir.y);

        // Cloud color is derived from the same sun and sky the terrain is
        // lit by, never from a constant white: that is what makes the deck
        // go amber at sunset and slate at midnight instead of staying a
        // bright cutout pasted over a dark sky. Moonlight is added back
        // separately, or the night deck would vanish entirely.
        vec3 moonlit    = vec3(0.10, 0.11, 0.15) * u_star_fade;
        vec3 cloud_lit  = u_sun_color * 0.78 + u_sky_top * 0.45 + moonlit;
        vec3 cloud_dark = cloud_lit * 0.34 + u_sky_horizon * 0.30;
        vec3 cloud      = mix(cloud_dark, cloud_lit, lit);

        // Silver lining: thin edges scatter forward toward the sun.
        float rim = smoothstep(0.55, 0.50, n) * cover;
        cloud += u_sun_color * rim * pow(max(dot(dir, u_sun_dir), 0.0), 6.0) * 0.9;

        sky = mix(sky, cloud, cover * 0.92);
    }

    frag_color = vec4(sky, 1.0);
}
