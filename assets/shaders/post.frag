#version 330 core
// The last pass before the UI (src/gpu/pipeline.cpp): the linear HDR frame back
// to the display's encoding (sRGB). With the mood's post off -- no bloom, grade,
// vignette or grain -- that is all it does, clamped, so a neutral mood still
// resolves to its albedo (gl_frame_test). With any of them on (R5), in order:
//   bloom      the half-resolution bloom chain (bloom_down/up.frag), added in
//              linear light, so a glow is light and not a wash of paint;
//   tonemap    a shoulder per channel: identity up to SHOULDER, rolling off
//              smoothly to 1, so a lamp's hot core whitens the way film does
//              instead of clipping to a flat yellow, and everything at an
//              ordinary paint value (most of the frame) is untouched. Per
//              channel rather than on the brightest one scaled through the
//              others, because that scaling dims a channel when another one
//              gains -- bloom would darken pixels (gl_frame_test);
//   grade      the mood's LUT strip (tools/make_lut.py), on display-encoded
//              colour, the space its grades are written in;
//   vignette   darkening toward the corners, on display colour;
//   grain      deterministic per simulation step, never the wall clock, so a
//              replay or a paused frame is the same image.
uniform sampler2D u_hdr;
uniform sampler2D u_bloom;  // linear filtered, half resolution
uniform sampler2D u_lut;    // 1024 x 32, see make_lut.py's header
uniform int u_post;         // any of the four below on
uniform float u_bloom_strength;
uniform int u_lut_on;
uniform float u_vignette;
uniform float u_grain;
uniform float u_frame;  // the simulation step, wrapped
uniform vec2 u_size;    // target pixels

out vec4 o_color;

const float SHOULDER = 0.7;

vec3 to_display(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 tonemap(vec3 c) {
    float k = 1.0 - SHOULDER;
    vec3 rolled = SHOULDER + k * (1.0 - exp(-(c - SHOULDER) / k));
    return mix(c, rolled, step(vec3(SHOULDER), c));
}

// Texel centres within a slice, and the two blue slices blended by hand: a
// bilinear tap across the seam between two slices would mix unrelated colours.
vec3 grade(vec3 c) {
    c = clamp(c, 0.0, 1.0) * 31.0;
    float b0 = floor(c.b);
    float b1 = min(b0 + 1.0, 31.0);
    vec2 inner = vec2(c.r + 0.5, c.g + 0.5);
    vec3 s0 = texture(u_lut, vec2((b0 * 32.0 + inner.x) / 1024.0, inner.y / 32.0)).rgb;
    vec3 s1 = texture(u_lut, vec2((b1 * 32.0 + inner.x) / 1024.0, inner.y / 32.0)).rgb;
    return mix(s0, s1, c.b - b0);
}

float hash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}

void main() {
    vec4 h = texelFetch(u_hdr, ivec2(gl_FragCoord.xy), 0);
    if (u_post == 0) {
        o_color = vec4(to_display(h.rgb), h.a);
        return;
    }
    vec2 uv = gl_FragCoord.xy / u_size;
    vec3 c = h.rgb;
    if (u_bloom_strength > 0.0) c += u_bloom_strength * texture(u_bloom, uv).rgb;
    vec3 d = to_display(tonemap(c));
    if (u_lut_on != 0) d = grade(d);
    if (u_vignette > 0.0) {
        // Elliptical, by the screen's own aspect, so an ultrawide darkens at its
        // sides and not in a circle in the middle.
        vec2 q = (uv - 0.5) * 2.0;
        float r = length(q * vec2(1.0, 0.8));
        d *= 1.0 - u_vignette * smoothstep(0.55, 1.45, r);
    }
    if (u_grain > 0.0) {
        // Strongest in the mid-tones, as film's is: no speckle in black or white.
        float n = hash(gl_FragCoord.xy + vec2(u_frame * 17.0, u_frame * 31.0)) - 0.5;
        float l = dot(d, vec3(0.299, 0.587, 0.114));
        d += u_grain * n * (0.35 + 2.6 * l * (1.0 - l));
    }
    o_color = vec4(clamp(d, 0.0, 1.0), h.a);
}
