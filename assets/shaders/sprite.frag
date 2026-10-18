#version 330 core
// A fill samples a 1x1 white texture and takes its colour from the vertex; a
// blit samples its texture with a white vertex colour. One program for both.
in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_tex;

out vec4 o_color;

void main() {
    o_color = texture(u_tex, v_uv) * v_color;
}
