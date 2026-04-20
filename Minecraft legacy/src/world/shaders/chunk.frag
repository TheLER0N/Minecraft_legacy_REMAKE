#version 450 core

layout(location = 0) in vec3 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 fragColor;

layout(set = 0, binding = 0) uniform sampler2DArray uTexture;

void main()
{
    vec4 texColor = texture(uTexture, v_uv);
    if (texColor.a < 0.1) {
        discard;
    }
    fragColor = v_color * texColor;
}
