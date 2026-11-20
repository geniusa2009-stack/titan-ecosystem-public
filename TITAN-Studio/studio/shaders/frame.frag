#version 440
// TITAN Studio: engine frame passthrough. The pixels are already tonemapped and sRGB-encoded by
// Unreal (SCS_FinalColorLDR) and the swapchain is not sRGB, so no conversion happens here.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 fragColor;
layout(std140, binding = 0) uniform buf {
    vec4 params;
};
layout(binding = 1) uniform sampler2D frameTex;
void main() {
    fragColor = vec4(texture(frameTex, v_uv).rgb * params.y, 1.0);
}
