#version 450

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) flat in uint frag_tex_index;

layout(binding = 0) uniform sampler2DArray texSampler;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex_color = texture(texSampler, vec3(frag_uv, float(frag_tex_index)));
    if (tex_color.a < 0.5) {
        discard;
    }
    vec3 lit_color = tex_color.rgb * clamp(frag_color, vec3(0.0), vec3(1.0));
    lit_color = clamp(lit_color, vec3(0.0), vec3(1.0));
    out_color = vec4(lit_color, tex_color.a);
}
