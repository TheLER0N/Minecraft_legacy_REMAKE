#version 450

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

layout(binding = 0) uniform sampler2D ui_sampler;

void main() {
    vec4 tex_color = texture(ui_sampler, in_uv);
    if (tex_color.a < 0.1) {
        discard;
    }
    out_color = tex_color;
}
