#version 330 core
// The sun's visibility, one texel per cell of the cell pack (src/gpu/pipeline.cpp).
// Marches from the cell toward the sun -- against u_dir, the way the light
// travels, +y down -- one cell a step along the major axis, multiplying what
// each cell crossed passes: air all of it, a liquid or gas its LightField
// per-cell figure, a solid or powder (or a loose body's hidden cell) little.
// The cell itself is not crossed, so the top of the ground is in the sun and
// what is under it is not. Leaving the pack ends the march in the open: only
// the visible window is known.
uniform usampler2D u_cells;
uniform vec2 u_dir;
uniform int u_shadow;  // 0: everything in full sun
uniform int u_march;   // Pipeline::SUN_MARCH_CELLS
uniform float u_sun_transmit[16];  // by ElementType
uniform float u_solid;

out vec4 o_color;

void main() {
    if (u_shadow == 0) {
        o_color = vec4(1.0);
        return;
    }
    ivec2 n = textureSize(u_cells, 0);
    vec2 toward = -u_dir / max(max(abs(u_dir.x), abs(u_dir.y)), 1e-4);
    vec2 p = floor(gl_FragCoord.xy) + 0.5;
    float t = 1.0;
    for (int k = 1; k <= u_march; ++k) {
        ivec2 q = ivec2(floor(p + toward * float(k)));
        if (q.x < 0 || q.y < 0 || q.x >= n.x || q.y >= n.y) break;
        uvec4 c = texelFetch(u_cells, q, 0);
        t *= (c.a & 1u) != 0u ? u_solid : u_sun_transmit[int(min(c.r, 15u))];
        if (t < 0.004) {
            t = 0.0;
            break;
        }
    }
    o_color = vec4(t, 0.0, 0.0, 1.0);
}
