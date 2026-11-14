#version 330 core
// One step down the bloom chain (src/gpu/pipeline.cpp): the source at twice
// this target's size, averaged over a 4x4 texel box with four bilinear taps.
// The first step reads the HDR frame and keeps only what is over the mood's
// threshold, with a soft knee so a surface just over it does not pop in.
uniform sampler2D u_src;
uniform vec2 u_src_texel;  // 1 / source size
uniform vec2 u_dst_size;
uniform int u_first;
uniform float u_threshold;

out vec4 o_color;

vec3 bright(vec3 c) {
    c = min(c, vec3(32.0));  // a stray inf or huge value would bloom the screen
    if (u_first == 0) return c;
    float l = max(max(c.r, c.g), c.b);
    float knee = 0.5 * u_threshold + 1e-4;
    float soft = clamp(l - u_threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float w = max(soft, l - u_threshold) / max(l, 1e-4);
    return c * w;
}

void main() {
    vec2 uv = gl_FragCoord.xy / u_dst_size;
    vec2 o = u_src_texel;
    vec3 s = bright(texture(u_src, uv + vec2(-o.x, -o.y)).rgb) +
             bright(texture(u_src, uv + vec2(o.x, -o.y)).rgb) +
             bright(texture(u_src, uv + vec2(-o.x, o.y)).rgb) +
             bright(texture(u_src, uv + vec2(o.x, o.y)).rgb);
    o_color = vec4(0.25 * s, 1.0);
}
