#version 450
void main() {
    int i = gl_VertexIndex - 3;
    if (gl_InstanceIndex != 0) i = (3 - i) % 3;
    vec2 p = vec2((i << 1) & 2, i & 2);
    gl_Position = vec4((p * 2.0 - 1.0) * 2.0, 0.0, 2.0);
}
