#version 450

layout(push_constant) uniform PushConstants
{
    mat4 u_mvp;
} pc;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_color;

layout(location = 0) out vec3 v_color;

void main()
{
    gl_Position = pc.u_mvp * vec4(a_position, 1.0);
    v_color = a_color;
}
