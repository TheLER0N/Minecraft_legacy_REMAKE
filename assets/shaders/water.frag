#version 450

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) flat in uint frag_tex_index;

layout(binding = 0) uniform sampler2DArray texSampler;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 tex_color = texture(texSampler, vec3(frag_uv, float(frag_tex_index)));
    vec3 water_tint = vec3(0.46, 0.68, 0.92);
    vec3 tinted = mix(tex_color.rgb, water_tint, 0.45) * clamp(frag_color, vec3(0.04), vec3(1.0));
    float alpha = max(tex_color.a * 0.58, 0.42);
    out_color = vec4(tinted, alpha);
}
