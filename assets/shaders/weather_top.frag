#version 330 core
// Weather's stop (src/gpu/pipeline.cpp): one texel per column of the cell pack,
// holding the first row from the top that rain cannot fall through -- a solid
// or powder (the pack's OCCLUDER bit), a loose body's hidden cell, or a liquid's
// surface (Water 2, Oil 5 -- ElementType's values; gl_frame_test holds them).
// u_none if the column is open all the way down. Rebuilt every lit frame from
// the pack the light passes read, so it is never a frame behind the world.
uniform usampler2D u_cells;
uniform float u_none;

out vec4 o_color;

void main() {
    ivec2 n = textureSize(u_cells, 0);
    int x = int(gl_FragCoord.x);
    float top = u_none;
    for (int y = 0; y < n.y; ++y) {
        uvec4 c = texelFetch(u_cells, ivec2(x, y), 0);
        if ((c.a & 3u) != 0u || c.r == 2u || c.r == 5u) {
            top = float(y);
            break;
        }
    }
    o_color = vec4(top, 0.0, 0.0, 1.0);
}
