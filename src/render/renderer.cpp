#include "render/renderer.hpp"

#include "common/log.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>

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

    FrameResources& frame = frames_[current_frame_];
    vkWaitForFences(device_, 1, &frame.in_flight, VK_TRUE, UINT64_MAX);

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
    vkCmdPushConstants(
        frame.command_buffer,
        pipeline_layout_,
        VK_SHADER_STAGE_VERTEX_BIT,
        0,
        sizeof(Mat4),
        current_camera_.view_proj.m.data()
    );

    frame_started_ = true;
}

void Renderer::upload_chunk_mesh(ChunkCoord coord, const ChunkMesh& mesh) {
    auto existing = chunk_buffers_.find(coord);
    if (existing != chunk_buffers_.end()) {
        vkDeviceWaitIdle(device_);
        destroy_buffer(existing->second.vertex_buffer);
        destroy_buffer(existing->second.index_buffer);
    }

    if (mesh.vertices.empty() || mesh.indices.empty()) {
        log_message(LogLevel::Warning, "Renderer: chunk mesh is empty");
        return;
    }

    const VkDeviceSize vertex_size = sizeof(Vertex) * mesh.vertices.size();
    const VkDeviceSize index_size = sizeof(std::uint32_t) * mesh.indices.size();

    ChunkRenderData render_data {};
    render_data.vertex_buffer = create_buffer(
        vertex_size,
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    render_data.index_buffer = create_buffer(
        index_size,
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    void* mapped = nullptr;
    vkMapMemory(device_, render_data.vertex_buffer.memory, 0, vertex_size, 0, &mapped);
    std::memcpy(mapped, mesh.vertices.data(), static_cast<std::size_t>(vertex_size));
    vkUnmapMemory(device_, render_data.vertex_buffer.memory);

    vkMapMemory(device_, render_data.index_buffer.memory, 0, index_size, 0, &mapped);
    std::memcpy(mapped, mesh.indices.data(), static_cast<std::size_t>(index_size));
    vkUnmapMemory(device_, render_data.index_buffer.memory);

    render_data.index_count = static_cast<std::uint32_t>(mesh.indices.size());
    chunk_buffers_[coord] = render_data;
}

void Renderer::draw_visible_chunks(std::span<const ActiveChunk> visible_chunks) {
    if (!frame_started_) {
        return;
    }

    const FrameResources& frame = frames_[current_frame_];
    const VkPipeline active_pipeline = (wireframe_enabled_ && wireframe_supported_ && wireframe_pipeline_ != VK_NULL_HANDLE)
        ? wireframe_pipeline_
        : fill_pipeline_;

    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, active_pipeline);

    if (descriptor_set_ != VK_NULL_HANDLE) {
        vkCmdBindDescriptorSets(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &descriptor_set_, 0, nullptr);
    }

    for (const ActiveChunk& chunk : visible_chunks) {
        auto it = chunk_buffers_.find(chunk.coord);
        if (it == chunk_buffers_.end()) {
            continue;
        }

        const VkBuffer vertex_buffers[] = {it->second.vertex_buffer.buffer};
        const VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
        vkCmdBindIndexBuffer(frame.command_buffer, it->second.index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(frame.command_buffer, it->second.index_count, 1, 0, 0, 0);
    }

    update_chunk_outline_buffer(visible_chunks);
    update_target_block_outline_buffer();
    update_crosshair_buffer();
    draw_chunk_outlines(frame);
    draw_target_block_outline(frame);
    draw_crosshair(frame);
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
        destroy_buffer(render_data.vertex_buffer);
        destroy_buffer(render_data.index_buffer);
    }
    chunk_buffers_.clear();
    destroy_buffer(chunk_outline_vertex_buffer_);
    destroy_buffer(target_block_outline_vertex_buffer_);
    destroy_buffer(crosshair_vertex_buffer_);

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
    if (crosshair_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, crosshair_pipeline_, nullptr);
    }
    if (wireframe_pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, wireframe_pipeline_, nullptr);
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

bool Renderer::wireframe_enabled() const {
    return wireframe_enabled_;
}

void Renderer::set_target_block(const std::optional<BlockHit>& target_block) {
    target_block_ = target_block;
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
            if (!logged_wireframe_support_) {
                log_message(LogLevel::Info, wireframe_supported_
                    ? "Renderer: fillModeNonSolid is supported"
                    : "Renderer: fillModeNonSolid is not supported, wireframe disabled");
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
    const auto hud_vertex_code = read_binary_file(shader_directory + "/hud.vert.spv");
    const auto color_fragment_code = read_binary_file(shader_directory + "/color.frag.spv");
    if (vertex_code.empty() || fragment_code.empty() || hud_vertex_code.empty() || color_fragment_code.empty()) {
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

    if (!create_graphics_pipeline(
            vertex_code,
            fragment_code,
            pipeline_layout_,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            VK_POLYGON_MODE_FILL,
            debug_disable_culling ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT,
            true,
            true,
            &fill_pipeline_)) {
        return false;
    }

    if (wireframe_supported_) {
        if (!create_graphics_pipeline(
                vertex_code,
                color_fragment_code,
                hud_pipeline_layout_, // Changed to hud_pipeline_layout since we don't bind descriptors for wireframe
                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                VK_POLYGON_MODE_LINE,
                VK_CULL_MODE_NONE,
                true,
                true,
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
            &block_outline_pipeline_)) {
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
    rasterizer.lineWidth = 1.0f;
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

void Renderer::draw_chunk_outlines(const FrameResources& frame) {
    if (chunk_outline_vertex_count_ == 0 || chunk_outline_pipeline_ == VK_NULL_HANDLE) {
        return;
    }

    vkCmdBindPipeline(frame.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, chunk_outline_pipeline_);
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
    const VkBuffer vertex_buffers[] = {target_block_outline_vertex_buffer_.buffer};
    const VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(frame.command_buffer, 0, 1, vertex_buffers, offsets);
    vkCmdDraw(frame.command_buffer, target_block_outline_vertex_count_, 1, 0, 0);
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

bool Renderer::load_textures() {
    const std::vector<std::string> texture_paths = {
        "assets/textures/texture_pack/classic/blocks/dirt.png",
        "assets/textures/texture_pack/classic/blocks/grass_carried.png",
        "assets/textures/texture_pack/classic/blocks/grass_side_carried.png",
        "assets/textures/texture_pack/classic/blocks/stone.png",
        "assets/textures/texture_pack/classic/blocks/water_placeholder.png",
        "assets/textures/texture_pack/classic/blocks/sand.png",
        "assets/textures/texture_pack/classic/blocks/gravel.png"
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

void Renderer::destroy_textures() {
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

}
