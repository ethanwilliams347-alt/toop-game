#version 330 core
// One step back up the bloom chain (src/gpu/pipeline.cpp): the smaller level
// through a 3x3 tent, added (GL_ONE, GL_ONE) onto the level above it, so the
// top level ends up holding every width of glow at once -- a tight halo round a
// lamp and a wide soft one round it.
uniform sampler2D u_src;
uniform vec2 u_src_texel;
uniform vec2 u_dst_size;

out vec4 o_color;

void main() {
    vec2 uv = gl_FragCoord.xy / u_dst_size;
    vec2 o = u_src_texel;
    vec3 s = 4.0 * texture(u_src, uv).rgb;
    s += 2.0 * (texture(u_src, uv + vec2(o.x, 0.0)).rgb + texture(u_src, uv - vec2(o.x, 0.0)).rgb +
                texture(u_src, uv + vec2(0.0, o.y)).rgb + texture(u_src, uv - vec2(0.0, o.y)).rgb);
    s += texture(u_src, uv + o).rgb + texture(u_src, uv - o).rgb +
         texture(u_src, uv + vec2(o.x, -o.y)).rgb + texture(u_src, uv + vec2(-o.x, o.y)).rgb;
    o_color = vec4(s / 16.0, 1.0);
}
