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
// Aerial fog (GlCanvas::set_fog): rgb the fog's colour in linear light, a how
// much of it stands in front of what is drawn now. 0 outside a lit frame and
// for everything not in the backdrop, so those draws are exactly as before.
uniform vec4 u_fog;
// How much less lamp light what is drawn now takes than the world does
// (GlCanvas::set_depth, mood `lamp_backdrop`): 0 for the world and every
// sprite, so the default of an unset uniform is the old behaviour. The third
// output carries 1 - it to the resolve, blended by this draw's coverage, so a
// backdrop pixel ends up with its own layer's share and a world pixel with 1.
uniform float u_unlit;

layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_emissive;
layout(location = 2) out vec4 o_receive;

void main() {
    o_color = texture(u_tex, v_uv) * v_color;
    o_emissive = vec4(0.0, 0.0, 0.0, o_color.a);
    o_receive = vec4(1.0 - u_unlit, 0.0, 0.0, o_color.a);
    if (u_fog.a > 0.0) {
        // The paint dimmed in linear light by what the fog hides, and the fog's
        // own light put in the emissive attachment, which the resolve adds
        // unlit: haze glows faintly at night instead of going black with the
        // ambient, as Rain World's far layers do.
        vec3 lin = mix(o_color.rgb / 12.92, pow((o_color.rgb + 0.055) / 1.055, vec3(2.4)),
                       step(vec3(0.04045), o_color.rgb));
        lin *= 1.0 - u_fog.a;
        o_color.rgb = mix(lin * 12.92, 1.055 * pow(lin, vec3(1.0 / 2.4)) - 0.055,
                          step(vec3(0.0031308), lin));
        o_emissive.rgb = u_fog.rgb * u_fog.a;
    }
}
