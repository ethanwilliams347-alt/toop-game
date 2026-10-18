#version 330 core
// The light pass, step 1 of 3: one texel per LightField block, from the cell
// pack. A port of the gather in LightField::update (src/render/light.cpp) --
// read that for why each line is what it is; the numbers are its own
// (LightField::gpu_constants). Out: rgb the block's emission, a its log
// transmission per block crossed.
//
// Texel (x, y) here is block (x, y) with y counting down the screen, the cell
// pack's own orientation; none of the light passes flip.
uniform usampler2D u_cells;
uniform int u_block;
uniform float u_log_transmit[16];  // by ElementType
uniform float u_log_solid;         // a loose body's hidden cell
uniform float u_glow_threshold;
uniform float u_max_emission;
uniform float u_coverage_floor;

out vec4 o_color;

void main() {
    ivec2 b = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(u_cells, 0);
    float hottest = 0.0;
    float log_sum = 0.0;
    int glowing = 0;
    for (int cy = 0; cy < u_block; ++cy) {
        for (int cx = 0; cx < u_block; ++cx) {
            ivec2 p = b * u_block + ivec2(cx, cy);
            // Past the pack is an Empty cell at ambient, as render/cell_pack.h pads.
            uvec4 c = uvec4(0u, 0u, 20u, 0u);
            if (p.x < size.x && p.y < size.y) c = texelFetch(u_cells, p, 0);
            float t = float(c.b);
            hottest = max(hottest, t);
            log_sum += (c.a & 1u) != 0u ? u_log_solid : u_log_transmit[int(min(c.r, 15u))];
            if (t > u_glow_threshold) ++glowing;
        }
    }
    vec3 e = vec3(0.0);
    if (hottest > u_glow_threshold) {
        float t = (hottest - u_glow_threshold) / (255.0 - u_glow_threshold);
        float coverage = float(glowing) / float(u_block * u_block);
        float s = u_max_emission * t * sqrt(t) *
                  (u_coverage_floor + (1.0 - u_coverage_floor) * coverage);
        e = vec3(s, s * (0.34 + 0.46 * t), s * (0.08 + 0.26 * t * t));
    }
    o_color = vec4(e, log_sum / float(u_block));
}
