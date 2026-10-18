#version 330 core
// A fill samples a 1x1 white texture and takes its colour from the vertex; a
// blit samples its texture with a white vertex colour. One program for both.
//
// The second output is the emissive attachment of a lit frame (the multiply
// light model, src/gpu/pass_graph.h): a sprite gives off no light, and its
// alpha covers whatever light was painted under it -- a player standing in
// front of a flame hides the flame's glow. Outside a lit frame only attachment
// 0 is drawn to and this output goes nowhere.
in vec2 v_uv;
in vec4 v_color;

uniform sampler2D u_tex;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_emissive;

void main() {
    o_color = texture(u_tex, v_uv) * v_color;
    o_emissive = vec4(0.0, 0.0, 0.0, o_color.a);
}
