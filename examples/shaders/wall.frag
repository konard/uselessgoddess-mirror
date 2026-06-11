#version 450

layout(location = 0) out vec4 out_color;

void main() {
    float checker = mod(floor(gl_FragCoord.x / 24.0) + floor(gl_FragCoord.y / 24.0), 2.0);
    vec3 base = mix(vec3(0.45, 0.35, 0.30), vec3(0.55, 0.45, 0.40), checker);
    out_color = vec4(base, 1.0);
}
