#version 450

layout(location = 0) in vec3 frag_color;
layout(location = 1) in vec2 frag_uv;
layout(location = 2) flat in uint frag_tex_index;

layout(binding = 0) uniform sampler2DArray texSampler;

layout(location = 0) out vec4 out_color;

layout(constant_id = 0) const bool kEncodeSrgbOutput = false;

vec3 encode_swapchain_color(vec3 color) {
    color = clamp(color, vec3(0.0), vec3(1.0));
    if (!kEncodeSrgbOutput) {
        return color;
    }
    vec3 low = color * 12.92;
    vec3 high = 1.055 * pow(color, vec3(1.0 / 2.4)) - vec3(0.055);
    return mix(high, low, lessThanEqual(color, vec3(0.0031308)));
}

void main() {
    vec4 tex_color = texture(texSampler, vec3(frag_uv, float(frag_tex_index)));
    vec3 water_tint = vec3(0.46, 0.68, 0.92);
    vec3 tinted = mix(tex_color.rgb, water_tint, 0.45) * clamp(frag_color, vec3(0.04), vec3(1.0));
    tinted = encode_swapchain_color(tinted);
    float alpha = max(tex_color.a * 0.58, 0.42);
    out_color = vec4(tinted, alpha);
}
