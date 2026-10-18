#version 330 core
// The world's cells (src/gpu/gl_canvas.cpp, GlCanvas::blit_cells), drawn with
// sprite.vert over the same quad a plain blit of the cell texture would use.
//
// Two textures, one texel per cell, the same window of the grid:
//   u_tex    the colour texture (Grid::get_pixels, ARGB8888), as today;
//   u_cells  the material pack (render/cell_pack.h), RGBA8UI. The bit layout is
//            that header's; this file mirrors it and must change with it.
//
// u_mode picks what is drawn:
//   0  legacy: the colour texture and nothing else, so the frame equals a plain
//      blit to the bit (v_color is opaque white in a blit, and x * 1.0 is x).
//      Every later look starts from here and is measured against it.
//   1  material view (F10, a debug aid): each cell flat-coloured by its type,
//      with the flags shown -- hidden cells magenta-tinted, occluders brighter,
//      flowing liquid tinted by direction. Says the pack arrived, and where.
//   2  raw: the pack's bytes as the colour, (r, g, b, a) -> red, green, blue,
//      alpha, unblended. gl_frame_test reads it back to check the upload and
//      the texel order against CellPack::texels() byte for byte.
//
// The second output is the emissive attachment of a lit frame (the multiply
// light model, src/gpu/pass_graph.h): the light a cell gives off itself, linear,
// u_emissive[type] of its own colour (LightField::gpu_constants: a flame all of
// its colour, everything else none). Its alpha is the colour's, so a part-full
// cell blends its glow as it blends its paint. Outside a lit frame only
// attachment 0 is drawn to and it goes nowhere, so the legacy output above is
// still exactly a blit.
in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_tex;
uniform usampler2D u_cells;
uniform int u_mode;
uniform float u_emissive[16];  // by ElementType

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_emissive;

// render/cell_pack.h: A's bits.
const uint FLAG_HIDDEN = 1u;
const uint FLAG_OCCLUDER = 2u;
const uint FLAG_FLOW_LEFT = 4u;
const int FLOW_MAG_SHIFT = 3;

// The pack covers the visible window, which can be smaller than the colour
// texture (the grid narrower than the view); past it is PAD_TEXEL, an Empty cell.
uvec4 cell_at(ivec2 p) {
    ivec2 size = textureSize(u_cells, 0);
    if (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y) return uvec4(0u, 0u, 20u, 0u);
    return texelFetch(u_cells, p, 0);
}

// One flat colour per ElementType, in enum order (physics/element.h).
vec3 type_colour(uint t) {
    if (t == 0u) return vec3(0.0);             // Empty
    if (t == 1u) return vec3(0.85, 0.75, 0.3); // Sand
    if (t == 2u) return vec3(0.15, 0.4, 0.9);  // Water
    if (t == 3u) return vec3(0.45, 0.45, 0.5); // Wall
    if (t == 4u) return vec3(0.55, 0.35, 0.15);// Wood
    if (t == 5u) return vec3(0.25, 0.1, 0.3);  // Oil
    if (t == 6u) return vec3(0.8, 0.8, 0.85);  // Steam
    if (t == 7u) return vec3(1.0, 0.45, 0.05); // Fire
    if (t == 8u) return vec3(0.15, 0.12, 0.1); // Charred
    return vec3(1.0, 0.0, 1.0);                // a type this file does not know yet
}

vec3 to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    vec4 colour = texture(u_tex, v_uv) * v_color;
    ivec2 p = ivec2(floor(v_uv * vec2(textureSize(u_tex, 0))));
    uvec4 c = cell_at(p);
    // A hidden cell's type is the fill around a loose body, not the body; the
    // body's sprite is what is drawn there, and it gives off nothing.
    float glow = (c.a & FLAG_HIDDEN) != 0u ? 0.0 : u_emissive[int(min(c.r, 15u))];
    o_emissive = vec4(to_linear(colour.rgb) * glow, colour.a);
    if (u_mode == 0) {
        o_color = colour;
        return;
    }
    if (u_mode == 2) {
        o_color = vec4(c) / 255.0;
        return;
    }
    vec3 rgb = type_colour(c.r);
    if ((c.a & FLAG_OCCLUDER) != 0u) rgb *= 1.15;
    int flow = int(c.a >> uint(FLOW_MAG_SHIFT)) * (((c.a & FLAG_FLOW_LEFT) != 0u) ? -1 : 1);
    if (flow != 0) rgb = mix(rgb, flow < 0 ? vec3(0.0, 1.0, 0.6) : vec3(1.0, 1.0, 0.0),
                             min(abs(float(flow)) / 31.0, 1.0) * 0.6);
    if ((c.a & FLAG_HIDDEN) != 0u) rgb = mix(rgb, vec3(1.0, 0.0, 1.0), 0.5);
    o_color = vec4(rgb, (c.r == 0u && (c.a & FLAG_HIDDEN) == 0u) ? 0.0 : 1.0);
}
