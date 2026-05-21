#version 410 core

in  vec2 v_uv;
out vec4 frag_color;

uniform sampler2D u_source;
uniform int       u_horizontal;

// 5-tap Gaussian (sigma ~1.6 in texel units). Weights sum to 1.
const float kWeights[5] = float[](0.227027, 0.194594, 0.121622, 0.054054, 0.016216);

void main() {
    vec2 texel = 1.0 / vec2(textureSize(u_source, 0));
    vec3 acc = texture(u_source, v_uv).rgb * kWeights[0];
    if (u_horizontal == 1) {
        for (int i = 1; i < 5; ++i) {
            vec2 off = vec2(texel.x * float(i), 0.0);
            acc += texture(u_source, v_uv + off).rgb * kWeights[i];
            acc += texture(u_source, v_uv - off).rgb * kWeights[i];
        }
    } else {
        for (int i = 1; i < 5; ++i) {
            vec2 off = vec2(0.0, texel.y * float(i));
            acc += texture(u_source, v_uv + off).rgb * kWeights[i];
            acc += texture(u_source, v_uv - off).rgb * kWeights[i];
        }
    }
    frag_color = vec4(acc, 1.0);
}
