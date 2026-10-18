#version 330 core
// The multiply light model's resolve (src/gpu/pipeline.cpp): per screen pixel,
//     albedo x (ambient + sun x visibility + fire light + point lights) + emissive
// in linear light, into the half-float HDR target. The albedo is the world as
// the frame code painted it, display-encoded (sRGB), so it is decoded first;
// post.frag encodes the result back. The output's alpha is the albedo's.
uniform sampler2D u_albedo;
uniform sampler2D u_emissive;
uniform sampler2D u_blocks;  // propagated fire light, linear, one texel per block
uniform sampler2D u_sun;     // visibility, one texel per cell

uniform float u_target_h;
uniform vec2 u_origin_px;     // screen position of the pack's cell (0, 0)
uniform float u_scale;        // pixels per cell
uniform vec2 u_view;          // world cell of the pack's cell (0, 0)
uniform vec2 u_block_extent;  // cells the block texture covers
uniform vec2 u_pack_size;     // cells the pack covers

uniform vec3 u_ambient;
uniform int u_sun_on;
uniform vec3 u_sun_color;
uniform float u_fire_gain;
uniform float u_time;
uniform int u_light_count;
uniform vec4 u_light_pos[64];  // world x, y, radius, flicker
uniform vec3 u_light_color[64];

out vec4 o_color;

vec3 to_linear(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main() {
    ivec2 px = ivec2(gl_FragCoord.xy);
    vec4 a = texelFetch(u_albedo, px, 0);
    // The frame is drawn y-down (sprite.vert), so screen row = target height - y.
    vec2 screen = vec2(gl_FragCoord.x, u_target_h - gl_FragCoord.y);
    vec2 cell = (screen - u_origin_px) / u_scale;

    vec3 light = u_ambient;
    if (u_sun_on != 0) light += u_sun_color * texture(u_sun, cell / u_pack_size).r;
    // Tone-mapped as the legacy light texture is (LightField's Reinhard), so a
    // fire reaches as far as it does today.
    vec3 fire = texture(u_blocks, cell / u_block_extent).rgb;
    light += u_fire_gain * fire / (1.0 + fire);

    vec2 world = u_view + cell;
    for (int i = 0; i < u_light_count; ++i) {
        vec4 l = u_light_pos[i];
        float f = clamp(1.0 - distance(world, l.xy) / l.z, 0.0, 1.0);
        if (f <= 0.0) continue;
        // Two incommensurate sines per light, phased by its index: a waver, not a
        // strobe, and no two lamps in step.
        float fi = float(i);
        float w = 0.5 + 0.25 * sin(u_time * (6.1 + 1.3 * fi) + 2.3 * fi) +
                  0.25 * sin(u_time * (17.3 + 0.7 * fi) + 0.9 * fi);
        light += u_light_color[i] * (f * f) * (1.0 - l.w * w);
    }

    vec3 e = texelFetch(u_emissive, px, 0).rgb;
    o_color = vec4(to_linear(a.rgb) * light + e, a.a);
}
