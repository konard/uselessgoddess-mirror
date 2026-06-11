#version 450

layout(location = 0) out vec4 out_color;

void main() {
    float t = gl_FragCoord.y / 512.0;
    out_color = vec4(mix(vec3(0.35, 0.55, 0.85), vec3(0.10, 0.15, 0.30), t), 1.0);
}
