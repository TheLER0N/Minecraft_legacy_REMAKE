#pragma once

#include "game/world_types.hpp"
#include "platform/platform_app.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ml {

class Renderer {
public:
    ~Renderer();

    bool debug_disable_culling {false};
    bool debug_log_draw_stats {false};

    bool initialize(const PlatformWindow& window, const std::string& shader_directory);
    void begin_frame(const CameraFrameData& camera);
    void upload_chunk_mesh(ChunkCoord coord, const ChunkMesh& mesh);
    void draw_visible_chunks(std::span<const ActiveChunk> visible_chunks);
    void end_frame();
    void shutdown();
    void toggle_wireframe();
    bool wireframe_enabled() const;
    void set_target_block(const std::optional<BlockHit>& target_block);

private:
    struct GpuBuffer {
        VkBuffer buffer {VK_NULL_HANDLE};
        VkDeviceMemory memory {VK_NULL_HANDLE};
        VkDeviceSize size {0};
    };

    struct ChunkRenderData {
        GpuBuffer vertex_buffer;
        GpuBuffer index_buffer;
        std::uint32_t index_count {0};
    };

    struct FrameResources {
        VkCommandBuffer command_buffer {VK_NULL_HANDLE};
        VkSemaphore image_available {VK_NULL_HANDLE};
        VkSemaphore render_finished {VK_NULL_HANDLE};
        VkFence in_flight {VK_NULL_HANDLE};
    };

    bool create_instance();
    bool create_surface(const PlatformWindow& window);
    bool pick_physical_device();
    bool create_device();
    bool create_swapchain();
    bool create_image_views();
    bool create_render_pass();
    bool create_pipeline(const std::string& shader_directory);
    bool create_graphics_pipeline(
        const std::vector<char>& vertex_code,
        const std::vector<char>& fragment_code,
        VkPipelineLayout layout,
        VkPrimitiveTopology topology,
        VkPolygonMode polygon_mode,
        VkCullModeFlags cull_mode,
        bool depth_test,
        bool depth_write,
        VkPipeline* output_pipeline
    );
    bool create_depth_resources();
    bool create_framebuffers();
    bool create_command_pool();
    bool create_command_buffers();
    bool create_sync_objects();
    void destroy_swapchain_objects();
    bool recreate_swapchain_if_needed();
    GpuBuffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    void destroy_buffer(GpuBuffer& buffer);
    std::uint32_t find_memory_type(std::uint32_t type_filter, VkMemoryPropertyFlags properties) const;
    VkShaderModule create_shader_module(const std::vector<char>& code) const;
    std::vector<char> read_binary_file(const std::string& path) const;
    void update_chunk_outline_buffer(std::span<const ActiveChunk> visible_chunks);
    void draw_chunk_outlines(const FrameResources& frame);
    void update_target_block_outline_buffer();
    void draw_target_block_outline(const FrameResources& frame);
    void update_crosshair_buffer();
    void draw_crosshair(const FrameResources& frame);
    void upload_dynamic_buffer(GpuBuffer& buffer, const std::vector<Vertex>& vertices);
    bool load_textures();
    void destroy_textures();

    VkInstance instance_ {VK_NULL_HANDLE};
    VkPhysicalDevice physical_device_ {VK_NULL_HANDLE};
    VkDevice device_ {VK_NULL_HANDLE};
    VkQueue graphics_queue_ {VK_NULL_HANDLE};
    VkSurfaceKHR surface_ {VK_NULL_HANDLE};
    VkQueue present_queue_ {VK_NULL_HANDLE};
    std::uint32_t graphics_family_index_ {0};
    VkSwapchainKHR swapchain_ {VK_NULL_HANDLE};
    VkFormat swapchain_format_ {VK_FORMAT_UNDEFINED};
    VkExtent2D swapchain_extent_ {};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    std::vector<VkFramebuffer> swapchain_framebuffers_;
    VkRenderPass render_pass_ {VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_ {VK_NULL_HANDLE};
    VkPipelineLayout hud_pipeline_layout_ {VK_NULL_HANDLE};
    VkPipeline fill_pipeline_ {VK_NULL_HANDLE};
    VkPipeline wireframe_pipeline_ {VK_NULL_HANDLE};
    VkPipeline chunk_outline_pipeline_ {VK_NULL_HANDLE};
    VkPipeline block_outline_pipeline_ {VK_NULL_HANDLE};
    VkPipeline crosshair_pipeline_ {VK_NULL_HANDLE};
    VkCommandPool command_pool_ {VK_NULL_HANDLE};
    std::vector<FrameResources> frames_;
    std::size_t current_frame_ {0};
    std::uint32_t current_image_index_ {0};
    CameraFrameData current_camera_ {};
    bool frame_started_ {false};

    VkImage depth_image_ {VK_NULL_HANDLE};
    VkDeviceMemory depth_memory_ {VK_NULL_HANDLE};
    VkImageView depth_view_ {VK_NULL_HANDLE};
    VkFormat depth_format_ {VK_FORMAT_D32_SFLOAT};

    VkImage texture_array_ {VK_NULL_HANDLE};
    VkDeviceMemory texture_memory_ {VK_NULL_HANDLE};
    VkImageView texture_view_ {VK_NULL_HANDLE};
    VkSampler texture_sampler_ {VK_NULL_HANDLE};

    VkDescriptorSetLayout descriptor_set_layout_ {VK_NULL_HANDLE};
    VkDescriptorPool descriptor_pool_ {VK_NULL_HANDLE};
    VkDescriptorSet descriptor_set_ {VK_NULL_HANDLE};

    std::unordered_map<ChunkCoord, ChunkRenderData, ChunkCoordHasher> chunk_buffers_;
    GpuBuffer chunk_outline_vertex_buffer_ {};
    std::uint32_t chunk_outline_vertex_count_ {0};
    GpuBuffer target_block_outline_vertex_buffer_ {};
    std::uint32_t target_block_outline_vertex_count_ {0};
    GpuBuffer crosshair_vertex_buffer_ {};
    std::uint32_t crosshair_vertex_count_ {0};
    VkViewport viewport_ {};
    VkRect2D scissor_ {};
    bool logged_push_constant_size_ {false};
    bool logged_draw_stats_ {false};
    std::size_t logged_upload_count_ {0};
    bool wireframe_supported_ {false};
    bool wireframe_enabled_ {false};
    bool logged_wireframe_support_ {false};
    std::optional<BlockHit> target_block_ {};
};

}
