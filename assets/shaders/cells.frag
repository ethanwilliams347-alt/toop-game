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
//
// u_look (R4a/R4b) is the lit look, on only inside a lit frame in mode 0 with a
// multiply mood (GlCanvas::blit_cells decides; lit::CellLook carries the
// numbers). Off, mode 0 is the blit above, untouched. On, it adds, each from
// the mood's `material` line for the cell's type:
//   - tint, a multiply on the albedo;
//   - texture: a world-anchored tile, one texel per cell, that moves luminance
//     only -- albedo * (1 + strength * (tile - tile mean)). Static materials
//     only; a powder keeps its own per-cell jitter, since a tile anchored to the
//     world would shimmer under falling grains;
//   - rim: an exposed edge of a solid brightened, more where it faces the sun.
//     The normal is faked from which of the eight neighbours are open (Rain
//     World's trick), so it needs nothing but the pack;
//   - water: see liquid() below; oil a dark sheen; steam soft, thin puffs.
// Every one is a function of the cell's own texel and its neighbours', so a
// cell is still one flat colour on screen -- the pixel art stays crisp -- except
// where water shows the backdrop through it, which is the backdrop's own pixels.
in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_tex;
uniform usampler2D u_cells;
uniform int u_mode;
uniform float u_emissive[16];  // by ElementType

// The look (u_look = 1); all by ElementType. See lit::CellLook.
uniform int u_look;
uniform vec3 u_tint[16];
uniform float u_strength[16];
uniform float u_rim[16];
uniform float u_reflect[16];
uniform float u_spec[16];
uniform float u_depth[16];
uniform float u_tile_mean[16];  // < 0: no tile for this type
uniform sampler2D u_tiles;      // 64x64 per type, side by side, grey
uniform sampler2D u_behind;     // the albedo before the world (pass_graph "behind")
uniform vec2 u_sun_dir;         // the way the sun's light travels, +y down
uniform ivec2 u_origin;         // world cell of texel (0, 0)
uniform int u_step;             // simulation steps: time, never the wall clock

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

const uint T_EMPTY = 0u, T_WATER = 2u, T_OIL = 5u, T_STEAM = 6u, T_FIRE = 7u;
const int TILE = 64;

// The type's tile at a world cell; wraps every 64 cells.
float tile(uint t, ivec2 w) {
    ivec2 q = ivec2(w.x & (TILE - 1), w.y & (TILE - 1));
    return texelFetch(u_tiles, ivec2(int(t) * TILE + q.x, q.y), 0).r;
}

// Open to the air for a rim: nothing there, or only gas or flame. Water is not
// open -- an edge under a pond does not catch the sky -- and neither is a loose
// body's cell, which is solid whatever fill it shows.
bool open_air(uvec4 q) {
    if ((q.a & FLAG_HIDDEN) != 0u) return false;
    return q.r == T_EMPTY || q.r == T_STEAM || q.r == T_FIRE;
}

// The exposed-edge factor of a solid cell: 0 inside, up to 1 on an edge facing
// the sun. Open neighbours sum to a fake outward normal; an edge facing away
// from the sun keeps a flat third so a silhouette still reads.
float rim_of(ivec2 p) {
    vec2 n = vec2(0.0);
    int open = 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            if (open_air(cell_at(p + ivec2(dx, dy)))) {
                n += vec2(float(dx), float(dy));
                ++open;
            }
        }
    if (open == 0) return 0.0;
    float facing = length(n) > 0.0 ? dot(normalize(n), -u_sun_dir) : 0.0;
    float r = 0.3 + 0.7 * max(facing, 0.0);
    // The terrain albedos still bake a rim into every cell open straight above
    // (tools/pixel_art.py apply_rim_light), so a full rim there draws it twice:
    // a hot doubled line along every lip. Until those albedos are regenerated
    // without it (the plan's follow-up commit), such a cell takes 0.4 of the
    // rim; side and underside edges, which nothing bakes, take all of it. A
    // player-built wall has no baked rim and is a little under-lit on top as a
    // result -- the smaller wrong.
    if (open_air(cell_at(p - ivec2(0, 1)))) r *= 0.4;
    return r;
}

