#include "render/renderer.hpp"

#include "common/log.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <sstream>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace ml {

static_assert(sizeof(Mat4) == 64, "Mat4 must match GLSL mat4 push constant size");

namespace {

constexpr int kMaxFramesInFlight = 2;

struct QueueFamilySelection {
    std::optional<std::uint32_t> graphics_family;
    std::optional<std::uint32_t> present_family;
};

struct ClipPoint {
    float x {0.0f};
    float y {0.0f};
    float z {0.0f};
    float w {1.0f};
};

std::uint32_t decode_utf8(const char*& it, const char* end) {
    if (it == end) return 0;
    std::uint32_t ch = static_cast<std::uint8_t>(*it++);
    if (ch < 0x80) return ch;
    if (ch < 0xC0) return 0xFFFD;
    if (ch < 0xE0) {
        if (it == end) return 0xFFFD;
        ch = ((ch & 0x1F) << 6) | (static_cast<std::uint8_t>(*it++) & 0x3F);
    } else if (ch < 0xF0) {
        if (it + 1 >= end) { it = end; return 0xFFFD; }
        ch = ((ch & 0x0F) << 12) | ((static_cast<std::uint8_t>(*it++) & 0x3F) << 6) | (static_cast<std::uint8_t>(*it++) & 0x3F);
    } else if (ch < 0xF8) {
        if (it + 2 >= end) { it = end; return 0xFFFD; }
        ch = ((ch & 0x07) << 18) | ((static_cast<std::uint8_t>(*it++) & 0x3F) << 12) | ((static_cast<std::uint8_t>(*it++) & 0x3F) << 6) | (static_cast<std::uint8_t>(*it++) & 0x3F);
    }
    return ch;
}

struct DrawSectionView {
    VkBuffer vertex_buffer {VK_NULL_HANDLE};
    VkBuffer index_buffer {VK_NULL_HANDLE};
    std::uint32_t index_count {0};
};

ClipPoint transform_point_clip(const Mat4& matrix, const Vec3& point) {
    return {
        matrix.m[0] * point.x + matrix.m[4] * point.y + matrix.m[8] * point.z + matrix.m[12],
        matrix.m[1] * point.x + matrix.m[5] * point.y + matrix.m[9] * point.z + matrix.m[13],
        matrix.m[2] * point.x + matrix.m[6] * point.y + matrix.m[10] * point.z + matrix.m[14],
        matrix.m[3] * point.x + matrix.m[7] * point.y + matrix.m[11] * point.z + matrix.m[15]
    };
}

bool point_inside_aabb(const Vec3& point, const Aabb& bounds) {
    return point.x >= bounds.min.x && point.x <= bounds.max.x &&
        point.y >= bounds.min.y && point.y <= bounds.max.y &&
        point.z >= bounds.min.z && point.z <= bounds.max.z;
}

bool same_block_hit(const std::optional<BlockHit>& lhs, const std::optional<BlockHit>& rhs) {
    if (lhs.has_value() != rhs.has_value()) {
        return false;
    }
    if (!lhs.has_value()) {
        return true;
    }
    return lhs->hit == rhs->hit &&
        lhs->block == rhs->block &&
        lhs->normal == rhs->normal &&
        lhs->placement_block == rhs->placement_block;
}

VkSurfaceFormatKHR choose_surface_format(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR choose_present_mode(const std::vector<VkPresentModeKHR>& modes) {
    for (const auto mode : modes) {
        if (mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            return mode;
        }
    }
    for (const auto mode : modes) {
        if (mode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D choose_extent(const VkSurfaceCapabilitiesKHR& capabilities) {
    if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
        return capabilities.currentExtent;
    }

    return {
        std::clamp(1600u, capabilities.minImageExtent.width, capabilities.maxImageExtent.width),
        std::clamp(900u, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)
    };
}

QueueFamilySelection find_queue_families(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilySelection result {};

    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> properties(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, properties.data());

    for (std::uint32_t i = 0; i < count; ++i) {
        if ((properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
            result.graphics_family = i;
        }

        VkBool32 present_supported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &present_supported);
        if (present_supported == VK_TRUE) {
            result.present_family = i;
        }
    }

    return result;
}

void append_box_edges(std::vector<Vertex>& vertices, Vec3 min_corner, Vec3 max_corner, Vec3 color) {
    const std::array<Vec3, 8> corners {{
        {min_corner.x, min_corner.y, min_corner.z},
        {max_corner.x, min_corner.y, min_corner.z},
        {max_corner.x, min_corner.y, max_corner.z},
        {min_corner.x, min_corner.y, max_corner.z},
        {min_corner.x, max_corner.y, min_corner.z},
        {max_corner.x, max_corner.y, min_corner.z},
        {max_corner.x, max_corner.y, max_corner.z},
        {min_corner.x, max_corner.y, max_corner.z}
    }};

    constexpr std::array<std::array<int, 2>, 12> edges {{
        {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}},
        {{4, 5}}, {{5, 6}}, {{6, 7}}, {{7, 4}},
        {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}}
    }};

    for (const auto& edge : edges) {
        vertices.push_back({corners[edge[0]], color});
        vertices.push_back({corners[edge[1]], color});
    }
}

float screen_x_to_ndc(float x, float width) {
    return width > 0.0f ? (x / width) * 2.0f - 1.0f : 0.0f;
}

float screen_y_to_ndc(float y, float height) {
    return height > 0.0f ? (y / height) * 2.0f - 1.0f : 0.0f;
}

void append_hud_rect_fill(
    std::vector<Vertex>& vertices,
    float left,
    float top,
    float right,
    float bottom,
    float width,
    float height,
    Vec3 color) {
    const float x0 = screen_x_to_ndc(left, width);
    const float x1 = screen_x_to_ndc(right, width);
    const float y0 = screen_y_to_ndc(top, height);
    const float y1 = screen_y_to_ndc(bottom, height);

    vertices.push_back({{x0, y0, 0.0f}, color});
    vertices.push_back({{x0, y1, 0.0f}, color});
    vertices.push_back({{x1, y1, 0.0f}, color});
    vertices.push_back({{x1, y1, 0.0f}, color});
    vertices.push_back({{x1, y0, 0.0f}, color});
    vertices.push_back({{x0, y0, 0.0f}, color});
}

void append_hud_rect_outline(
    std::vector<Vertex>& vertices,
    float left,
    float top,
    float right,
    float bottom,
    float width,
    float height,
    Vec3 color) {
    const float x0 = screen_x_to_ndc(left, width);
    const float x1 = screen_x_to_ndc(right, width);
    const float y0 = screen_y_to_ndc(top, height);
    const float y1 = screen_y_to_ndc(bottom, height);

    vertices.push_back({{x0, y0, 0.0f}, color});
    vertices.push_back({{x1, y0, 0.0f}, color});
    vertices.push_back({{x1, y0, 0.0f}, color});
    vertices.push_back({{x1, y1, 0.0f}, color});
    vertices.push_back({{x1, y1, 0.0f}, color});
    vertices.push_back({{x0, y1, 0.0f}, color});
    vertices.push_back({{x0, y1, 0.0f}, color});
    vertices.push_back({{x0, y0, 0.0f}, color});
}

void append_hud_textured_quad(
    std::vector<Vertex>& vertices,
    float left,
    float top,
    float right,
    float bottom,
    float width,
    float height,
    float u0,
    float v0,
    float u1,
    float v1) {
    const float x0 = screen_x_to_ndc(left, width);
    const float x1 = screen_x_to_ndc(right, width);
    const float y0 = screen_y_to_ndc(top, height);
    const float y1 = screen_y_to_ndc(bottom, height);
    const Vec3 color {1.0f, 1.0f, 1.0f};

    vertices.push_back({{x0, y0, 0.0f}, color, {u0, v0}, 0});
    vertices.push_back({{x0, y1, 0.0f}, color, {u0, v1}, 0});
    vertices.push_back({{x1, y1, 0.0f}, color, {u1, v1}, 0});
    vertices.push_back({{x1, y1, 0.0f}, color, {u1, v1}, 0});
    vertices.push_back({{x1, y0, 0.0f}, color, {u1, v0}, 0});
    vertices.push_back({{x0, y0, 0.0f}, color, {u0, v0}, 0});
}

std::uint8_t segment_mask(char c) {
    switch (c) {
    case '0': return 0b0111111;
    case '1': return 0b0000110;
    case '2': return 0b1011011;
    case '3': return 0b1001111;
    case '4': return 0b1100110;
    case '5': return 0b1101101;
    case '6': return 0b1111101;
    case '7': return 0b0000111;
    case '8': return 0b1111111;
    case '9': return 0b1101111;
    case 'A': return 0b1110111;
    case 'B': return 0b1111100;
    case 'C': return 0b0111001;
    case 'D': return 0b0111110;
    case 'E': return 0b1111001;
    case 'F': return 0b1110001;
    case 'H': return 0b1110110;
    case 'I': return 0b0000110;
    case 'L': return 0b0111000;
    case 'M': return 0b0110111;
    case 'N': return 0b1010100;
    case 'O': return 0b0111111;
    case 'P': return 0b1110011;
    case 'Q': return 0b1100111;
    case 'R': return 0b1110111;
    case 'S': return 0b1101101;
    case 'T': return 0b1111000;
    case 'U': return 0b0111110;
    case 'X': return 0b1110110;
    case 'Y': return 0b1101110;
    case 'Z': return 0b1011011;
    default: return 0;
    }
}

void append_hud_line(
    std::vector<Vertex>& vertices,
    float x0,
    float y0,
    float x1,
    float y1,
    float width,
    float height,
    Vec3 color) {
    vertices.push_back({{screen_x_to_ndc(x0, width), screen_y_to_ndc(y0, height), 0.0f}, color});
    vertices.push_back({{screen_x_to_ndc(x1, width), screen_y_to_ndc(y1, height), 0.0f}, color});
}

void append_debug_char(
    std::vector<Vertex>& vertices,
    char c,
    float x,
    float y,
    float scale,
    float width,
    float height,
    Vec3 color) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const float w = 4.0f * scale;
    const float h = 7.0f * scale;
    const float mid = y + h * 0.5f;
    const std::uint8_t mask = segment_mask(c);

    if ((mask & (1u << 0)) != 0u) append_hud_line(vertices, x, y, x + w, y, width, height, color);
    if ((mask & (1u << 1)) != 0u) append_hud_line(vertices, x + w, y, x + w, mid, width, height, color);
    if ((mask & (1u << 2)) != 0u) append_hud_line(vertices, x + w, mid, x + w, y + h, width, height, color);
    if ((mask & (1u << 3)) != 0u) append_hud_line(vertices, x, y + h, x + w, y + h, width, height, color);
    if ((mask & (1u << 4)) != 0u) append_hud_line(vertices, x, mid, x, y + h, width, height, color);
    if ((mask & (1u << 5)) != 0u) append_hud_line(vertices, x, y, x, mid, width, height, color);
    if ((mask & (1u << 6)) != 0u) append_hud_line(vertices, x, mid, x + w, mid, width, height, color);

    if (c == ':' || c == '.') {
        append_hud_line(vertices, x + w * 0.5f, y + h * 0.3f, x + w * 0.5f, y + h * 0.3f + scale, width, height, color);
        append_hud_line(vertices, x + w * 0.5f, y + h * 0.7f, x + w * 0.5f, y + h * 0.7f + scale, width, height, color);
    } else if (c == '-') {
        append_hud_line(vertices, x, mid, x + w, mid, width, height, color);
    } else if (c == '/') {
        append_hud_line(vertices, x, y + h, x + w, y, width, height, color);
    }
}

void append_debug_text(
    std::vector<Vertex>& vertices,
    const std::string& text,
    float right,
    float top,
    float scale,
    float width,
    float height,
    Vec3 color) {
    const float advance = 6.0f * scale;
    const float text_width = static_cast<float>(text.size()) * advance;
    float x = right - text_width;
    for (char c : text) {
        if (c != ' ') {
            append_debug_char(vertices, c, x, top, scale, width, height, color);
        }
        x += advance;
    }
}

std::array<std::uint8_t, 7> glyph_mask(char c) {
    switch (static_cast<char>(std::toupper(static_cast<unsigned char>(c)))) {
    case 'A': return {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'B': return {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E};
    case 'C': return {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F};
    case 'D': return {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    case 'E': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F};
    case 'F': return {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    case 'G': return {0x0F, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0F};
    case 'H': return {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    case 'I': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F};
    case 'J': return {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0E};
    case 'K': return {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11};
    case 'L': return {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F};
    case 'M': return {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    case 'N': return {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11};
    case 'O': return {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'P': return {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10};
    case 'Q': return {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D};
    case 'R': return {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11};
    case 'S': return {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    case 'T': return {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    case 'U': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E};
    case 'V': return {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04};
    case 'W': return {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    case 'X': return {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11};
    case 'Y': return {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04};
    case 'Z': return {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F};
    case '&': return {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D};
    case '?': return {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};
    default: return {0, 0, 0, 0, 0, 0, 0};
    }
}

void append_rotated_rect(
    std::vector<Vertex>& vertices,
    float left,
    float top,
    float right,
    float bottom,
    float origin_x,
    float origin_y,
    float cos_a,
    float sin_a,
    float width,
    float height,
    Vec3 color) {
    const auto transform = [&](float x, float y) {
        const float dx = x - origin_x;
        const float dy = y - origin_y;
        const float rx = origin_x + dx * cos_a - dy * sin_a;
        const float ry = origin_y + dx * sin_a + dy * cos_a;
        return Vec3 {screen_x_to_ndc(rx, width), screen_y_to_ndc(ry, height), 0.0f};
    };

    const Vec3 p0 = transform(left, top);
    const Vec3 p1 = transform(left, bottom);
    const Vec3 p2 = transform(right, bottom);
    const Vec3 p3 = transform(right, top);
    vertices.push_back({p0, color});
    vertices.push_back({p1, color});
    vertices.push_back({p2, color});
    vertices.push_back({p2, color});
    vertices.push_back({p3, color});
    vertices.push_back({p0, color});
}

float pixel_text_width(const std::string& text, float scale) {
    return static_cast<float>(text.size()) * 6.0f * scale;
}

float ping_pong_offset(float time_seconds, float speed, float travel, float start_offset) {
    if (travel <= 0.0f || speed <= 0.0f) {
        return 0.0f;
    }

    const float cycle_length = travel * 2.0f;
    float position = std::fmod(start_offset + time_seconds * speed, cycle_length);
    if (position < 0.0f) {
        position += cycle_length;
    }

    const float offset = position <= travel ? position : cycle_length - position;
    return clamp(offset, 0.0f, travel);
}

float menu_integer_scale(float width, float height) {
    const float fit_scale = std::min(width / 640.0f, height / 360.0f);
    return std::max(1.0f, std::floor(fit_scale));
}

void append_pixel_text(
    std::vector<Vertex>& vertices,
    const std::string& text,
    float x,
    float y,
    float scale,
    float width,
    float height,
    Vec3 color,
    float rotation_radians = 0.0f) {
    const float cos_a = std::cos(rotation_radians);
    const float sin_a = std::sin(rotation_radians);
    const float origin_x = x + pixel_text_width(text, scale) * 0.5f;
    const float origin_y = y + 7.0f * scale * 0.5f;

    float cursor_x = x;
    for (char c : text) {
        if (c != ' ') {
            const auto rows = glyph_mask(c);
            for (int row = 0; row < 7; ++row) {
                for (int col = 0; col < 5; ++col) {
                    if ((rows[static_cast<std::size_t>(row)] & (1u << (4 - col))) == 0u) {
                        continue;
                    }
                    append_rotated_rect(
                        vertices,
                        cursor_x + static_cast<float>(col) * scale,
                        y + static_cast<float>(row) * scale,
                        cursor_x + static_cast<float>(col + 1) * scale,
                        y + static_cast<float>(row + 1) * scale,
                        origin_x,
                        origin_y,
                        cos_a,
                        sin_a,
                        width,
                        height,
                        color
                    );
                }
            }
        }
        cursor_x += 6.0f * scale;
    }
}

}

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::initialize(const PlatformWindow& window, const std::string& shader_directory) {
    log_message(LogLevel::Info, "Renderer: create_instance");
    if (!create_instance()) {
        log_message(LogLevel::Error, "Renderer: create_instance failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_surface");
    if (!create_surface(window)) {
        log_message(LogLevel::Error, "Renderer: create_surface failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: pick_physical_device");
    if (!pick_physical_device()) {
        log_message(LogLevel::Error, "Renderer: pick_physical_device failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_device");
    if (!create_device()) {
        log_message(LogLevel::Error, "Renderer: create_device failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_swapchain");
    if (!create_swapchain()) {
        log_message(LogLevel::Error, "Renderer: create_swapchain failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_image_views");
    if (!create_image_views()) {
        log_message(LogLevel::Error, "Renderer: create_image_views failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_render_pass");
    if (!create_render_pass()) {
        log_message(LogLevel::Error, "Renderer: create_render_pass failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_pipeline");
    if (!create_pipeline(shader_directory)) {
        log_message(LogLevel::Error, "Renderer: create_pipeline failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_depth_resources");
    if (!create_depth_resources()) {
        log_message(LogLevel::Error, "Renderer: create_depth_resources failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_framebuffers");
    if (!create_framebuffers()) {
        log_message(LogLevel::Error, "Renderer: create_framebuffers failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_command_pool");
    if (!create_command_pool()) {
        log_message(LogLevel::Error, "Renderer: create_command_pool failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: load_textures");
    if (!load_textures()) {
        log_message(LogLevel::Error, "Renderer: load_textures failed");
        return false;
    }
    if (!load_ui_textures()) {
        log_message(LogLevel::Error, "Renderer: load_ui_textures failed");
        return false;
    }
    if (!load_menu_textures()) {
        log_message(LogLevel::Error, "Renderer: load_menu_textures failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_command_buffers");
    if (!create_command_buffers()) {
        log_message(LogLevel::Error, "Renderer: create_command_buffers failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: create_sync_objects");
    if (!create_sync_objects()) {
        log_message(LogLevel::Error, "Renderer: create_sync_objects failed");
        return false;
    }
    log_message(LogLevel::Info, "Renderer: initialized");
    return true;
}

void Renderer::begin_frame(const CameraFrameData& camera) {
    current_camera_ = camera;
    if (!logged_push_constant_size_) {
        log_message(LogLevel::Info, "Renderer: push constant Mat4 size is 64 bytes");
        logged_push_constant_size_ = true;
    }
    recreate_swapchain_if_needed();
    if (dynamic_hud_extent_.width != swapchain_extent_.width || dynamic_hud_extent_.height != swapchain_extent_.height) {
        dynamic_hud_extent_ = swapchain_extent_;
        mark_dynamic_hud_dirty();
    }

    FrameResources& frame = frames_[current_frame_];
    vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);
    retire_deferred_chunk_buffers();

    const VkResult acquire_result = vkAcquireNextImageKHR(
        device_,
        swapchain_,
        UINT64_MAX,
        frame.image_available,
        VK_NULL_HANDLE,
        &current_image_index_
    );

    if (acquire_result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain_if_needed();
        return;
    }

    vkResetFences(device_, 1, &frame.in_flight);
    vkResetCommandBuffer(frame.command_buffer, 0);

    VkCommandBufferBeginInfo begin_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(frame.command_buffer, &begin_info);

    std::array<VkClearValue, 2> clear_values {};
    clear_values[0].color = {{0.52f, 0.75f, 0.94f, 1.0f}};
    clear_values[1].depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo render_pass_info {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    render_pass_info.renderPass = render_pass_;
    render_pass_info.framebuffer = swapchain_framebuffers_[current_image_index_];
    render_pass_info.renderArea.offset = {0, 0};
    render_pass_info.renderArea.extent = swapchain_extent_;
    render_pass_info.clearValueCount = static_cast<std::uint32_t>(clear_values.size());
    render_pass_info.pClearValues = clear_values.data();

    vkCmdBeginRenderPass(frame.command_buffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(frame.command_buffer, 0, 1, &viewport_);
    vkCmdSetScissor(frame.command_buffer, 0, 1, &scissor_);

    frame_started_ = true;
}

void Renderer::upload_chunk_mesh(ChunkCoord coord, const ChunkMesh& mesh) {
    auto existing = chunk_buffers_.find(coord);
    if (existing != chunk_buffers_.end()) {
        defer_destroy_chunk_buffers(std::move(existing->second));
        chunk_buffers_.erase(existing);
    }

    if (mesh.empty()) {
        log_message(LogLevel::Warning, "Renderer: chunk mesh is empty");
        return;
    }

    ChunkRenderData render_data {};
    upload_mesh_section(
        mesh.opaque_mesh,
        render_data.opaque_vertex_buffer,
        render_data.opaque_index_buffer,
        render_data.opaque_index_count
    );
    upload_mesh_section(
        mesh.cutout_mesh,
        render_data.cutout_vertex_buffer,
        render_data.cutout_index_buffer,
        render_data.cutout_index_count
    );
    upload_mesh_section(
        mesh.transparent_mesh,
        render_data.transparent_vertex_buffer,
        render_data.transparent_index_buffer,
        render_data.transparent_index_count
    );
    chunk_buffers_[coord] = render_data;
}

void Renderer::draw_main_menu(float time_seconds, bool use_night_panorama, int hovered_button) {
    if (!frame_started_) {
        return;
    }

    update_main_menu_buffers(time_seconds, use_night_panorama, hovered_button);

    const FrameResources& frame = frames_[current_frame_];
    draw_textured_buffer(
        frame,
        menu_panorama_vertex_buffer_,
        menu_panorama_vertex_count_,
        use_night_panorama ? menu_panorama_night_.descriptor_set : menu_panorama_day_.descriptor_set
    );
    draw_colored_buffer(frame, menu_overlay_vertex_buffer_, menu_overlay_vertex_count_, hotbar_fill_pipeline_);
    draw_textured_buffer(frame, menu_logo_vertex_buffer_, menu_logo_vertex_count_, menu_logo_.descriptor_set);
    draw_textured_buffer(frame, menu_button_vertex_buffer_, menu_button_vertex_count_, menu_button_.descriptor_set);
    draw_textured_buffer(frame, menu_button_highlight_vertex_buffer_, menu_button_highlight_vertex_count_, menu_button_highlighted_.descriptor_set);
    draw_textured_buffer(frame, menu_font_vertex_buffer_, menu_font_vertex_count_, menu_font_.texture.descriptor_set);
    draw_colored_buffer(frame, menu_text_vertex_buffer_, menu_text_vertex_count_, hotbar_fill_pipeline_);
}

void Renderer::unload_chunk_mesh(ChunkCoord coord) {
    auto existing = chunk_buffers_.find(coord);
    if (existing == chunk_buffers_.end()) {
        return;
    }

    defer_destroy_chunk_buffers(std::move(existing->second));
    chunk_buffers_.erase(existing);
}

void Renderer::draw_visible_chunks(std::span<const ActiveChunk> visible_chunks) {
    if (!frame_started_) {
        return;
    }

    const FrameResources& frame = frames_[current_frame_];
    const bool use_wireframe = wireframe_enabled_ && wireframe_supported_ && wireframe_pipeline_ != VK_NULL_HANDLE;
    const bool draw_textured_fill = !use_wireframe || wireframe_textures_enabled_;

    std::vector<ActiveChunk> render_chunks;
    render_chunks.reserve(visible_chunks.size());
    for (const ActiveChunk& chunk : visible_chunks) {
        auto it = chunk_buffers_.find(chunk.coord);
        if (it == chunk_buffers_.end()) {
            continue;
        }
        if (!aabb_visible_in_current_frustum(chunk_bounds(chunk.coord))) {
            continue;
        }
        render_chunks.push_back(chunk);
    }
    last_drawn_chunks_ = render_chunks.size();
    debug_hud_data_.drawn_chunks = last_drawn_chunks_;

    const auto draw_chunks_with_pipeline = [&](VkPipeline pipeline, VkPipelineLayout layout, bool bind_textures, auto section_getter) {
        vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        if (bind_textures && descriptor_set_ != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &descriptor_set_, 0, nullptr);
        }

        for (const ActiveChunk& chunk : render_chunks) {
            auto it = chunk_buffers_.find(chunk.coord);
            if (it == chunk_buffers_.end()) {
                continue;
            }
            const auto section = section_getter(it->second);
            if (section.index_count == 0 || section.vertex_buffer == VK_NULL_HANDLE || section.index_buffer == VK_NULL_HANDLE) {
                continue;
            }
            const Mat4 chunk_matrix = chunk_view_proj(chunk.coord);
            vkCmdPushConstants(
                frame.command_buffer,
                layout,
                VK_SHADER_STAGE_VERTEX_BIT,
                0,
                sizeof(Mat4),
                chunk_matrix.m.data()
            );

            const VkBuffer vertex_buffers[] = {section.vertex_buffer};
            const VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
            vkCmdBindIndexBuffer(frame.command_buffer, section.index_buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(frame.command_buffer, section.index_count, 1, 0, 0, 0);
        }
    };

    if (draw_textured_fill) {
        draw_chunks_with_pipeline(fill_pipeline_, pipeline_layout_, true, [](const ChunkRenderData& chunk) {
            return DrawSectionView {chunk.opaque_vertex_buffer.buffer, chunk.opaque_index_buffer.buffer, chunk.opaque_index_count};
        });
        draw_chunks_with_pipeline(cutout_pipeline_, pipeline_layout_, true, [](const ChunkRenderData& chunk) {
            return DrawSectionView {chunk.cutout_vertex_buffer.buffer, chunk.cutout_index_buffer.buffer, chunk.cutout_index_count};
        });
        draw_chunks_with_pipeline(water_pipeline_, pipeline_layout_, true, [](const ChunkRenderData& chunk) {
            return DrawSectionView {chunk.transparent_vertex_buffer.buffer, chunk.transparent_index_buffer.buffer, chunk.transparent_index_count};
        });
    }
    if (use_wireframe) {
        draw_chunks_with_pipeline(wireframe_pipeline_, hud_pipeline_layout_, false, [](const ChunkRenderData& chunk) {
            return DrawSectionView {chunk.opaque_vertex_buffer.buffer, chunk.opaque_index_buffer.buffer, chunk.opaque_index_count};
        });
        draw_chunks_with_pipeline(wireframe_pipeline_, hud_pipeline_layout_, false, [](const ChunkRenderData& chunk) {
            return DrawSectionView {chunk.cutout_vertex_buffer.buffer, chunk.cutout_index_buffer.buffer, chunk.cutout_index_count};
        });
        draw_chunks_with_pipeline(wireframe_pipeline_, hud_pipeline_layout_, false, [](const ChunkRenderData& chunk) {
            return DrawSectionView {chunk.transparent_vertex_buffer.buffer, chunk.transparent_index_buffer.buffer, chunk.transparent_index_count};
        });
    }

    if (wireframe_enabled_) {
        update_chunk_outline_buffer(render_chunks);
    } else {
        chunk_outline_vertex_count_ = 0;
    }
    if (target_block_dirty_) {
        update_target_block_outline_buffer();
        target_block_dirty_ = false;
    }
    if (hotbar_dirty_) {
        update_hotbar_buffer();
        hotbar_dirty_ = false;
    }
    if (crosshair_dirty_) {
        update_crosshair_buffer();
        crosshair_dirty_ = false;
    }
    if (debug_hud_dirty_) {
        update_debug_hud_buffer();
        debug_hud_dirty_ = false;
    }
    draw_chunk_outlines(frame);
    draw_target_block_outline(frame);
    draw_hotbar(frame);
    draw_crosshair(frame);
    draw_debug_hud(frame);
}

void Renderer::end_frame() {
    if (!frame_started_) {
        return;
    }

    FrameResources& frame = frames_[current_frame_];
    vkCmdEndRenderPass(frame.command_buffer);
    vkEndCommandBuffer(frame.command_buffer);

    const VkPipelineStageFlags wait_stages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

    VkSubmitInfo submit_info {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &frame.image_available;
    submit_info.pWaitDstStageMask = wait_stages;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &frame.command_buffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &frame.render_finished;

    vkQueueSubmit(graphics_queue_, 1, &submit_info, frame.in_flight);

    VkPresentInfoKHR present_info {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &frame.render_finished;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain_;
    present_info.pImageIndices = &current_image_index_;
    vkQueuePresentKHR(present_queue_, &present_info);

    current_frame_ = (current_frame_ + 1) % frames_.size();
    frame_started_ = false;
}

void Renderer::shutdown() {
    if (device_ == VK_NULL_HANDLE) {
        return;
    }

    vkDeviceWaitIdle(device_);

    destroy_textures();

    for (auto& [coord, render_data] : chunk_buffers_) {
        (void)coord;
        destroy_buffer(render_data.opaque_vertex_buffer);
        destroy_buffer(render_data.opaque_index_buffer);
        destroy_buffer(render_data.cutout_vertex_buffer);
        destroy_buffer(render_data.cutout_index_buffer);
        destroy_buffer(render_data.transparent_vertex_buffer);
        destroy_buffer(render_data.transparent_index_buffer);
    }
    chunk_buffers_.clear();
    destroy_deferred_chunk_buffers_immediate();
    destroy_buffer(chunk_outline_vertex_buffer_);
    destroy_buffer(target_block_outline_vertex_buffer_);
    destroy_buffer(hotbar_fill_vertex_buffer_);
    destroy_buffer(hotbar_outline_vertex_buffer_);
    destroy_buffer(hotbar_texture_vertex_buffer_);
    destroy_buffer(crosshair_vertex_buffer_);
    destroy_buffer(debug_hud_vertex_buffer_);
    destroy_buffer(menu_panorama_vertex_buffer_);
    destroy_buffer(menu_logo_vertex_buffer_);
    destroy_buffer(menu_button_vertex_buffer_);
    destroy_buffer(menu_button_highlight_vertex_buffer_);
    destroy_buffer(menu_overlay_vertex_buffer_);
    destroy_buffer(menu_text_vertex_buffer_);
    destroy_buffer(menu_font_vertex_buffer_);

    for (auto& frame : frames_) {
        if (frame.image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frame.image_available, nullptr);
        }
        if (frame.render_finished != VK_NULL_HANDLE) {
            vkDestroySemaphore(device_, frame.render_finished, nullptr);
        }
        if (frame.in_flight != VK_NULL_HANDLE) {
            vkDestroyFence(device_, frame.in_flight, nullptr);
        }
    }
    frames_.clear();

    if (command_pool_ != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device_, command_pool_, nullptr);
    }

    destroy_swapchain_objects();

    if (chunk_outline_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, chunk_outline_pipeline_, nullptr);
    }
    if (block_outline_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, block_outline_pipeline_, nullptr);
    }
    if (hotbar_fill_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, hotbar_fill_pipeline_, nullptr);
    }
    if (hotbar_outline_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, hotbar_outline_pipeline_, nullptr);
    }
    if (hotbar_texture_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, hotbar_texture_pipeline_, nullptr);
    }
    if (crosshair_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, crosshair_pipeline_, nullptr);
    }
    if (wireframe_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, wireframe_pipeline_, nullptr);
    }
    if (water_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, water_pipeline_, nullptr);
    }
    if (cutout_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, cutout_pipeline_, nullptr);
    }
    if (fill_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, fill_pipeline_, nullptr);
    }
    if (pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    }
    if (hud_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, hud_pipeline_layout_, nullptr);
    }
    if (ui_pipeline_layout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, ui_pipeline_layout_, nullptr);
    }
    if (render_pass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, render_pass_, nullptr);
    }
    if (device_ != VK_NULL_HANDLE) {
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_ != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }

    device_ = VK_NULL_HANDLE;
}

void Renderer::toggle_wireframe() {
    if (!wireframe_supported_) {
        log_message(LogLevel::Warning, "Renderer: wireframe mode is not supported on this device");
        return;
    }

    wireframe_enabled_ = !wireframe_enabled_;
    log_message(LogLevel::Info, wireframe_enabled_ ? "Renderer: wireframe enabled" : "Renderer: wireframe disabled");
}

void Renderer::toggle_wireframe_textures() {
    wireframe_textures_enabled_ = !wireframe_textures_enabled_;
    log_message(
        LogLevel::Info,
        wireframe_textures_enabled_
            ? "Renderer: wireframe texture overlay enabled"
            : "Renderer: wireframe texture overlay disabled"
    );
}

bool Renderer::wireframe_enabled() const {
    return wireframe_enabled_;
}

void Renderer::set_target_block(const std::optional<BlockHit>& target_block) {
    if (!same_block_hit(target_block_, target_block)) {
        target_block_dirty_ = true;
    }
    target_block_ = target_block;
}

void Renderer::set_hotbar_state(std::size_t selected_slot, std::size_t slot_count) {
    const std::size_t next_slot_count = std::max<std::size_t>(1, slot_count);
    const std::size_t next_selected_slot = std::min(selected_slot, next_slot_count - 1);
    if (hotbar_slot_count_ != next_slot_count || hotbar_selected_slot_ != next_selected_slot) {
        hotbar_dirty_ = true;
    }
    hotbar_slot_count_ = next_slot_count;
    hotbar_selected_slot_ = next_selected_slot;
}

void Renderer::set_debug_hud(bool enabled, const DebugHudData& data) {
    if (debug_hud_enabled_ != enabled ||
        debug_hud_data_.fps != data.fps ||
        debug_hud_data_.position.x != data.position.x ||
        debug_hud_data_.position.y != data.position.y ||
        debug_hud_data_.position.z != data.position.z ||
        debug_hud_data_.debug_fly != data.debug_fly ||
        debug_hud_data_.visible_chunks != data.visible_chunks ||
        debug_hud_data_.pending_uploads != data.pending_uploads ||
        debug_hud_data_.uploads_this_frame != data.uploads_this_frame ||
        debug_hud_data_.queued_rebuilds != data.queued_rebuilds ||
        debug_hud_data_.drawn_chunks != data.drawn_chunks ||
        debug_hud_data_.fancy_leaves != data.fancy_leaves) {
        debug_hud_dirty_ = true;
    }
    debug_hud_enabled_ = enabled;
    debug_hud_data_ = data;
}

bool Renderer::create_instance() {
    std::uint32_t extension_count = 0;
    const char* const* required_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (required_extensions == nullptr) {
        log_message(LogLevel::Error, "SDL_Vulkan_GetInstanceExtensions failed");
        return false;
    }

    std::vector<const char*> extensions(required_extensions, required_extensions + extension_count);
#ifndef NDEBUG
    extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

    VkApplicationInfo app_info {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "minecraft_legacy";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "minecraft_legacy";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_3;

    std::vector<const char*> layers {};
#ifndef NDEBUG
    layers.push_back("VK_LAYER_KHRONOS_validation");
#endif

    VkInstanceCreateInfo create_info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    create_info.ppEnabledExtensionNames = extensions.data();
    create_info.enabledLayerCount = static_cast<std::uint32_t>(layers.size());
    create_info.ppEnabledLayerNames = layers.data();

    return vkCreateInstance(&create_info, nullptr, &instance_) == VK_SUCCESS;
}

bool Renderer::create_surface(const PlatformWindow& window) {
    return SDL_Vulkan_CreateSurface(window.handle, instance_, nullptr, &surface_);
}

bool Renderer::pick_physical_device() {
    std::uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (count == 0) {
        return false;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    for (VkPhysicalDevice device : devices) {
        const QueueFamilySelection queues = find_queue_families(device, surface_);
        if (queues.graphics_family.has_value() && queues.present_family.has_value()) {
            physical_device_ = device;
            graphics_family_index_ = queues.graphics_family.value();

            VkPhysicalDeviceFeatures features {};
            vkGetPhysicalDeviceFeatures(physical_device_, &features);
            wireframe_supported_ = features.fillModeNonSolid == VK_TRUE;
            wide_lines_supported_ = features.wideLines == VK_TRUE;
            if (!logged_wireframe_support_) {
                log_message(LogLevel::Info, wireframe_supported_
                    ? "Renderer: fillModeNonSolid is supported"
                    : "Renderer: fillModeNonSolid is not supported, wireframe disabled");
                log_message(LogLevel::Info, wide_lines_supported_
                    ? "Renderer: wideLines is supported"
                    : "Renderer: wideLines is not supported, wireframe line width stays 1px");
                logged_wireframe_support_ = true;
            }
            return true;
        }
    }
    return false;
}

bool Renderer::create_device() {
    const QueueFamilySelection queues = find_queue_families(physical_device_, surface_);
    std::set<std::uint32_t> unique_indices = {queues.graphics_family.value(), queues.present_family.value()};
    const float priority = 1.0f;

    std::vector<VkDeviceQueueCreateInfo> queue_infos;
    for (std::uint32_t index : unique_indices) {
        VkDeviceQueueCreateInfo queue_info {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = index;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        queue_infos.push_back(queue_info);
    }

    VkPhysicalDeviceFeatures available_features {};
    vkGetPhysicalDeviceFeatures(physical_device_, &available_features);

    VkPhysicalDeviceFeatures enabled_features {};
    enabled_features.fillModeNonSolid = wireframe_supported_ ? VK_TRUE : VK_FALSE;
    enabled_features.wideLines = wide_lines_supported_ ? VK_TRUE : VK_FALSE;

    const std::array device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

    VkDeviceCreateInfo create_info {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    create_info.queueCreateInfoCount = static_cast<std::uint32_t>(queue_infos.size());
    create_info.pQueueCreateInfos = queue_infos.data();
    create_info.pEnabledFeatures = &enabled_features;
    create_info.enabledExtensionCount = static_cast<std::uint32_t>(device_extensions.size());
    create_info.ppEnabledExtensionNames = device_extensions.data();

    if (vkCreateDevice(physical_device_, &create_info, nullptr, &device_) != VK_SUCCESS) {
        return false;
    }

    vkGetDeviceQueue(device_, queues.graphics_family.value(), 0, &graphics_queue_);
    vkGetDeviceQueue(device_, queues.present_family.value(), 0, &present_queue_);
    return true;
}

bool Renderer::create_swapchain() {
    VkSurfaceCapabilitiesKHR capabilities {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &capabilities);

    std::uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &format_count, formats.data());

    std::uint32_t present_mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &present_mode_count, nullptr);
    std::vector<VkPresentModeKHR> present_modes(present_mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &present_mode_count, present_modes.data());

    const VkSurfaceFormatKHR surface_format = choose_surface_format(formats);
    const VkPresentModeKHR present_mode = choose_present_mode(present_modes);
    const VkExtent2D extent = choose_extent(capabilities);

    std::uint32_t image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
        image_count = capabilities.maxImageCount;
    }
    // Fix: tie swapchain image count to frames in flight to avoid semaphore reuse validation errors
    if (image_count < kMaxFramesInFlight) {
        image_count = kMaxFramesInFlight;
    } else if (image_count > kMaxFramesInFlight) {
        image_count = kMaxFramesInFlight;
    }

    const QueueFamilySelection queues = find_queue_families(physical_device_, surface_);
    const std::uint32_t indices[] = {queues.graphics_family.value(), queues.present_family.value()};

    VkSwapchainCreateInfoKHR create_info {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    create_info.surface = surface_;
    create_info.minImageCount = image_count;
    create_info.imageFormat = surface_format.format;
    create_info.imageColorSpace = surface_format.colorSpace;
    create_info.imageExtent = extent;
    create_info.imageArrayLayers = 1;
    create_info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (queues.graphics_family != queues.present_family) {
        create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        create_info.queueFamilyIndexCount = 2;
        create_info.pQueueFamilyIndices = indices;
    } else {
        create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    create_info.preTransform = capabilities.currentTransform;
    create_info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    create_info.presentMode = present_mode;
    create_info.clipped = VK_TRUE;

    if (vkCreateSwapchainKHR(device_, &create_info, nullptr, &swapchain_) != VK_SUCCESS) {
        return false;
    }

    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    swapchain_images_.resize(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, swapchain_images_.data());

    swapchain_format_ = surface_format.format;
    swapchain_extent_ = extent;

    viewport_ = {
        0.0f,
        0.0f,
        static_cast<float>(swapchain_extent_.width),
        static_cast<float>(swapchain_extent_.height),
        0.0f,
        1.0f
    };
    scissor_.offset = {0, 0};
    scissor_.extent = swapchain_extent_;

    return true;
}

bool Renderer::create_image_views() {
    swapchain_image_views_.resize(swapchain_images_.size());
    for (std::size_t i = 0; i < swapchain_images_.size(); ++i) {
        VkImageViewCreateInfo create_info {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        create_info.image = swapchain_images_[i];
        create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        create_info.format = swapchain_format_;
        create_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        create_info.subresourceRange.levelCount = 1;
        create_info.subresourceRange.layerCount = 1;
        if (vkCreateImageView(device_, &create_info, nullptr, &swapchain_image_views_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool Renderer::create_render_pass() {
    VkAttachmentDescription color_attachment {};
    color_attachment.format = swapchain_format_;
    color_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color_attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depth_attachment {};
    depth_attachment.format = depth_format_;
    depth_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth_attachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference color_ref {};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depth_ref {};
    depth_ref.attachment = 1;
    depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;

    std::array attachments = {color_attachment, depth_attachment};
    VkRenderPassCreateInfo create_info {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    create_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
    create_info.pAttachments = attachments.data();
    create_info.subpassCount = 1;
    create_info.pSubpasses = &subpass;

    return vkCreateRenderPass(device_, &create_info, nullptr, &render_pass_) == VK_SUCCESS;
}

bool Renderer::create_pipeline(const std::string& shader_directory) {
    const auto vertex_code = read_binary_file(shader_directory + "/voxel.vert.spv");
    const auto fragment_code = read_binary_file(shader_directory + "/voxel.frag.spv");
    const auto water_fragment_code = read_binary_file(shader_directory + "/water.frag.spv");
    const auto hud_vertex_code = read_binary_file(shader_directory + "/hud.vert.spv");
    const auto hud_textured_vertex_code = read_binary_file(shader_directory + "/hud_textured.vert.spv");
    const auto hud_textured_fragment_code = read_binary_file(shader_directory + "/hud_textured.frag.spv");
    const auto color_fragment_code = read_binary_file(shader_directory + "/color.frag.spv");
    const auto wireframe_debug_fragment_code = read_binary_file(shader_directory + "/wireframe_debug.frag.spv");
    if (vertex_code.empty() || fragment_code.empty() || water_fragment_code.empty() || hud_vertex_code.empty() ||
        hud_textured_vertex_code.empty() || hud_textured_fragment_code.empty() || color_fragment_code.empty() ||
        wireframe_debug_fragment_code.empty()) {
        return false;
    }

    VkPushConstantRange push_constant {};
    push_constant.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_constant.offset = 0;
    push_constant.size = sizeof(Mat4);

    VkDescriptorSetLayoutBinding sampler_binding {};
    sampler_binding.binding = 0;
    sampler_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler_binding.descriptorCount = 1;
    sampler_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layout_create_info {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layout_create_info.bindingCount = 1;
    layout_create_info.pBindings = &sampler_binding;
    if (vkCreateDescriptorSetLayout(device_, &layout_create_info, nullptr, &descriptor_set_layout_) != VK_SUCCESS) {
        return false;
    }
    if (vkCreateDescriptorSetLayout(device_, &layout_create_info, nullptr, &ui_descriptor_set_layout_) != VK_SUCCESS) {
        return false;
    }

    VkPipelineLayoutCreateInfo layout_info {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push_constant;
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &descriptor_set_layout_;
    if (vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
        return false;
    }

    // color.frag doesn't need descriptor sets, so hud_layout_info remains empty
    VkPipelineLayoutCreateInfo hud_layout_info {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    hud_layout_info.pushConstantRangeCount = 1;
    hud_layout_info.pPushConstantRanges = &push_constant;
    if (vkCreatePipelineLayout(device_, &hud_layout_info, nullptr, &hud_pipeline_layout_) != VK_SUCCESS) {
        return false;
    }

    VkPipelineLayoutCreateInfo ui_layout_info {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ui_layout_info.setLayoutCount = 1;
    ui_layout_info.pSetLayouts = &ui_descriptor_set_layout_;
    if (vkCreatePipelineLayout(device_, &ui_layout_info, nullptr, &ui_pipeline_layout_) != VK_SUCCESS) {
        return false;
    }

    if (!create_graphics_pipeline(
            vertex_code,
            fragment_code,
            pipeline_layout_,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_POLYGON_MODE_FILL,
            debug_disable_culling ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT,
            true,
            true,
            false,
            &fill_pipeline_)) {
        return false;
    }

    if (!create_graphics_pipeline(
            vertex_code,
            fragment_code,
            pipeline_layout_,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_POLYGON_MODE_FILL,
            debug_disable_culling ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT,
            true,
            true,
            false,
            &cutout_pipeline_)) {
        return false;
    }

    if (!create_graphics_pipeline(
            vertex_code,
            water_fragment_code,
            pipeline_layout_,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_POLYGON_MODE_FILL,
            VK_CULL_MODE_NONE,
            true,
            false,
            true,
            &water_pipeline_)) {
        return false;
    }

    if (wireframe_supported_) {
        if (!create_graphics_pipeline(
                vertex_code,
                wireframe_debug_fragment_code,
                hud_pipeline_layout_,
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                VK_POLYGON_MODE_LINE,
                VK_CULL_MODE_NONE,
                true,
                false,
                false,
                &wireframe_pipeline_)) {
            return false;
        }
    }

    if (!create_graphics_pipeline(
            vertex_code,
            color_fragment_code,
            hud_pipeline_layout_, // Changed to hud_pipeline_layout
            VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
            VK_POLYGON_MODE_FILL,
            VK_CULL_MODE_NONE,
            true,
            false,
            false,
            &chunk_outline_pipeline_)) {
        return false;
    }

    if (!create_graphics_pipeline(
            vertex_code,
            color_fragment_code,
            hud_pipeline_layout_, // Changed to hud_pipeline_layout
            VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
            VK_POLYGON_MODE_FILL,
            VK_CULL_MODE_NONE,
            true,
            false,
            false,
            &block_outline_pipeline_)) {
        return false;
    }

    if (!create_graphics_pipeline(
            hud_vertex_code,
            color_fragment_code,
            hud_pipeline_layout_,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_POLYGON_MODE_FILL,
            VK_CULL_MODE_NONE,
            false,
            false,
            false,
            &hotbar_fill_pipeline_)) {
        return false;
    }

    if (!create_graphics_pipeline(
            hud_vertex_code,
            color_fragment_code,
            hud_pipeline_layout_,
            VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
            VK_POLYGON_MODE_FILL,
            VK_CULL_MODE_NONE,
            false,
            false,
            false,
            &hotbar_outline_pipeline_)) {
        return false;
    }

    if (!create_graphics_pipeline(
            hud_textured_vertex_code,
            hud_textured_fragment_code,
            ui_pipeline_layout_,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_POLYGON_MODE_FILL,
            VK_CULL_MODE_NONE,
            false,
            false,
            true,
            &hotbar_texture_pipeline_)) {
        return false;
    }

    if (!create_graphics_pipeline(
            hud_vertex_code,
            color_fragment_code,
            hud_pipeline_layout_,
            VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
            VK_POLYGON_MODE_FILL,
            VK_CULL_MODE_NONE,
            false,
            false,
            false,
            &crosshair_pipeline_)) {
        return false;
    }

    return true;
}

bool Renderer::create_graphics_pipeline(
    const std::vector<char>& vertex_code,
    const std::vector<char>& fragment_code,
    VkPipelineLayout layout,
    VkPrimitiveTopology topology,
    VkPolygonMode polygon_mode,
    VkCullModeFlags cull_mode,
    bool depth_test,
    bool depth_write,
    bool alpha_blend,
    VkPipeline* output_pipeline) {
    const VkShaderModule vertex_module = create_shader_module(vertex_code);
    const VkShaderModule fragment_module = create_shader_module(fragment_code);

    VkPipelineShaderStageCreateInfo vertex_stage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertex_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertex_stage.module = vertex_module;
    vertex_stage.pName = "main";

    VkPipelineShaderStageCreateInfo fragment_stage {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragment_stage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragment_stage.module = fragment_module;
    fragment_stage.pName = "main";

    std::array shader_stages = {vertex_stage, fragment_stage};

    std::array<VkVertexInputBindingDescription, 1> bindings {{
        {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}
    }};
    std::array<VkVertexInputAttributeDescription, 4> attributes {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, color))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, static_cast<std::uint32_t>(offsetof(Vertex, uv))},
        {3, 0, VK_FORMAT_R32_UINT, static_cast<std::uint32_t>(offsetof(Vertex, texture_index))}
    }};

    VkPipelineVertexInputStateCreateInfo vertex_input {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = static_cast<std::uint32_t>(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = topology;

    VkPipelineViewportStateCreateInfo viewport_state {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = polygon_mode;
    rasterizer.lineWidth = (polygon_mode == VK_POLYGON_MODE_LINE && wide_lines_supported_) ? 2.0f : 1.0f;
    rasterizer.cullMode = cull_mode;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth_stencil {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = depth_test ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_ALWAYS;

    VkPipelineColorBlendAttachmentState color_blend_attachment {};
    color_blend_attachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    if (alpha_blend) {
        color_blend_attachment.blendEnable = VK_TRUE;
        color_blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        color_blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        color_blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        color_blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        color_blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo color_blending {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blending.attachmentCount = 1;
    color_blending.pAttachments = &color_blend_attachment;

    std::array dynamic_states = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic_state {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic_state.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic_state.pDynamicStates = dynamic_states.data();

    VkGraphicsPipelineCreateInfo pipeline_info {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline_info.stageCount = static_cast<std::uint32_t>(shader_stages.size());
    pipeline_info.pStages = shader_stages.data();
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterizer;
    pipeline_info.pMultisampleState = &multisampling;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &color_blending;
    pipeline_info.pDynamicState = &dynamic_state;
    pipeline_info.layout = layout;
    pipeline_info.renderPass = render_pass_;
    pipeline_info.subpass = 0;

    const bool success = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, output_pipeline) == VK_SUCCESS;
    vkDestroyShaderModule(device_, vertex_module, nullptr);
    vkDestroyShaderModule(device_, fragment_module, nullptr);
    return success;
}

bool Renderer::create_depth_resources() {
    VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = swapchain_extent_.width;
    image_info.extent.height = swapchain_extent_.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = depth_format_;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &image_info, nullptr, &depth_image_) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements requirements {};
    vkGetImageMemoryRequirements(device_, depth_image_, &requirements);

    VkMemoryAllocateInfo alloc_info {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &depth_memory_) != VK_SUCCESS) {
        return false;
    }

    vkBindImageMemory(device_, depth_image_, depth_memory_, 0);

    VkImageViewCreateInfo view_info {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = depth_image_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = depth_format_;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    return vkCreateImageView(device_, &view_info, nullptr, &depth_view_) == VK_SUCCESS;
}

bool Renderer::create_framebuffers() {
    swapchain_framebuffers_.resize(swapchain_image_views_.size());
    for (std::size_t i = 0; i < swapchain_image_views_.size(); ++i) {
        std::array attachments = {swapchain_image_views_[i], depth_view_};
        VkFramebufferCreateInfo create_info {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        create_info.renderPass = render_pass_;
        create_info.attachmentCount = static_cast<std::uint32_t>(attachments.size());
        create_info.pAttachments = attachments.data();
        create_info.width = swapchain_extent_.width;
        create_info.height = swapchain_extent_.height;
        create_info.layers = 1;

        if (vkCreateFramebuffer(device_, &create_info, nullptr, &swapchain_framebuffers_[i]) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

bool Renderer::create_command_pool() {
    VkCommandPoolCreateInfo create_info {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    create_info.queueFamilyIndex = graphics_family_index_;
    return vkCreateCommandPool(device_, &create_info, nullptr, &command_pool_) == VK_SUCCESS;
}

bool Renderer::create_command_buffers() {
    frames_.resize(swapchain_images_.size());
    std::vector<VkCommandBuffer> buffers(frames_.size());

    VkCommandBufferAllocateInfo alloc_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = static_cast<std::uint32_t>(buffers.size());

    if (vkAllocateCommandBuffers(device_, &alloc_info, buffers.data()) != VK_SUCCESS) {
        return false;
    }

    for (std::size_t i = 0; i < frames_.size(); ++i) {
        frames_[i].command_buffer = buffers[i];
    }
    return true;
}

bool Renderer::create_sync_objects() {
    VkSemaphoreCreateInfo semaphore_info {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (FrameResources& frame : frames_) {
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.image_available) != VK_SUCCESS) {
            return false;
        }
        if (vkCreateSemaphore(device_, &semaphore_info, nullptr, &frame.render_finished) != VK_SUCCESS) {
            return false;
        }
        if (vkCreateFence(device_, &fence_info, nullptr, &frame.in_flight) != VK_SUCCESS) {
            return false;
        }
    }
    return true;
}

void Renderer::destroy_swapchain_objects() {
    for (VkFramebuffer framebuffer : swapchain_framebuffers_) {
        vkDestroyFramebuffer(device_, framebuffer, nullptr);
    }
    swapchain_framebuffers_.clear();

    if (depth_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, depth_view_, nullptr);
        depth_view_ = VK_NULL_HANDLE;
    }
    if (depth_image_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, depth_image_, nullptr);
        depth_image_ = VK_NULL_HANDLE;
    }
    if (depth_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, depth_memory_, nullptr);
        depth_memory_ = VK_NULL_HANDLE;
    }

    for (VkImageView view : swapchain_image_views_) {
        vkDestroyImageView(device_, view, nullptr);
    }
    swapchain_image_views_.clear();

    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

bool Renderer::recreate_swapchain_if_needed() {
    if (swapchain_extent_.width == 0 || swapchain_extent_.height == 0) {
        return false;
    }
    return true;
}

void Renderer::defer_destroy_chunk_buffers(ChunkRenderData&& render_data) {
    if (render_data.opaque_vertex_buffer.buffer == VK_NULL_HANDLE &&
        render_data.opaque_index_buffer.buffer == VK_NULL_HANDLE &&
        render_data.cutout_vertex_buffer.buffer == VK_NULL_HANDLE &&
        render_data.cutout_index_buffer.buffer == VK_NULL_HANDLE &&
        render_data.transparent_vertex_buffer.buffer == VK_NULL_HANDLE &&
        render_data.transparent_index_buffer.buffer == VK_NULL_HANDLE) {
        return;
    }

    deferred_chunk_buffers_.push_back({
        render_data.opaque_vertex_buffer,
        render_data.opaque_index_buffer,
        render_data.cutout_vertex_buffer,
        render_data.cutout_index_buffer,
        render_data.transparent_vertex_buffer,
        render_data.transparent_index_buffer,
        static_cast<std::uint32_t>(frames_.size() + 1)
    });
}

void Renderer::retire_deferred_chunk_buffers() {
    for (auto it = deferred_chunk_buffers_.begin(); it != deferred_chunk_buffers_.end();) {
        if (it->frames_remaining > 0) {
            --it->frames_remaining;
        }
        if (it->frames_remaining == 0) {
            destroy_buffer(it->opaque_vertex_buffer);
            destroy_buffer(it->opaque_index_buffer);
            destroy_buffer(it->cutout_vertex_buffer);
            destroy_buffer(it->cutout_index_buffer);
            destroy_buffer(it->transparent_vertex_buffer);
            destroy_buffer(it->transparent_index_buffer);
            it = deferred_chunk_buffers_.erase(it);
        } else {
            ++it;
        }
    }
}

void Renderer::destroy_deferred_chunk_buffers_immediate() {
    for (DeferredChunkBuffers& buffers : deferred_chunk_buffers_) {
        destroy_buffer(buffers.opaque_vertex_buffer);
        destroy_buffer(buffers.opaque_index_buffer);
        destroy_buffer(buffers.cutout_vertex_buffer);
        destroy_buffer(buffers.cutout_index_buffer);
        destroy_buffer(buffers.transparent_vertex_buffer);
        destroy_buffer(buffers.transparent_index_buffer);
    }
    deferred_chunk_buffers_.clear();
}

void Renderer::upload_mesh_section(const MeshSection& mesh, GpuBuffer& vertex_buffer, GpuBuffer& index_buffer, std::uint32_t& index_count) {
    if (mesh.empty()) {
        index_count = 0;
        return;
    }

    const VkDeviceSize vertex_size = sizeof(Vertex) * mesh.vertices.size();
    const VkDeviceSize index_size = sizeof(std::uint32_t) * mesh.indices.size();

    vertex_buffer = create_buffer(
        vertex_size,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    index_buffer = create_buffer(
        index_size,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    void* mapped = nullptr;
    vkMapMemory(device_, vertex_buffer.memory, 0, vertex_size, 0, &mapped);
    std::memcpy(mapped, mesh.vertices.data(), static_cast<std::size_t>(vertex_size));
    vkUnmapMemory(device_, vertex_buffer.memory);

    vkMapMemory(device_, index_buffer.memory, 0, index_size, 0, &mapped);
    std::memcpy(mapped, mesh.indices.data(), static_cast<std::size_t>(index_size));
    vkUnmapMemory(device_, index_buffer.memory);

    index_count = static_cast<std::uint32_t>(mesh.indices.size());
}

Renderer::GpuBuffer Renderer::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties) {
    GpuBuffer result {};
    result.size = size;

    VkBufferCreateInfo create_info {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    create_info.size = size;
    create_info.usage = usage;
    create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateBuffer(device_, &create_info, nullptr, &result.buffer);

    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(device_, result.buffer, &requirements);

    VkMemoryAllocateInfo alloc_info {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(requirements.memoryTypeBits, properties);
    vkAllocateMemory(device_, &alloc_info, nullptr, &result.memory);
    vkBindBufferMemory(device_, result.buffer, result.memory, 0);
    return result;
}

void Renderer::destroy_buffer(GpuBuffer& buffer) {
    if (buffer.buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_, buffer.buffer, nullptr);
        buffer.buffer = VK_NULL_HANDLE;
    }
    if (buffer.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, buffer.memory, nullptr);
        buffer.memory = VK_NULL_HANDLE;
    }
    buffer.size = 0;
}

std::uint32_t Renderer::find_memory_type(std::uint32_t type_filter, VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties);

    for (std::uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((type_filter & (1u << i)) != 0u &&
            (memory_properties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0;
}

VkShaderModule Renderer::create_shader_module(const std::vector<char>& code) const {
    VkShaderModuleCreateInfo create_info {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    create_info.codeSize = code.size();
    create_info.pCode = reinterpret_cast<const std::uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &create_info, nullptr, &module);
    return module;
}

std::vector<char> Renderer::read_binary_file(const std::string& path) const {
#ifdef _WIN32
    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wide_path(static_cast<std::size_t>(wide_length > 0 ? wide_length : 0), L'\0');
    if (wide_length > 0) {
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide_path.data(), wide_length);
        if (!wide_path.empty()) {
            wide_path.pop_back();
        }
    }
    std::ifstream file(std::filesystem::path(wide_path), std::ios::binary | std::ios::ate);
#else
    std::ifstream file(std::filesystem::path(path), std::ios::binary | std::ios::ate);
#endif
    if (!file.is_open()) {
        log_message(LogLevel::Error, "Renderer: failed to open shader file");
        return {};
    }
    const std::streamsize size = file.tellg();
    file.seekg(0);

    std::vector<char> buffer(static_cast<std::size_t>(size));
    file.read(buffer.data(), size);
    return buffer;
}

Aabb Renderer::chunk_bounds(ChunkCoord coord) const {
    const float min_x = static_cast<float>(coord.x * kChunkWidth);
    const float min_z = static_cast<float>(coord.z * kChunkDepth);
    return {
        {min_x, 0.0f, min_z},
        {
            min_x + static_cast<float>(kChunkWidth),
            static_cast<float>(kChunkHeight),
            min_z + static_cast<float>(kChunkDepth)
        }
    };
}

bool Renderer::aabb_visible_in_current_frustum(const Aabb& bounds) const {
    if (point_inside_aabb(current_camera_.camera_position, bounds)) {
        return true;
    }

    const std::array<Vec3, 8> corners {{
        {bounds.min.x, bounds.min.y, bounds.min.z},
        {bounds.max.x, bounds.min.y, bounds.min.z},
        {bounds.min.x, bounds.max.y, bounds.min.z},
        {bounds.max.x, bounds.max.y, bounds.min.z},
        {bounds.min.x, bounds.min.y, bounds.max.z},
        {bounds.max.x, bounds.min.y, bounds.max.z},
        {bounds.min.x, bounds.max.y, bounds.max.z},
        {bounds.max.x, bounds.max.y, bounds.max.z}
    }};

    std::array<ClipPoint, corners.size()> clip_points {};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        clip_points[i] = transform_point_clip(current_camera_.view_proj, corners[i]);
    }

    bool outside_left = true;
    bool outside_right = true;
    bool outside_bottom = true;
    bool outside_top = true;
    bool outside_near = true;
    bool outside_far = true;

    for (const ClipPoint& point : clip_points) {
        outside_left = outside_left && point.x < -point.w;
        outside_right = outside_right && point.x > point.w;
        outside_bottom = outside_bottom && point.y < -point.w;
        outside_top = outside_top && point.y > point.w;
        outside_near = outside_near && point.z < 0.0f;
        outside_far = outside_far && point.z > point.w;
    }

    return !(outside_left || outside_right || outside_bottom || outside_top || outside_near || outside_far);
}

Mat4 Renderer::chunk_view_proj(ChunkCoord coord) const {
    const Vec3 chunk_origin {
        static_cast<float>(coord.x * kChunkWidth),
        0.0f,
        static_cast<float>(coord.z * kChunkDepth)
    };
    return multiply(current_camera_.view_proj, translation_matrix(chunk_origin));
}

void Renderer::upload_dynamic_buffer(GpuBuffer& buffer, const std::vector<Vertex>& vertices) {
    if (vertices.empty()) {
        return;
    }

    const VkDeviceSize buffer_size = sizeof(Vertex) * vertices.size();
    if (buffer.buffer == VK_NULL_HANDLE || buffer.size < buffer_size) {
        vkDeviceWaitIdle(device_);
        destroy_buffer(buffer);
        buffer = create_buffer(
            buffer_size,
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );
    }

    void* mapped = nullptr;
    vkMapMemory(device_, buffer.memory, 0, buffer_size, 0, &mapped);
    std::memcpy(mapped, vertices.data(), static_cast<std::size_t>(buffer_size));
    vkUnmapMemory(device_, buffer.memory);
}

void Renderer::mark_dynamic_hud_dirty() {
    target_block_dirty_ = true;
    hotbar_dirty_ = true;
    crosshair_dirty_ = true;
    debug_hud_dirty_ = true;
}

void Renderer::update_chunk_outline_buffer(std::span<const ActiveChunk> visible_chunks) {
    std::vector<Vertex> vertices;
    vertices.reserve(visible_chunks.size() * 24);

    const Vec3 outline_color {1.0f, 0.72f, 0.12f};

    for (const ActiveChunk& chunk : visible_chunks) {
        const float min_x = static_cast<float>(chunk.coord.x * kChunkWidth);
        const float min_y = 0.0f;
        const float min_z = static_cast<float>(chunk.coord.z * kChunkDepth);
        const float max_x = min_x + static_cast<float>(kChunkWidth);
        const float max_y = static_cast<float>(kChunkHeight);
        const float max_z = min_z + static_cast<float>(kChunkDepth);
        append_box_edges(vertices, {min_x, min_y, min_z}, {max_x, max_y, max_z}, outline_color);
    }

    chunk_outline_vertex_count_ = static_cast<std::uint32_t>(vertices.size());
    if (vertices.empty()) {
        return;
    }

    upload_dynamic_buffer(chunk_outline_vertex_buffer_, vertices);
}

void Renderer::update_target_block_outline_buffer() {
    target_block_outline_vertex_count_ = 0;
    if (!target_block_.has_value()) {
        return;
    }

    constexpr float epsilon = 0.0035f;
    const BlockHit& hit = *target_block_;
    const Vec3 outline_color {1.0f, 1.0f, 1.0f};

    std::vector<Vertex> vertices;
    vertices.reserve(24);
    append_box_edges(
        vertices,
        {
            static_cast<float>(hit.block.x) - epsilon,
            static_cast<float>(hit.block.y) - epsilon,
            static_cast<float>(hit.block.z) - epsilon
        },
        {
            static_cast<float>(hit.block.x + 1) + epsilon,
            static_cast<float>(hit.block.y + 1) + epsilon,
            static_cast<float>(hit.block.z + 1) + epsilon
        },
        outline_color
    );

    target_block_outline_vertex_count_ = static_cast<std::uint32_t>(vertices.size());
    upload_dynamic_buffer(target_block_outline_vertex_buffer_, vertices);
}

void Renderer::update_crosshair_buffer() {
    const Vec3 color {1.0f, 1.0f, 1.0f};
    const float pixel_to_ndc_x = swapchain_extent_.width > 0
        ? 2.0f / static_cast<float>(swapchain_extent_.width)
        : 0.0f;
    const float pixel_to_ndc_y = swapchain_extent_.height > 0
        ? 2.0f / static_cast<float>(swapchain_extent_.height)
        : 0.0f;
    const float arm_x = 7.0f * pixel_to_ndc_x;
    const float arm_y = 7.0f * pixel_to_ndc_y;
    const float gap_x = 2.0f * pixel_to_ndc_x;
    const float gap_y = 2.0f * pixel_to_ndc_y;

    std::vector<Vertex> vertices;
    vertices.reserve(8);
    vertices.push_back({{-arm_x, 0.0f, 0.0f}, color});
    vertices.push_back({{-gap_x, 0.0f, 0.0f}, color});
    vertices.push_back({{gap_x, 0.0f, 0.0f}, color});
    vertices.push_back({{arm_x, 0.0f, 0.0f}, color});
    vertices.push_back({{0.0f, -arm_y, 0.0f}, color});
    vertices.push_back({{0.0f, -gap_y, 0.0f}, color});
    vertices.push_back({{0.0f, gap_y, 0.0f}, color});
    vertices.push_back({{0.0f, arm_y, 0.0f}, color});

    crosshair_vertex_count_ = static_cast<std::uint32_t>(vertices.size());
    upload_dynamic_buffer(crosshair_vertex_buffer_, vertices);
}

void Renderer::update_hotbar_buffer() {
    hotbar_fill_vertex_count_ = 0;
    hotbar_outline_vertex_count_ = 0;
    hotbar_texture_vertex_count_ = 0;

    if (hotbar_slot_count_ == 0 || swapchain_extent_.width == 0 || swapchain_extent_.height == 0) {
        return;
    }

    constexpr float ui_scale = 3.0f;
    constexpr float slot_width = 20.0f * ui_scale;
    constexpr float slot_height = 22.0f * ui_scale;
    constexpr float selected_width = 24.0f * ui_scale;
    constexpr float selected_height = 24.0f * ui_scale;
    constexpr float bottom_margin = 24.0f;
    constexpr float atlas_width = 204.0f;
    constexpr float atlas_height = 24.0f;
    const float width = static_cast<float>(swapchain_extent_.width);
    const float height = static_cast<float>(swapchain_extent_.height);
    const float total_width = static_cast<float>(hotbar_slot_count_) * slot_width;
    const float start_x = (width - total_width) * 0.5f;
    const float top = height - bottom_margin - slot_height;

    std::vector<Vertex> vertices;
    vertices.reserve(hotbar_slot_count_ * 6 + 6);

    for (std::size_t slot = 0; slot < hotbar_slot_count_; ++slot) {
        const float left = start_x + static_cast<float>(slot) * slot_width;
        const float right = left + slot_width;
        const float bottom = top + slot_height;
        const float u0 = static_cast<float>(slot * 20) / atlas_width;
        const float u1 = static_cast<float>(slot * 20 + 20) / atlas_width;
        const float v0 = 0.0f;
        const float v1 = 22.0f / atlas_height;
        append_hud_textured_quad(vertices, left, top, right, bottom, width, height, u0, v0, u1, v1);
    }

    const float selected_left = start_x + static_cast<float>(hotbar_selected_slot_) * slot_width - 2.0f * ui_scale;
    const float selected_top = top - 1.0f * ui_scale;
    const float selected_u0 = 180.0f / atlas_width;
    const float selected_u1 = 204.0f / atlas_width;
    append_hud_textured_quad(
        vertices,
        selected_left,
        selected_top,
        selected_left + selected_width,
        selected_top + selected_height,
        width,
        height,
        selected_u0,
        0.0f,
        selected_u1,
        1.0f
    );

    hotbar_texture_vertex_count_ = static_cast<std::uint32_t>(vertices.size());
    upload_dynamic_buffer(hotbar_texture_vertex_buffer_, vertices);
}

void Renderer::update_debug_hud_buffer() {
    debug_hud_vertex_count_ = 0;
    if (!debug_hud_enabled_ || swapchain_extent_.width == 0 || swapchain_extent_.height == 0) {
        return;
    }

    const float width = static_cast<float>(swapchain_extent_.width);
    const float height = static_cast<float>(swapchain_extent_.height);
    const float left = 18.0f;
    const float top = height - 32.0f;
    const float scale = 2.0f;
    const Vec3 color {1.0f, 1.0f, 1.0f};

    std::ostringstream fps_stream;
    fps_stream << "FPS:" << static_cast<int>(debug_hud_data_.fps + 0.5f);

    std::ostringstream xyz_stream;
    xyz_stream << std::fixed << std::setprecision(2)
        << "XYZ:" << debug_hud_data_.position.x
        << "/" << debug_hud_data_.position.y
        << "/" << debug_hud_data_.position.z;

    const std::string mode = debug_hud_data_.debug_fly ? "MODE:FLY" : "MODE:PLAYER";
    const std::string chunks = "CH:" + std::to_string(debug_hud_data_.visible_chunks);
    const std::string drawn = "DRAW:" + std::to_string(debug_hud_data_.drawn_chunks);
    const std::string uploads = "UP:" + std::to_string(debug_hud_data_.uploads_this_frame);
    const std::string pending = "PEND:" + std::to_string(debug_hud_data_.pending_uploads);
    const std::string rebuilds = "REB:" + std::to_string(debug_hud_data_.queued_rebuilds);
    const std::string leaves = debug_hud_data_.fancy_leaves ? "LEAVES:FANCY" : "LEAVES:FAST";

    std::vector<Vertex> vertices;
    vertices.reserve(
        (fps_stream.str().size() + xyz_stream.str().size() + mode.size() +
            chunks.size() + drawn.size() + uploads.size() + pending.size() + rebuilds.size() + leaves.size()) * 16
    );
    const auto append_left_text = [&](const std::string& text, float y) {
        const float advance = 6.0f * scale;
        append_debug_text(vertices, text, left + static_cast<float>(text.size()) * advance, y, scale, width, height, color);
    };

    append_left_text(fps_stream.str(), top);
    append_left_text(xyz_stream.str(), top - 20.0f);
    append_left_text(mode, top - 40.0f);
    append_left_text(chunks, top - 60.0f);
    append_left_text(drawn, top - 80.0f);
    append_left_text(uploads, top - 100.0f);
    append_left_text(pending, top - 120.0f);
    append_left_text(rebuilds, top - 140.0f);
    append_left_text(leaves, top - 160.0f);

    debug_hud_vertex_count_ = static_cast<std::uint32_t>(vertices.size());
    upload_dynamic_buffer(debug_hud_vertex_buffer_, vertices);
}

void Renderer::draw_chunk_outlines(const FrameResources& frame) {
    if (chunk_outline_vertex_count_ == 0 || chunk_outline_pipeline_ == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, chunk_outline_pipeline_);
    vkCmdPushConstants(
        frame.command_buffer,
        hud_pipeline_layout_,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(Mat4),
        current_camera_.view_proj.m.data()
    );
    const VkBuffer vertex_buffers[] = {chunk_outline_vertex_buffer_.buffer};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdDraw(frame.command_buffer, chunk_outline_vertex_count_, 1, 0, 0);
}

void Renderer::draw_target_block_outline(const FrameResources& frame) {
    if (target_block_outline_vertex_count_ == 0 || block_outline_pipeline_ == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, block_outline_pipeline_);
    vkCmdPushConstants(
        frame.command_buffer,
        hud_pipeline_layout_,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(Mat4),
        current_camera_.view_proj.m.data()
    );
    const VkBuffer vertex_buffers[] = {target_block_outline_vertex_buffer_.buffer};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdDraw(frame.command_buffer, target_block_outline_vertex_count_, 1, 0, 0);
}

void Renderer::draw_hotbar(const FrameResources& frame) {
    if (hotbar_texture_vertex_count_ == 0 || hotbar_texture_pipeline_ == VK_NULL_HANDLE || ui_descriptor_set_ == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hotbar_texture_pipeline_);
    vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_layout_, 0, 1, &ui_descriptor_set_, 0, nullptr);
    const VkBuffer vertex_buffers[] = {hotbar_texture_vertex_buffer_.buffer};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdDraw(frame.command_buffer, hotbar_texture_vertex_count_, 1, 0, 0);
}

void Renderer::draw_debug_hud(const FrameResources& frame) {
    if (debug_hud_vertex_count_ == 0 || crosshair_pipeline_ == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, crosshair_pipeline_);
    const VkBuffer vertex_buffers[] = {debug_hud_vertex_buffer_.buffer};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdDraw(frame.command_buffer, debug_hud_vertex_count_, 1, 0, 0);
}

void Renderer::draw_crosshair(const FrameResources& frame) {
    if (crosshair_vertex_count_ == 0 || crosshair_pipeline_ == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, crosshair_pipeline_);
    const VkBuffer vertex_buffers[] = {crosshair_vertex_buffer_.buffer};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdDraw(frame.command_buffer, crosshair_vertex_count_, 1, 0, 0);
}

void Renderer::update_main_menu_buffers(float time_seconds, bool use_night_panorama, int hovered_button) {
    menu_panorama_vertex_count_ = 0;
    menu_logo_vertex_count_ = 0;
    menu_button_vertex_count_ = 0;
    menu_button_highlight_vertex_count_ = 0;
    menu_overlay_vertex_count_ = 0;
    menu_text_vertex_count_ = 0;
    menu_font_vertex_count_ = 0;
    if (swapchain_extent_.width == 0 || swapchain_extent_.height == 0) {
        return;
    }

    const float width = static_cast<float>(swapchain_extent_.width);
    const float height = static_cast<float>(swapchain_extent_.height);
    const float scale = menu_integer_scale(width, height);
    const MenuTexture& panorama = use_night_panorama ? menu_panorama_night_ : menu_panorama_day_;

    std::vector<Vertex> panorama_vertices;
    panorama_vertices.reserve(6);
    const float view_ratio = width / height;
    const float image_ratio = panorama.height > 0 ? static_cast<float>(panorama.width) / static_cast<float>(panorama.height) : view_ratio;
    const float visible_u = clamp(view_ratio / std::max(image_ratio, 0.001f), 0.0f, 1.0f);
    const float travel = std::max(0.0f, 1.0f - visible_u);
    constexpr float kPanoramaScrollUvPerSecond = 0.006f;
    const float u0 = ping_pong_offset(time_seconds, kPanoramaScrollUvPerSecond, travel, travel * 0.5f);
    const float u1 = std::min(1.0f, u0 + visible_u);
    append_hud_textured_quad(panorama_vertices, 0.0f, 0.0f, width, height, width, height, u0, 0.0f, u1, 1.0f);
    menu_panorama_vertex_count_ = static_cast<std::uint32_t>(panorama_vertices.size());
    upload_dynamic_buffer(menu_panorama_vertex_buffer_, panorama_vertices);

    menu_overlay_vertex_count_ = 0;

    std::vector<Vertex> logo_vertices;
    logo_vertices.reserve(6);
    const float logo_width = std::min(width * 0.46f, 857.0f * scale * 0.32f);
    const float logo_height = logo_width * (207.0f / 857.0f);
    const float logo_left = (width - logo_width) * 0.5f;
    const float logo_top = height * 0.075f;
    append_hud_textured_quad(logo_vertices, logo_left, logo_top, logo_left + logo_width, logo_top + logo_height, width, height, 0.0f, 0.0f, 1.0f, 1.0f);
    menu_logo_vertex_count_ = static_cast<std::uint32_t>(logo_vertices.size());
    upload_dynamic_buffer(menu_logo_vertex_buffer_, logo_vertices);

    constexpr std::array<const char*, 6> labels {{
        "Play Game",
        "Mini Games",
        "Leaderboards",
        "Help & Options",
        "Minecraft Store",
        "Launch New Minecraft"
    }};
    const float button_width = 200.0f * scale;
    const float button_height = 20.0f * scale;
    const float gap = 5.0f * scale;
    const float left = (width - button_width) * 0.5f;
    const float first_top = height * 0.35f;

    std::vector<Vertex> button_vertices;
    std::vector<Vertex> button_highlight_vertices;
    std::vector<Vertex> text_vertices;
    std::vector<Vertex> font_vertices;
    button_vertices.reserve(6 * 5);
    button_highlight_vertices.reserve(6);
    text_vertices.reserve(6000);
    font_vertices.reserve(6000);

    for (std::size_t i = 0; i < labels.size(); ++i) {
        const float top = first_top + static_cast<float>(i) * (button_height + gap);
        std::vector<Vertex>& target = static_cast<int>(i) == hovered_button ? button_highlight_vertices : button_vertices;
        append_hud_textured_quad(target, left, top, left + button_width, top + button_height, width, height, 0.0f, 0.0f, 1.0f, 1.0f);

        const std::string label = labels[i];
        const float preferred_text_scale = std::max(1.2f, scale * 1.12f);
        const float text_scale = std::max(1.0f, std::min(preferred_text_scale, (button_width * 0.82f) / pixel_text_width(label, 1.0f)));
        const float font_pixel_height = 8.0f * preferred_text_scale;
        const float text_width = menu_font_.loaded ? menu_font_text_width(label, font_pixel_height) : pixel_text_width(label, text_scale);
        
        float actual_pixel_height = font_pixel_height;
        if (text_width > button_width * 0.82f && menu_font_.loaded) {
            actual_pixel_height = actual_pixel_height * ((button_width * 0.82f) / text_width);
        }
        
        const float actual_text_width = menu_font_.loaded ? menu_font_text_width(label, actual_pixel_height) : text_width;
        const float text_left = left + (button_width - actual_text_width) * 0.5f;
        const float text_top = top + (button_height - actual_pixel_height) * 0.5f;
        const Vec3 text_color = static_cast<int>(i) == hovered_button ? Vec3 {1.0f, 1.0f, 0.25f} : Vec3 {1.0f, 1.0f, 1.0f};
        
        if (menu_font_.loaded) {
            append_menu_font_text(font_vertices, label, text_left + std::max(1.0f, scale * 0.4f), text_top + std::max(1.0f, scale * 0.4f), actual_pixel_height, width, height, {0.0f, 0.0f, 0.0f});
            append_menu_font_text(font_vertices, label, text_left, text_top, actual_pixel_height, width, height, text_color);
        } else {
            append_pixel_text(text_vertices, label, text_left + std::max(1.0f, scale * 0.4f), text_top + std::max(1.0f, scale * 0.4f), text_scale, width, height, {0.0f, 0.0f, 0.0f});
            append_pixel_text(text_vertices, label, text_left, text_top, text_scale, width, height, text_color);
        }
    }

    const std::string splash = "WHAT DOES THE FOX SAY?";
    const float splash_scale = std::max(1.5f, std::min(scale * 1.45f, (width * 0.30f) / pixel_text_width(splash, 1.0f)));
    const float splash_font_height = 12.0f * splash_scale;
    const float splash_actual_width = menu_font_.loaded ? menu_font_text_width(splash, splash_font_height) : pixel_text_width(splash, splash_scale);
    const float splash_left = std::min(width * 0.54f, width - splash_actual_width - width * 0.08f);
    const float splash_top = logo_top + logo_height * 0.42f;
    
    if (menu_font_.loaded) {
        append_menu_font_text(font_vertices, splash, splash_left + 2.0f, splash_top + 2.0f, splash_font_height, width, height, {0.25f, 0.25f, 0.0f}, -0.34f);
        append_menu_font_text(font_vertices, splash, splash_left, splash_top, splash_font_height, width, height, {1.0f, 1.0f, 0.0f}, -0.34f);
    } else {
        append_pixel_text(text_vertices, splash, splash_left + 2.0f, splash_top + 2.0f, splash_scale, width, height, {0.25f, 0.25f, 0.0f}, -0.34f);
        append_pixel_text(text_vertices, splash, splash_left, splash_top, splash_scale, width, height, {1.0f, 1.0f, 0.0f}, -0.34f);
    }

    menu_button_vertex_count_ = static_cast<std::uint32_t>(button_vertices.size());
    menu_button_highlight_vertex_count_ = static_cast<std::uint32_t>(button_highlight_vertices.size());
    menu_text_vertex_count_ = static_cast<std::uint32_t>(text_vertices.size());
    menu_font_vertex_count_ = static_cast<std::uint32_t>(font_vertices.size());
    upload_dynamic_buffer(menu_button_vertex_buffer_, button_vertices);
    upload_dynamic_buffer(menu_button_highlight_vertex_buffer_, button_highlight_vertices);
    upload_dynamic_buffer(menu_text_vertex_buffer_, text_vertices);
    upload_dynamic_buffer(menu_font_vertex_buffer_, font_vertices);
}

void Renderer::draw_textured_buffer(const FrameResources& frame, const GpuBuffer& buffer, std::uint32_t vertex_count, VkDescriptorSet descriptor_set) {
    if (vertex_count == 0 || buffer.buffer == VK_NULL_HANDLE || hotbar_texture_pipeline_ == VK_NULL_HANDLE || descriptor_set == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, hotbar_texture_pipeline_);
    vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_layout_, 0, 1, &descriptor_set, 0, nullptr);
    const VkBuffer vertex_buffers[] = {buffer.buffer};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdDraw(frame.command_buffer, vertex_count, 1, 0, 0);
}

void Renderer::draw_colored_buffer(const FrameResources& frame, const GpuBuffer& buffer, std::uint32_t vertex_count, VkPipeline pipeline) {
    if (vertex_count == 0 || buffer.buffer == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkBuffer vertex_buffers[] = {buffer.buffer};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdDraw(frame.command_buffer, vertex_count, 1, 0, 0);
}

bool Renderer::load_textures() {
    const std::vector<std::string> texture_paths = {
        "assets/textures/texture_pack/classic/blocks/dirt.png",
        "assets/textures/texture_pack/classic/blocks/grass_carried.png",
        "assets/textures/texture_pack/classic/blocks/grass_side_carried.png",
        "assets/textures/texture_pack/classic/blocks/stone.png",
        "assets/textures/texture_pack/classic/blocks/water_placeholder.png",
        "assets/textures/texture_pack/classic/blocks/sand.png",
        "assets/textures/texture_pack/classic/blocks/gravel.png",
        "assets/textures/texture_pack/classic/blocks/log_oak.png",
        "assets/textures/texture_pack/classic/blocks/log_oak_top.png",
        "assets/textures/texture_pack/classic/blocks/leaves_big_oak_carried.tga"
    };

    const int array_layers = static_cast<int>(texture_paths.size());
    const int tex_width = 16;
    const int tex_height = 16;
    const int tex_channels = 4;
    const VkDeviceSize layer_size = tex_width * tex_height * tex_channels;
    const VkDeviceSize image_size = layer_size * array_layers;

    GpuBuffer staging_buffer = create_buffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    void* data;
    vkMapMemory(device_, staging_buffer.memory, 0, image_size, 0, &data);

    for (int i = 0; i < array_layers; ++i) {
        int width, height, channels;
        stbi_uc* pixels = stbi_load(texture_paths[i].c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (!pixels) {
            log_message(LogLevel::Error, "Renderer: failed to load texture image");
            vkUnmapMemory(device_, staging_buffer.memory);
            destroy_buffer(staging_buffer);
            return false;
        }
        if (width != tex_width || height != tex_height) {
            log_message(LogLevel::Error, "Renderer: texture size is not 16x16");
            stbi_image_free(pixels);
            vkUnmapMemory(device_, staging_buffer.memory);
            destroy_buffer(staging_buffer);
            return false;
        }

        std::memcpy(static_cast<stbi_uc*>(data) + layer_size * i, pixels, static_cast<std::size_t>(layer_size));
        stbi_image_free(pixels);
    }

    vkUnmapMemory(device_, staging_buffer.memory);

    VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = static_cast<std::uint32_t>(tex_width);
    image_info.extent.height = static_cast<std::uint32_t>(tex_height);
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = static_cast<std::uint32_t>(array_layers);
    image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &image_info, nullptr, &texture_array_) != VK_SUCCESS) {
        destroy_buffer(staging_buffer);
        return false;
    }

    VkMemoryRequirements mem_requirements;
    vkGetImageMemoryRequirements(device_, texture_array_, &mem_requirements);

    VkMemoryAllocateInfo alloc_info {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (vkAllocateMemory(device_, &alloc_info, nullptr, &texture_memory_) != VK_SUCCESS) {
        destroy_buffer(staging_buffer);
        return false;
    }

    vkBindImageMemory(device_, texture_array_, texture_memory_, 0);

    VkCommandBufferAllocateInfo alloc_info_cb {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info_cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info_cb.commandPool = command_pool_;
    alloc_info_cb.commandBufferCount = 1;

    VkCommandBuffer command_buffer;
    vkAllocateCommandBuffers(device_, &alloc_info_cb, &command_buffer);

    VkCommandBufferBeginInfo begin_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(command_buffer, &begin_info);

    VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture_array_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = static_cast<std::uint32_t>(array_layers);
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = static_cast<std::uint32_t>(array_layers);
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {
        static_cast<std::uint32_t>(tex_width),
        static_cast<std::uint32_t>(tex_height),
        1
    };

    vkCmdCopyBufferToImage(command_buffer, staging_buffer.buffer, texture_array_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );

    vkEndCommandBuffer(command_buffer);

    VkSubmitInfo submit_info {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;

    vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);

    vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);

    destroy_buffer(staging_buffer);

    VkImageViewCreateInfo view_info {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = texture_array_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = static_cast<std::uint32_t>(array_layers);

    if (vkCreateImageView(device_, &view_info, nullptr, &texture_view_) != VK_SUCCESS) {
        return false;
    }

    VkSamplerCreateInfo sampler_info {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.mipLodBias = 0.0f;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;

    if (vkCreateSampler(device_, &sampler_info, nullptr, &texture_sampler_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize pool_size{};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = 1;

    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info_set {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_info_set.descriptorPool = descriptor_pool_;
    alloc_info_set.descriptorSetCount = 1;
    alloc_info_set.pSetLayouts = &descriptor_set_layout_;

    if (vkAllocateDescriptorSets(device_, &alloc_info_set, &descriptor_set_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo image_desc_info{};
    image_desc_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_desc_info.imageView = texture_view_;
    image_desc_info.sampler = texture_sampler_;

    VkWriteDescriptorSet descriptor_write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    descriptor_write.dstSet = descriptor_set_;
    descriptor_write.dstBinding = 0;
    descriptor_write.dstArrayElement = 0;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_write.descriptorCount = 1;
    descriptor_write.pImageInfo = &image_desc_info;

    vkUpdateDescriptorSets(device_, 1, &descriptor_write, 0, nullptr);

    return true;
}

bool Renderer::load_ui_textures() {
    constexpr int atlas_width = 204;
    constexpr int atlas_height = 24;
    constexpr int tex_channels = 4;
    const VkDeviceSize image_size = atlas_width * atlas_height * tex_channels;

    std::vector<stbi_uc> atlas(static_cast<std::size_t>(image_size), 0);
    const auto blit_image = [&](const std::string& path, int dst_x, int expected_width, int expected_height) -> bool {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
        if (pixels == nullptr) {
            log_message(LogLevel::Error, "Renderer: failed to load UI texture image");
            return false;
        }
        if (width != expected_width || height != expected_height) {
            log_message(LogLevel::Error, "Renderer: UI texture has unexpected size");
            stbi_image_free(pixels);
            return false;
        }

        for (int y = 0; y < height; ++y) {
            const std::size_t src_offset = static_cast<std::size_t>(y * width * tex_channels);
            const std::size_t dst_offset = static_cast<std::size_t>((y * atlas_width + dst_x) * tex_channels);
            std::memcpy(atlas.data() + dst_offset, pixels + src_offset, static_cast<std::size_t>(width * tex_channels));
        }
        stbi_image_free(pixels);
        return true;
    };

    for (int i = 0; i < 9; ++i) {
        if (!blit_image(
                "assets/textures/texture_pack/classic/ui/hotbar_" + std::to_string(i) + ".png",
                i * 20,
                20,
                22)) {
            return false;
        }
    }
    if (!blit_image("assets/textures/texture_pack/classic/ui/selected_hotbar_slot.png", 180, 24, 24)) {
        return false;
    }

    GpuBuffer staging_buffer = create_buffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    void* data = nullptr;
    vkMapMemory(device_, staging_buffer.memory, 0, image_size, 0, &data);
    std::memcpy(data, atlas.data(), static_cast<std::size_t>(image_size));
    vkUnmapMemory(device_, staging_buffer.memory);

    VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = atlas_width;
    image_info.extent.height = atlas_height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &image_info, nullptr, &ui_texture_) != VK_SUCCESS) {
        destroy_buffer(staging_buffer);
        return false;
    }

    VkMemoryRequirements mem_requirements {};
    vkGetImageMemoryRequirements(device_, ui_texture_, &mem_requirements);

    VkMemoryAllocateInfo alloc_info {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &ui_texture_memory_) != VK_SUCCESS) {
        destroy_buffer(staging_buffer);
        return false;
    }
    vkBindImageMemory(device_, ui_texture_, ui_texture_memory_, 0);

    VkCommandBufferAllocateInfo alloc_info_cb {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info_cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info_cb.commandPool = command_pool_;
    alloc_info_cb.commandBufferCount = 1;

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &alloc_info_cb, &command_buffer);

    VkCommandBufferBeginInfo begin_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command_buffer, &begin_info);

    VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = ui_texture_;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    VkBufferImageCopy region {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {atlas_width, atlas_height, 1};

    vkCmdCopyBufferToImage(command_buffer, staging_buffer.buffer, ui_texture_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    vkEndCommandBuffer(command_buffer);

    VkSubmitInfo submit_info {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
    destroy_buffer(staging_buffer);

    VkImageViewCreateInfo view_info {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = ui_texture_;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &ui_texture_view_) != VK_SUCCESS) {
        return false;
    }

    VkSamplerCreateInfo sampler_info {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_NEAREST;
    sampler_info.minFilter = VK_FILTER_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;
    if (vkCreateSampler(device_, &sampler_info, nullptr, &ui_texture_sampler_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize pool_size {};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = 1;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &ui_descriptor_pool_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info_set {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_info_set.descriptorPool = ui_descriptor_pool_;
    alloc_info_set.descriptorSetCount = 1;
    alloc_info_set.pSetLayouts = &ui_descriptor_set_layout_;
    if (vkAllocateDescriptorSets(device_, &alloc_info_set, &ui_descriptor_set_) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo image_desc_info {};
    image_desc_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_desc_info.imageView = ui_texture_view_;
    image_desc_info.sampler = ui_texture_sampler_;

    VkWriteDescriptorSet descriptor_write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    descriptor_write.dstSet = ui_descriptor_set_;
    descriptor_write.dstBinding = 0;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_write.descriptorCount = 1;
    descriptor_write.pImageInfo = &image_desc_info;
    vkUpdateDescriptorSets(device_, 1, &descriptor_write, 0, nullptr);

    return true;
}

bool Renderer::load_menu_textures() {
    if (!load_menu_font()) {
        log_message(LogLevel::Error, "Renderer: failed to load menu font");
    }
    return load_menu_texture("assets/panorama/panorama_tu69_day.png", false, false, menu_panorama_day_) &&
        load_menu_texture("assets/panorama/panorama_tu69_night.png", false, false, menu_panorama_night_) &&
        load_menu_texture("assets/button/button.png", false, true, menu_button_) &&
        load_menu_texture("assets/button/button_highlighted.png", false, true, menu_button_highlighted_) &&
        load_menu_texture("assets/sound/ui/logo/legacy_console_edition_logo.png.png", false, true, menu_logo_);
}

bool Renderer::load_menu_texture(const std::string& path, bool repeat, bool pixelated, MenuTexture& texture) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, STBI_rgb_alpha);
    if (pixels == nullptr || width <= 0 || height <= 0) {
        log_message(LogLevel::Error, "Renderer: failed to load menu texture " + path);
        return false;
    }

    texture.width = static_cast<std::uint32_t>(width);
    texture.height = static_cast<std::uint32_t>(height);
    constexpr int tex_channels = 4;
    const VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * tex_channels;

    GpuBuffer staging_buffer = create_buffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    void* data = nullptr;
    vkMapMemory(device_, staging_buffer.memory, 0, image_size, 0, &data);
    std::memcpy(data, pixels, static_cast<std::size_t>(image_size));
    vkUnmapMemory(device_, staging_buffer.memory);
    stbi_image_free(pixels);

    VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = texture.width;
    image_info.extent.height = texture.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &image_info, nullptr, &texture.image) != VK_SUCCESS) {
        destroy_buffer(staging_buffer);
        return false;
    }

    VkMemoryRequirements mem_requirements {};
    vkGetImageMemoryRequirements(device_, texture.image, &mem_requirements);

    VkMemoryAllocateInfo alloc_info {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &texture.memory) != VK_SUCCESS) {
        destroy_buffer(staging_buffer);
        return false;
    }
    vkBindImageMemory(device_, texture.image, texture.memory, 0);

    VkCommandBufferAllocateInfo alloc_info_cb {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info_cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info_cb.commandPool = command_pool_;
    alloc_info_cb.commandBufferCount = 1;

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &alloc_info_cb, &command_buffer);

    VkCommandBufferBeginInfo begin_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command_buffer, &begin_info);

    VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    VkBufferImageCopy region {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {texture.width, texture.height, 1};

    vkCmdCopyBufferToImage(command_buffer, staging_buffer.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    vkEndCommandBuffer(command_buffer);

    VkSubmitInfo submit_info {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
    destroy_buffer(staging_buffer);

    VkImageViewCreateInfo view_info {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &texture.view) != VK_SUCCESS) {
        return false;
    }

    VkSamplerCreateInfo sampler_info {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = pixelated ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sampler_info.minFilter = pixelated ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sampler_info.addressModeU = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
    sampler_info.mipmapMode = pixelated ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;
    if (vkCreateSampler(device_, &sampler_info, nullptr, &texture.sampler) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize pool_size {};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = 1;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &texture.descriptor_pool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info_set {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_info_set.descriptorPool = texture.descriptor_pool;
    alloc_info_set.descriptorSetCount = 1;
    alloc_info_set.pSetLayouts = &ui_descriptor_set_layout_;
    if (vkAllocateDescriptorSets(device_, &alloc_info_set, &texture.descriptor_set) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo image_desc_info {};
    image_desc_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_desc_info.imageView = texture.view;
    image_desc_info.sampler = texture.sampler;

    VkWriteDescriptorSet descriptor_write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    descriptor_write.dstSet = texture.descriptor_set;
    descriptor_write.dstBinding = 0;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_write.descriptorCount = 1;
    descriptor_write.pImageInfo = &image_desc_info;
    vkUpdateDescriptorSets(device_, 1, &descriptor_write, 0, nullptr);

    return true;
}

void Renderer::destroy_menu_texture(MenuTexture& texture) {
    if (texture.descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, texture.descriptor_pool, nullptr);
        texture.descriptor_pool = VK_NULL_HANDLE;
    }
    if (texture.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device_, texture.sampler, nullptr);
        texture.sampler = VK_NULL_HANDLE;
    }
    if (texture.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, texture.view, nullptr);
        texture.view = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE) {
        vkDestroyImage(device_, texture.image, nullptr);
        texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_, texture.memory, nullptr);
        texture.memory = VK_NULL_HANDLE;
    }
    texture.descriptor_set = VK_NULL_HANDLE;
    texture.width = 0;
    texture.height = 0;
}

void Renderer::destroy_textures() {
    destroy_menu_texture(menu_panorama_day_);
    destroy_menu_texture(menu_panorama_night_);
    destroy_menu_texture(menu_button_);
    destroy_menu_texture(menu_button_highlighted_);
    destroy_menu_texture(menu_logo_);

    if (ui_descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, ui_descriptor_pool_, nullptr);
        ui_descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (ui_descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, ui_descriptor_set_layout_, nullptr);
        ui_descriptor_set_layout_ = VK_NULL_HANDLE;
    }
    if (ui_texture_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, ui_texture_sampler_, nullptr);
        ui_texture_sampler_ = VK_NULL_HANDLE;
    }
    if (ui_texture_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, ui_texture_view_, nullptr);
        ui_texture_view_ = VK_NULL_HANDLE;
    }
    if (ui_texture_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, ui_texture_, nullptr);
        ui_texture_ = VK_NULL_HANDLE;
    }
    if (ui_texture_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, ui_texture_memory_, nullptr);
        ui_texture_memory_ = VK_NULL_HANDLE;
    }

    if (descriptor_pool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
    }
    if (descriptor_set_layout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
        descriptor_set_layout_ = VK_NULL_HANDLE;
    }
    if (texture_sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(device_, texture_sampler_, nullptr);
        texture_sampler_ = VK_NULL_HANDLE;
    }
    if (texture_view_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, texture_view_, nullptr);
        texture_view_ = VK_NULL_HANDLE;
    }
    if (texture_array_ != VK_NULL_HANDLE) {
        vkDestroyImage(device_, texture_array_, nullptr);
        texture_array_ = VK_NULL_HANDLE;
    }
    if (texture_memory_ != VK_NULL_HANDLE) {
        vkFreeMemory(device_, texture_memory_, nullptr);
        texture_memory_ = VK_NULL_HANDLE;
    }
}

bool Renderer::load_menu_texture_from_rgba(const std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t height, bool repeat, bool pixelated, MenuTexture& texture) {
    if (pixels.empty() || width == 0 || height == 0) {
        log_message(LogLevel::Error, "Renderer: invalid pixels for load_menu_texture_from_rgba");
        return false;
    }

    texture.width = width;
    texture.height = height;
    constexpr int tex_channels = 4;
    const VkDeviceSize image_size = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * tex_channels;

    GpuBuffer staging_buffer = create_buffer(
        image_size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    void* data = nullptr;
    vkMapMemory(device_, staging_buffer.memory, 0, image_size, 0, &data);
    std::memcpy(data, pixels.data(), static_cast<std::size_t>(image_size));
    vkUnmapMemory(device_, staging_buffer.memory);

    VkImageCreateInfo image_info {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.extent.width = texture.width;
    image_info.extent.height = texture.height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateImage(device_, &image_info, nullptr, &texture.image) != VK_SUCCESS) {
        destroy_buffer(staging_buffer);
        return false;
    }

    VkMemoryRequirements mem_requirements {};
    vkGetImageMemoryRequirements(device_, texture.image, &mem_requirements);

    VkMemoryAllocateInfo alloc_info {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc_info.allocationSize = mem_requirements.size;
    alloc_info.memoryTypeIndex = find_memory_type(mem_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device_, &alloc_info, nullptr, &texture.memory) != VK_SUCCESS) {
        destroy_buffer(staging_buffer);
        return false;
    }
    vkBindImageMemory(device_, texture.image, texture.memory, 0);

    VkCommandBufferAllocateInfo alloc_info_cb {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info_cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info_cb.commandPool = command_pool_;
    alloc_info_cb.commandBufferCount = 1;

    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(device_, &alloc_info_cb, &command_buffer);

    VkCommandBufferBeginInfo begin_info {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(command_buffer, &begin_info);

    VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = texture.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    VkBufferImageCopy region {};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {texture.width, texture.height, 1};

    vkCmdCopyBufferToImage(command_buffer, staging_buffer.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(
        command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &barrier
    );

    vkEndCommandBuffer(command_buffer);

    VkSubmitInfo submit_info {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer;
    vkQueueSubmit(graphics_queue_, 1, &submit_info, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue_);
    vkFreeCommandBuffers(device_, command_pool_, 1, &command_buffer);
    destroy_buffer(staging_buffer);

    VkImageViewCreateInfo view_info {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = texture.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_SRGB;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device_, &view_info, nullptr, &texture.view) != VK_SUCCESS) {
        return false;
    }

    VkSamplerCreateInfo sampler_info {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = pixelated ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sampler_info.minFilter = pixelated ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sampler_info.addressModeU = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.anisotropyEnable = VK_FALSE;
    sampler_info.maxAnisotropy = 1.0f;
    sampler_info.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    sampler_info.unnormalizedCoordinates = VK_FALSE;
    sampler_info.compareEnable = VK_FALSE;
    sampler_info.compareOp = VK_COMPARE_OP_ALWAYS;
    sampler_info.mipmapMode = pixelated ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = 0.0f;
    if (vkCreateSampler(device_, &sampler_info, nullptr, &texture.sampler) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorPoolSize pool_size {};
    pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool_size.descriptorCount = 1;

    VkDescriptorPoolCreateInfo pool_info {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &pool_size;
    pool_info.maxSets = 1;
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr, &texture.descriptor_pool) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorSetAllocateInfo alloc_info_set {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    alloc_info_set.descriptorPool = texture.descriptor_pool;
    alloc_info_set.descriptorSetCount = 1;
    alloc_info_set.pSetLayouts = &ui_descriptor_set_layout_;
    if (vkAllocateDescriptorSets(device_, &alloc_info_set, &texture.descriptor_set) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo image_desc_info {};
    image_desc_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image_desc_info.imageView = texture.view;
    image_desc_info.sampler = texture.sampler;

    VkWriteDescriptorSet descriptor_write {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    descriptor_write.dstSet = texture.descriptor_set;
    descriptor_write.dstBinding = 0;
    descriptor_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptor_write.descriptorCount = 1;
    descriptor_write.pImageInfo = &image_desc_info;
    vkUpdateDescriptorSets(device_, 1, &descriptor_write, 0, nullptr);

    return true;
}

bool Renderer::load_menu_font() {
    std::ifstream file("assets/fonts/RU/minecraft.ttf", std::ios::binary);
    if (!file) {
        log_message(LogLevel::Error, "Renderer: failed to open assets/fonts/RU/minecraft.ttf");
        return false;
    }
    
    std::vector<unsigned char> ttf_buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    log_message(LogLevel::Info, "Renderer: loaded minecraft.ttf, size: " + std::to_string(ttf_buffer.size()) + " bytes");
    
    const float pixel_height = 64.0f;
    menu_font_.pixel_height = pixel_height;
    
    const int atlas_width = 2048;
    const int atlas_height = 2048;
    std::vector<unsigned char> bitmap(atlas_width * atlas_height);
    
    stbtt_pack_context spc;
    if (!stbtt_PackBegin(&spc, bitmap.data(), atlas_width, atlas_height, 0, 1, nullptr)) {
        log_message(LogLevel::Error, "Renderer: stbtt_PackBegin failed");
        return false;
    }
    
    stbtt_packedchar ascii_data[128];
    stbtt_packedchar cyrillic_data[256];
    
    stbtt_pack_range ranges[2];
    ranges[0].font_size = pixel_height;
    ranges[0].first_unicode_codepoint_in_range = 0;
    ranges[0].array_of_unicode_codepoints = nullptr;
    ranges[0].num_chars = 128;
    ranges[0].chardata_for_range = ascii_data;
    ranges[0].h_oversample = 1;
    ranges[0].v_oversample = 1;
    
    ranges[1].font_size = pixel_height;
    ranges[1].first_unicode_codepoint_in_range = 0x0400; // Cyrillic range U+0400 to U+04FF
    ranges[1].array_of_unicode_codepoints = nullptr;
    ranges[1].num_chars = 256;
    ranges[1].chardata_for_range = cyrillic_data;
    ranges[1].h_oversample = 1;
    ranges[1].v_oversample = 1;
    
    if (!stbtt_PackFontRanges(&spc, ttf_buffer.data(), 0, ranges, 2)) {
        log_message(LogLevel::Error, "Renderer: stbtt_PackFontRanges failed (atlas size might still be too small or font is invalid)");
        stbtt_PackEnd(&spc);
        return false;
    }
    stbtt_PackEnd(&spc);
    log_message(LogLevel::Info, "Renderer: font glyphs packed successfully into " + std::to_string(atlas_width) + "x" + std::to_string(atlas_height) + " atlas");
    
    std::vector<std::uint8_t> rgba(atlas_width * atlas_height * 4);
    for (int i = 0; i < atlas_width * atlas_height; ++i) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = bitmap[i];
    }
    
    if (!load_menu_texture_from_rgba(rgba, atlas_width, atlas_height, false, false, menu_font_.texture)) {
        log_message(LogLevel::Error, "Renderer: failed to create font texture from RGBA data");
        return false;
    }
    
    for (int i = 0; i < 128; ++i) {
        MenuGlyph& glyph = menu_font_.glyphs[i];
        glyph.u0 = ascii_data[i].x0 / static_cast<float>(atlas_width);
        glyph.v0 = ascii_data[i].y0 / static_cast<float>(atlas_height);
        glyph.u1 = ascii_data[i].x1 / static_cast<float>(atlas_width);
        glyph.v1 = ascii_data[i].y1 / static_cast<float>(atlas_height);
        glyph.width = static_cast<float>(ascii_data[i].x1 - ascii_data[i].x0);
        glyph.height = static_cast<float>(ascii_data[i].y1 - ascii_data[i].y0);
        glyph.advance = ascii_data[i].xadvance;
        glyph.bearing_x = ascii_data[i].xoff;
        glyph.bearing_y = ascii_data[i].yoff;
    }
    
    for (int i = 0; i < 256; ++i) {
        MenuGlyph& glyph = menu_font_.glyphs[0x0400 + i];
        glyph.u0 = cyrillic_data[i].x0 / static_cast<float>(atlas_width);
        glyph.v0 = cyrillic_data[i].y0 / static_cast<float>(atlas_height);
        glyph.u1 = cyrillic_data[i].x1 / static_cast<float>(atlas_width);
        glyph.v1 = cyrillic_data[i].y1 / static_cast<float>(atlas_height);
        glyph.width = static_cast<float>(cyrillic_data[i].x1 - cyrillic_data[i].x0);
        glyph.height = static_cast<float>(cyrillic_data[i].y1 - cyrillic_data[i].y0);
        glyph.advance = cyrillic_data[i].xadvance;
        glyph.bearing_x = cyrillic_data[i].xoff;
        glyph.bearing_y = cyrillic_data[i].yoff;
    }
    
    menu_font_.loaded = true;
    log_message(LogLevel::Info, "Renderer: menu font loaded successfully");
    return true;
}

float Renderer::menu_font_text_width(const std::string& text, float target_pixel_height) const {
    if (!menu_font_.loaded) return 0.0f;
    float width = 0.0f;
    const float scale = target_pixel_height / menu_font_.pixel_height;
    const char* it = text.c_str();
    const char* end = it + text.size();
    while (it < end) {
        const std::uint32_t c = decode_utf8(it, end);
        if (c < menu_font_.glyphs.size()) {
            width += menu_font_.glyphs[c].advance * scale;
        }
    }
    return width;
}

void Renderer::append_menu_font_text(std::vector<Vertex>& vertices, const std::string& text, float x, float y, float target_pixel_height, float viewport_width, float viewport_height, Vec3 color, float rotation_radians) const {
    if (!menu_font_.loaded) return;
    float cursor_x = x;
    const float cursor_y = y;
    const float scale = target_pixel_height / menu_font_.pixel_height;
    
    const float cos_r = std::cos(rotation_radians);
    const float sin_r = std::sin(rotation_radians);
    
    const char* it = text.c_str();
    const char* end = it + text.size();
    while (it < end) {
        const std::uint32_t c = decode_utf8(it, end);
        if (c < menu_font_.glyphs.size()) {
            const MenuGlyph& glyph = menu_font_.glyphs[c];
            if (glyph.width > 0.0f && glyph.height > 0.0f) {
                const float x0 = cursor_x + glyph.bearing_x * scale;
                const float y0 = cursor_y + glyph.bearing_y * scale + target_pixel_height * 0.8f;
                const float x1 = x0 + glyph.width * scale;
                const float y1 = y0 + glyph.height * scale;
                
                auto transform = [&](float vx, float vy) -> std::pair<float, float> {
                    float dx = vx - x;
                    float dy = vy - y;
                    return {x + dx * cos_r - dy * sin_r, y + dx * sin_r + dy * cos_r};
                };
                
                std::pair<float, float> t0 = transform(x0, y0);
                std::pair<float, float> t1 = transform(x1, y0);
                std::pair<float, float> t2 = transform(x1, y1);
                std::pair<float, float> t3 = transform(x0, y1);
                
                const float nx0 = (t0.first / viewport_width) * 2.0f - 1.0f;
                const float ny0 = (t0.second / viewport_height) * 2.0f - 1.0f;
                const float nx1 = (t1.first / viewport_width) * 2.0f - 1.0f;
                const float ny1 = (t1.second / viewport_height) * 2.0f - 1.0f;
                const float nx2 = (t2.first / viewport_width) * 2.0f - 1.0f;
                const float ny2 = (t2.second / viewport_height) * 2.0f - 1.0f;
                const float nx3 = (t3.first / viewport_width) * 2.0f - 1.0f;
                const float ny3 = (t3.second / viewport_height) * 2.0f - 1.0f;
                
                vertices.push_back({{nx0, ny0, 0.0f}, color, {glyph.u0, glyph.v0}, 0});
                vertices.push_back({{nx3, ny3, 0.0f}, color, {glyph.u0, glyph.v1}, 0});
                vertices.push_back({{nx2, ny2, 0.0f}, color, {glyph.u1, glyph.v1}, 0});
                
                vertices.push_back({{nx0, ny0, 0.0f}, color, {glyph.u0, glyph.v0}, 0});
                vertices.push_back({{nx2, ny2, 0.0f}, color, {glyph.u1, glyph.v1}, 0});
                vertices.push_back({{nx1, ny1, 0.0f}, color, {glyph.u1, glyph.v0}, 0});
            }
            cursor_x += glyph.advance * scale;
        }
    }
}

}