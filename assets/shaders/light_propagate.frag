#version 330 core
// The light pass, step 2 of 3: one iteration of LightField's max-propagation
// (src/render/light.cpp, the propagate sweep), run ITERATIONS times ping-pong.
// Light takes the brightest neighbour, paying the crossing cost of this block
// at the step's length -- 1 orthogonal, sqrt 2 diagonal, sqrt 5 for a knight's
// move, which may not jump a wall (the lesser of the far block and the one it
// passes through) -- with the block's own emission as a floor.
uniform sampler2D u_front;  // the last iteration
uniform sampler2D u_emit;   // rgb emission, a log transmission
uniform float u_min_visible;

out vec4 o_color;

vec3 at(ivec2 p) { return texelFetch(u_front, p, 0).rgb; }

void main() {
    ivec2 i = ivec2(gl_FragCoord.xy);
    ivec2 n = textureSize(u_front, 0);
    bool l = i.x > 0, r = i.x < n.x - 1, u = i.y > 0, d = i.y < n.y - 1;

    vec3 best = vec3(0.0);
    if (l) best = max(best, at(i + ivec2(-1, 0)));
    if (r) best = max(best, at(i + ivec2(1, 0)));
    if (u) best = max(best, at(i + ivec2(0, -1)));
    if (d) best = max(best, at(i + ivec2(0, 1)));

    vec3 diag = vec3(0.0);
    if (l && u) diag = max(diag, at(i + ivec2(-1, -1)));
    if (r && u) diag = max(diag, at(i + ivec2(1, -1)));
    if (l && d) diag = max(diag, at(i + ivec2(-1, 1)));
    if (r && d) diag = max(diag, at(i + ivec2(1, 1)));

    vec3 knight = vec3(0.0);
    bool l2 = i.x > 1, r2 = i.x < n.x - 2, u2 = i.y > 1, d2 = i.y < n.y - 2;
    if (l && u2) knight = max(knight, min(at(i + ivec2(-1, -2)), at(i + ivec2(0, -1))));
    if (r && u2) knight = max(knight, min(at(i + ivec2(1, -2)), at(i + ivec2(0, -1))));
    if (l && d2) knight = max(knight, min(at(i + ivec2(-1, 2)), at(i + ivec2(0, 1))));
    if (r && d2) knight = max(knight, min(at(i + ivec2(1, 2)), at(i + ivec2(0, 1))));
    if (l2 && u) knight = max(knight, min(at(i + ivec2(-2, -1)), at(i + ivec2(-1, 0))));
    if (r2 && u) knight = max(knight, min(at(i + ivec2(2, -1)), at(i + ivec2(1, 0))));
    if (l2 && d) knight = max(knight, min(at(i + ivec2(-2, 1)), at(i + ivec2(-1, 0))));
    if (r2 && d) knight = max(knight, min(at(i + ivec2(2, 1)), at(i + ivec2(1, 0))));

    vec4 em = texelFetch(u_emit, i, 0);
    float k = exp(em.a), kd = exp(em.a * 1.41421356), kk = exp(em.a * 2.23606798);
    vec3 v = max(max(em.rgb, best * k), max(diag * kd, knight * kk));
    // Under one 8-bit step is not light (MIN_VISIBLE): cut to zero.
    v *= step(vec3(u_min_visible), v);
    o_color = vec4(v, 1.0);
}