// Water and oil: the colour this fragment shows. Water is composited over the
// backdrop here (the "behind" copy) rather than by blending, so its reflection
// can replace what is seen through it.
//   depth    cells below the surface, by scanning up the column (16 at most:
//            past that it is simply deep);
//   through  the backdrop behind it, and under an open surface its mirror image
//            about the surface row, wobbled sideways by whole screen pixels per
//            world row (Cast n Chill);
//   body     the water's own colour, darkened with depth, plus caustics: the
//            type's tile twice, drifting two ways in whole cells, anchored to
//            the world, brightest near the surface;
//   surface  the top cell lit as a highlight line, whiter where flow is fast.
// Oil keeps its opacity and gets a faint thin-film sheen instead.
vec3 liquid(vec3 albedo, ivec2 p, uvec4 c, vec2 f, vec2 px_per_cell) {
    uint t = c.r;
    int k = 0;
    bool found = false;
    uvec4 above = uvec4(0u);
    for (int i = 1; i <= 16; ++i) {
        uvec4 q = cell_at(p - ivec2(0, i));
        if (q.r != t || (q.a & FLAG_HIDDEN) != 0u) {
            found = true;
            above = q;
            break;
        }
        k = i;
    }
    bool open_top = found && (above.r == T_EMPTY || above.r == T_STEAM) &&
                    (above.a & FLAG_HIDDEN) == 0u;
    float dist = float(k) + fract(f.y);  // cells below the surface's top edge
    float deep = clamp(dist / 12.0, 0.0, 1.0);
    ivec2 world = u_origin + p;
    int ti = int(t);

    vec3 body = albedo * u_tint[ti] * mix(1.0, 0.7, u_depth[ti] * deep);
    if (t == T_OIL) {
        float phase = float(world.x) * 0.19 + float(world.y) * 0.11 + float(u_step / 8) * 0.013;
        vec3 irid = 0.5 + 0.5 * cos(6.2831853 * (phase + vec3(0.0, 0.33, 0.67)));
        return body + u_spec[ti] * (k == 0 ? 0.16 : 0.06) * irid;
    }

    if (u_tile_mean[ti] >= 0.0) {
        int s = u_step;
        float c1 = tile(t, world + ivec2(s / 24, s / 70));
        float c2 = tile(t, ivec2(-world.x, world.y) + ivec2(s / 31, -s / 53));
        float caus = min(c1, c2) - u_tile_mean[ti] * 0.75;
        body *= 1.0 + u_strength[ti] * caus * exp(-dist / 6.0);
    }

    ivec2 frag = ivec2(gl_FragCoord.xy);
    ivec2 size = textureSize(u_behind, 0);
    vec3 behind = texelFetch(u_behind, frag, 0).rgb;
    vec3 through = behind;
    if (open_top && u_reflect[ti] > 0.0) {
        float row = float(world.y);
        float wob = sin(row * 1.7 + float(u_step) * 0.07) +
                    0.5 * sin(row * 0.6 - float(u_step) * 0.045);
        vec2 delta = vec2(wob * 0.35 * min(dist, 4.0), -2.0 * dist) * px_per_cell;
        ivec2 q = clamp(frag + ivec2(round(delta)), ivec2(0), size - 1);
        vec3 refl = texelFetch(u_behind, q, 0).rgb;
        float w = u_reflect[ti] * mix(0.9, 0.35, clamp(dist / 16.0, 0.0, 1.0));
        through = mix(behind, refl, w);
    }
    float opacity = mix(0.5, 0.92, clamp(u_depth[ti] * dist / 9.0, 0.0, 1.0));
    vec3 col = mix(through, body, opacity);

    if (open_top && k == 0) {
        int flow = int(c.a >> uint(FLOW_MAG_SHIFT));
        float foam = clamp(float(flow) / 6.0, 0.0, 1.0);
        vec3 glint = mix(vec3(1.0, 0.95, 0.82), vec3(1.0), foam);
        col += glint * u_spec[ti] * (0.12 + 0.25 * foam);
    }
    return col;
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
    // Screen pixels per cell along each axis, signed (the framebuffer's y may run
    // either way): the cell coordinate's derivative is exact and constant over
    // the quad. Taken here, in uniform control flow, where derivatives are
    // defined; the water reflection uses it.
    vec2 f = v_uv * vec2(textureSize(u_tex, 0));
    vec2 px_per_cell = 1.0 / vec2(dFdx(f.x), dFdy(f.y));
    if (u_mode == 0) {
        o_color = colour;
        if (u_look == 1 && (c.a & FLAG_HIDDEN) == 0u && c.r != T_EMPTY && c.r != T_FIRE) {
            int ti = int(min(c.r, 15u));
            vec3 rgb = colour.rgb;
            if (c.r == T_WATER || c.r == T_OIL) {
                rgb = liquid(rgb, p, c, f, px_per_cell);
            } else if (c.r == T_STEAM) {
                // Soft puffs: thinner at a puff's edge (fewer steam neighbours)
                // and as its life runs out, so a cloud has a body and frays.
                int n = 0;
                if (cell_at(p + ivec2(1, 0)).r == T_STEAM) ++n;
                if (cell_at(p - ivec2(1, 0)).r == T_STEAM) ++n;
                if (cell_at(p + ivec2(0, 1)).r == T_STEAM) ++n;
                if (cell_at(p - ivec2(0, 1)).r == T_STEAM) ++n;
                float life = clamp(float(c.g) / 60.0 + 0.35, 0.0, 1.0);
                rgb *= u_tint[ti];
                o_color.a *= (0.3 + 0.1 * float(n)) * life;
            } else {
                rgb *= u_tint[ti];
                // A tile on a static material only (a powder has no tile in any
                // mood today, and keeps its per-cell jitter if one is named).
                bool powder = c.r == 1u;
                if (!powder && u_tile_mean[ti] >= 0.0)
                    rgb *= 1.0 + u_strength[ti] * (tile(c.r, u_origin + p) - u_tile_mean[ti]);
                if ((c.a & FLAG_OCCLUDER) != 0u && u_rim[ti] > 0.0)
                    rgb *= 1.0 + u_rim[ti] * 0.5 * rim_of(p);
            }
            o_color.rgb = clamp(rgb, 0.0, 1.0);
        }
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
