#version 450
uniform sampler2D Rows;
uniform isamplerBuffer Faces;
layout(location=0) out vec4 color;
void main() {
    float row = texelFetch(Rows, ivec2(gl_FragCoord.xy), 0).r;
    float wOk = abs(1.0 / gl_FragCoord.w - 2.0) < 0.001 ? 1.0 : 0.0;
    float facing = gl_FrontFacing && texelFetch(Faces, 0).r == -7 ? 1.0 : 0.0;
    color = vec4(row, wOk, facing, 1.0);
}
