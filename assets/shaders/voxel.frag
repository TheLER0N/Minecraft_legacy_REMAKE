#version 450

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) flat in uint frag_tex_index;

layout(binding = 0) uniform sampler2DArray texSampler;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex_color = texture(texSampler, vec3(frag_uv, float(frag_tex_index)));
    out_color = tex_color * vec4(frag_color, 1.0);
}