#version 330 core
// The last pass before the UI (src/gpu/pipeline.cpp): the linear HDR frame back
// to the display's encoding (sRGB), clamped. For now that is all; R5 puts the
// tonemap, bloom, the grade LUT, fog, vignette and grain here.
uniform sampler2D u_hdr;

out vec4 o_color;

vec3 to_display(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

void main() {
    vec4 h = texelFetch(u_hdr, ivec2(gl_FragCoord.xy), 0);
    o_color = vec4(to_display(h.rgb), h.a);
}
