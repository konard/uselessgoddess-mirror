#version 450

layout(location = 0) out vec4 out_color;

void main() {
    float band = mod(floor(gl_FragCoord.y / 12.0), 2.0);
    vec3 base = mix(vec3(0.60, 0.20, 0.15), vec3(0.70, 0.30, 0.20), band);
    out_color = vec4(base, 1.0);
}
