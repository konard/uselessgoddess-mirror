#version 450

layout(location = 0) out vec4 out_color;

void main() {
    float stripe = mod(floor((gl_FragCoord.x + gl_FragCoord.y) / 16.0), 2.0);
    vec3 base = mix(vec3(0.25, 0.30, 0.25), vec3(0.30, 0.35, 0.30), stripe);
    out_color = vec4(base, 1.0);
}
