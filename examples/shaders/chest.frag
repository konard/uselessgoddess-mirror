#version 450

layout(location = 0) out vec4 out_color;

void main() {
    float band = mod(floor(gl_FragCoord.y / 8.0), 2.0);
    vec3 base = mix(vec3(0.55, 0.40, 0.10), vec3(0.75, 0.55, 0.20), band);
    out_color = vec4(base, 1.0);
}
