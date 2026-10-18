#version 330 core
// One triangle that covers the whole target, made from gl_VertexID, for the
// light pipeline's passes (src/gpu/pipeline.cpp). Each pass reads its inputs by
// gl_FragCoord, so there are no attributes and no texture coordinates here.
void main() {
    vec2 p = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
