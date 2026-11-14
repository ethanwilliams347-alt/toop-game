#version 330 core
// R6 weather (src/gpu/pipeline.cpp): instanced and stateless. Each instance is
// one drop, wisp or mote whose position is a function of its index and the
// simulation time alone -- no buffer of particles, nothing carried between
// frames -- so a replay, a pause or a frame rendered twice is the same picture,
// and the cost is the draw and nothing else. Positions wrap over a span a
// little larger than the screen and are offset by the camera, so the weather
// is in the world: pan right and the rain slides left past you like the hills.
// Corners are snapped to whole pixels (dust to whole cells) so nothing smears
// across a pixel edge; the art stays crisp.
uniform vec2 u_target;   // pixels
uniform vec2 u_cam;      // the camera's world position, in screen pixels
uniform float u_scale;   // pixels per cell
uniform float u_t;       // simulation seconds: step / 60
uniform int u_kind;      // 1 rain, 2 fog wisps, 3 dust
uniform vec2 u_dir;      // unit, the way it falls, +y down
uniform float u_speed;   // pixels per second

out vec2 v_px;    // pixels from the quad's tail corner, along its own axes
out vec2 v_size;  // the quad's size in pixels: across, along
out float v_seed;

float hash(float n) { return fract(sin(n * 12.9898 + 78.233) * 43758.5453); }

void main() {
    float i = float(gl_InstanceID);
    float h1 = hash(i * 1.37 + 0.11), h2 = hash(i * 2.71 + 0.53), h3 = hash(i * 0.91 + 0.97);
    v_seed = h3;

    vec2 dir = u_dir;
    float speed = u_speed * (0.85 + 0.3 * h3);  // no two in lockstep
    vec2 size;  // across, along
    float parallax = 1.0;
    if (u_kind == 1) {
        // A streak about as long as it falls in a thirtieth of a second, as a
        // drop is seen; two pixels or a quarter cell wide, whichever is more.
        size = vec2(max(2.0, floor(u_scale * 0.25)), clamp(speed / 30.0, 3.0 * u_scale, 14.0 * u_scale));
        parallax = 0.9 + 0.2 * h3;
    } else if (u_kind == 2) {
        // Long low banks drifting sideways, slower than the wind they ride.
        size = vec2((6.0 + 6.0 * h3) * u_scale, (30.0 + 40.0 * h1) * u_scale);
        dir = vec2(sign(u_dir.x + 1e-3), 0.0);
        speed *= 0.08;
        parallax = 0.6 + 0.4 * h3;
    } else {
        size = vec2(u_scale);
        speed *= 0.05;
        dir = normalize(vec2(u_dir.x + 0.3 * sin(u_t * 0.7 + 6.2831 * h1), 0.15 + 0.3 * h2));
    }

    vec2 margin = vec2(size.y + 2.0 * u_scale);
    vec2 span = u_target + 2.0 * margin;
    vec2 head = mod(vec2(h1, h2) * span + dir * speed * u_t - u_cam * parallax, span) - margin;
    if (u_kind == 3) head = floor(head / u_scale) * u_scale;

    // The quad: across is perpendicular to the fall, along runs tail to head.
    vec2 across = vec2(-dir.y, dir.x);
    vec2 c = vec2((gl_VertexID == 1 || gl_VertexID == 2 || gl_VertexID == 4) ? 1.0 : 0.0,
                  (gl_VertexID == 2 || gl_VertexID == 4 || gl_VertexID == 5) ? 1.0 : 0.0);
    if (u_kind == 2) across = vec2(0.0, 1.0);
    vec2 tail = head - dir * size.y;
    vec2 p = tail + across * ((c.x - 0.5) * size.x) + dir * (c.y * size.y);
    // Wisps and motes are upright boxes: a wisp lies along its drift, a mote
    // sits on the cell its snapped head names.
    if (u_kind == 2) p = tail + vec2(c.y * size.y * dir.x, c.x * size.x);
    if (u_kind == 3) p = head + vec2(c.y, c.x) * u_scale;
    p = floor(p + 0.5);
    v_px = vec2(c.x * size.x, c.y * size.y);
    v_size = size;
    gl_Position = vec4(p.x / u_target.x * 2.0 - 1.0, 1.0 - p.y / u_target.y * 2.0, 0.0, 1.0);
}
