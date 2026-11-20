#version 440
// TITAN Studio viewport — vertex stage. Original TITAN code.
layout(location = 0) in vec3 position;
layout(location = 1) in vec3 normal;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out float vDepth;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 model;
    mat4 normalMat;
    vec4 color;
    vec4 lightDir;
    vec4 params;
    vec4 sky;
};

void main()
{
    vNormal = mat3(normalMat) * normal;
    gl_Position = mvp * vec4(position, 1.0);
    vDepth = gl_Position.w;
}
