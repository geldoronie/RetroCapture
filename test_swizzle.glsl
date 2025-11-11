#version 330
void main() {
    vec3 test1 = 0.0.xxx;
    vec3 test2 = mix(1.0.xxx + vec3(1.0), 1.0.xxx, 0.5);
    vec3 test3 = pow(vec3(1.0), 0.70.xxx - 0.325);
    gl_FragColor = vec4(test1 + test2 + test3, 1.0);
}
