#version 330 core
// R6 weather's fragments (see weather.vert), added onto the HDR frame after the
// resolve. Lit as the world is -- ambient, sun, the point lights, the same
// falloff as resolve.frag -- so rain is near-invisible in the dark and glitters
// in a lamp's pool, which is most of what makes rain at night read as rain.
// Stopped at the first thing it cannot fall through in its column
// (weather_top.frag), per pixel, so a drop ends exactly on the ground's surface
// and never streaks across a wall or into the lake.
in vec2 v_px;
in vec2 v_size;
in float v_seed;

uniform sampler2D u_top;  // one texel per pack column: first stopping row
uniform int u_pack_w;
uniform float u_target_h;
uniform vec2 u_origin_px;
uniform float u_scale;
uniform vec2 u_view;
uniform int u_kind;
uniform float u_alpha;
uniform vec3 u_tint;
uniform vec3 u_ambient;
uniform vec3 u_sun_color;
uniform int u_light_count;
uniform vec4 u_light_pos[64];
uniform vec3 u_light_color[64];

out vec4 o_color;

void main() {
    vec2 screen = vec2(gl_FragCoord.x, u_target_h - gl_FragCoord.y);
    vec2 cell = (screen - u_origin_px) / u_scale;
    int col = int(floor(cell.x));
    if (col >= 0 && col < u_pack_w && cell.y >= texelFetch(u_top, ivec2(col, 0), 0).r) discard;

    float a;
    if (u_kind == 1) {
        // Brightest at the head, fading up the streak.
        a = v_px.y / v_size.y;
        a *= a;
    } else if (u_kind == 2) {
        // A soft bank, evaluated on the cell grid so its edge steps in cells as
        // the art does, rather than a smooth gradient no pixel artist would draw.
        vec2 q = (floor(v_px / u_scale) + 0.5) * u_scale / v_size * 2.0 - 1.0;
        a = clamp(1.0 - dot(q, q), 0.0, 1.0);
        a = floor(a * 4.0) / 4.0;
    } else {
        a = 0.5 + 0.5 * v_seed;
    }

    vec2 world = u_view + cell;
    vec3 light = u_ambient + 0.5 * u_sun_color;
    for (int i = 0; i < u_light_count; ++i) {
        float f = clamp(1.0 - distance(world, u_light_pos[i].xy) / u_light_pos[i].z, 0.0, 1.0);
        light += u_light_color[i] * (f * f);
    }
    o_color = vec4(u_tint * light * (a * u_alpha), 0.0);
}
