#version 440
// TITAN Studio: full-screen triangle that shows a frame rendered by the engine (Unreal). Original TITAN code.
layout(location = 0) out vec2 v_uv;
layout(std140, binding = 0) uniform buf {
    vec4 params;  // x: +1 if NDC Y points up (Metal, D3D, GL), -1 otherwise (Vulkan); y: brightness
};
out gl_PerVertex { vec4 gl_Position; };
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));  // (0,0) (2,0) (0,2)
    v_uv = params.x > 0.0 ? vec2(p.x, 1.0 - p.y) : p;                              // image rows are top-first
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
