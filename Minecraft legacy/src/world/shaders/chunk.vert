#version 450 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_uv; // x=u, y=v, z=layer
layout(location = 2) in vec4 a_color;

layout(location = 0) out vec3 v_uv;
layout(location = 1) out vec4 v_color;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
} push;

void main()
{
    gl_Position = vec4(a_position, 1.0) * push.uMVP;
    v_uv = a_uv;
    v_color = a_color;
}
