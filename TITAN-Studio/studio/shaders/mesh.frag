#version 440
// TITAN Studio viewport — fragment stage: hemispheric ambient + one directional light,
// distance fog towards the canvas colour. Colours are authored in sRGB and lit in linear.
// Original TITAN code.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in float vDepth;

layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 mvp;
    mat4 model;
    mat4 normalMat;
    vec4 color;
    vec4 lightDir;   // xyz towards light, w = ambient strength
    vec4 params;     // x = lit, y = selected, z = light intensity, w = ghost
    vec4 sky;        // rgb canvas/sky, w = fog density
};

vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 toSrgb(vec3 c) { return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2)); }

void main()
{
    vec3 base = toLinear(color.rgb);
    vec3 skyLin = toLinear(sky.rgb);
    vec3 c = base;
    if (params.x > 0.5) {
        vec3 n = normalize(vNormal);
        vec3 l = normalize(lightDir.xyz);
        float ndl = max(dot(n, l), 0.0);
        float hemi = 0.5 + 0.5 * n.y;
        vec3 ambient = mix(vec3(0.20, 0.19, 0.18), max(skyLin, vec3(0.35)), hemi) * lightDir.w * 2.2;
        c = base * (ndl * params.z * vec3(1.0, 0.97, 0.92) + ambient);
        float fog = 1.0 - exp(-pow(vDepth * sky.w, 2.0));
        c = mix(c, skyLin, clamp(fog, 0.0, 0.85));
    }
    if (params.y > 0.5) c += base * 0.08;
    fragColor = vec4(toSrgb(c), color.a);
}
