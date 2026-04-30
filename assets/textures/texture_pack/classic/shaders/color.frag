#version 450

layout(location = 0) in vec3 frag_color;
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
    out_color = vec4(encode_swapchain_color(frag_color), 1.0);
}
