#version 450

/*
 * Fullscreen triangle at z = 0. The effective fragment depth is controlled
 * by the caller through the viewport's minDepth/maxDepth range, which lets
 * tests place draws at exact depths without vertex buffers.
 */
void main() {
    vec2 position = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);
}
