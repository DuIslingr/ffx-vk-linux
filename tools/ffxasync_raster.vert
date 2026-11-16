#version 450
// ffxasync's raster load: a full-screen triangle per instance, depth only (no fragment shader) - fill and depth bound, the
// shader cores mostly idle, as a game's shadow maps or depth prepass. Its cost grows with the instance count.
void main() {
    const vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, float(gl_InstanceIndex % 1000) / 1000.0, 1.0);
}
