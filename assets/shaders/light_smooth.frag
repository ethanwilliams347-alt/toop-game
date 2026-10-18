#version 330 core
// The light pass, step 3 of 3: one 1-2-1 sweep of LightField's smoothing
// (src/render/light.cpp, the smooth block), along u_step. Each neighbour is
// weighted by how open it is, so a wall stays a wall, and the emission is
// re-imposed as a floor so a source never dims itself.
uniform sampler2D u_front;
uniform sampler2D u_emit;
uniform ivec2 u_step;  // (1, 0) horizontal, (0, 1) vertical
uniform float u_min_visible;
uniform float u_transmit_clear;
uniform float u_transmit_solid;
uniform float u_centre;

out vec4 o_color;

float openness(ivec2 j) {
    float k = exp(texelFetch(u_emit, j, 0).a);
    return clamp((k - u_transmit_solid) / (u_transmit_clear - u_transmit_solid), 0.0, 1.0);
}

void main() {
    ivec2 i = ivec2(gl_FragCoord.xy);
    ivec2 n = textureSize(u_front, 0);
    float weight = u_centre;
    vec3 acc = texelFetch(u_front, i, 0).rgb * u_centre;
    ivec2 lo = i - u_step, hi = i + u_step;
    if (lo.x >= 0 && lo.y >= 0) {
        float w = openness(lo);
        acc += texelFetch(u_front, lo, 0).rgb * w;
        weight += w;
    }
    if (hi.x < n.x && hi.y < n.y) {
        float w = openness(hi);
        acc += texelFetch(u_front, hi, 0).rgb * w;
        weight += w;
    }
    vec3 v = max(acc / weight, texelFetch(u_emit, i, 0).rgb);
    v *= step(vec3(u_min_visible), v);
    o_color = vec4(v, 1.0);
}
