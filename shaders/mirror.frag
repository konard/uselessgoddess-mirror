#version 450

/*
 * Replacement fragment shader injected by the mirror layer.
 *
 * mirror_mode is provided through specialization constants at pipeline
 * creation time:
 *   0 - depth visualization: grayscale equal to the window-space depth
 *       (gl_FragCoord.z), i.e. exactly the value the depth buffer would hold.
 *   1 - highlight: a configurable solid color, slightly brightened for
 *       fragments that are closer to the camera so highlighted objects keep
 *       depth contrast.
 */

layout(constant_id = 0) const uint mirror_mode = 0u;
layout(constant_id = 1) const float highlight_r = 1.0;
layout(constant_id = 2) const float highlight_g = 0.0;
layout(constant_id = 3) const float highlight_b = 0.0;

layout(location = 0) out vec4 out_color;

void main() {
    float depth = gl_FragCoord.z;
    if (mirror_mode == 0u) {
        out_color = vec4(vec3(depth), 1.0);
    } else {
        float luminance = 1.0 - 0.5 * depth;
        out_color = vec4(vec3(highlight_r, highlight_g, highlight_b) * luminance, 1.0);
    }
}
