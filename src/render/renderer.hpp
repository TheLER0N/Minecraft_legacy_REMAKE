#pragma once

#include "game/world_types.hpp"
#include "platform/platform_app.hpp"

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ml {

class Renderer {
public:
    struct CaveVisibilityFrame {
        bool cave_mode {false};
        int camera_chunk_x {0};
        int camera_chunk_z {0};
        int camera_world_y {kSeaLevel};
        int roof_blocks {0};
    };

    struct DebugHudData {
        float fps {0.0f};
        Vec3 position {};
        bool debug_fly {false};
        std::size_t visible_chunks {0};
        std::size_t pending_uploads {0};
        std::size_t uploads_this_frame {0};
        std::size_t queued_rebuilds {0};
        std::size_t queued_generates {0};
        std::size_t queued_decorates {0};
        std::size_t queued_lights {0};
        std::size_t queued_meshes {0};
        std::size_t queued_fast_meshes {0};
        std::size_t queued_final_meshes {0};
        std::size_t pending_upload_bytes {0};
        std::size_t uploaded_bytes_this_frame {0};
        std::size_t stale_results {0};
        std::size_t stale_uploads {0};
        std::size_t provisional_uploads {0};
        std::size_t light_stale_results {0};
        std::size_t edge_fixups {0};
        std::size_t dropped_jobs {0};
        std::size_t dirty_save_chunks {0};
        bool light_borders_ready {false};
        int light_border_status {0};
        float generate_ms {0.0f};
        float light_ms {0.0f};
        float mesh_ms {0.0f};
        float upload_ms {0.0f};
        std::size_t drawn_chunks {0};
        std::size_t visible_sections {0};
        std::size_t drawn_sections {0};
        std::size_t frustum_culled_sections {0};
        std::size_t occlusion_culled_sections {0};
        std::size_t cave_culled_sections {0};
        std::size_t surface_culled_sections {0};
        std::size_t mixed_sections {0};
        bool cave_visibility_cave_mode {false};
        int cave_visibility_roof_blocks {0};
        std::size_t draw_calls {0};
        std::size_t drawn_vertices {0};
        std::size_t drawn_indices {0};
        std::size_t gpu_buffer_bytes {0};
        bool fancy_leaves {true};
    };

    ~Renderer();

    bool debug_disable_culling {false};
    bool debug_log_draw_stats {false};

    bool initialize(const PlatformWindow& window, const std::string& shader_directory);
    void begin_frame(const CameraFrameData& camera);
    void draw_startup_splash(float time_seconds, float fade_multiplier);
    void draw_main_menu(float time_seconds, bool use_night_panorama, int hovered_button);
    void upload_chunk_mesh(ChunkCoord coord, const ChunkMesh& mesh, const ChunkVisibilityMetadata& visibility);
    void unload_chunk_mesh(ChunkCoord coord);
    void set_cave_visibility_frame(const CaveVisibilityFrame& frame);
    void draw_visible_chunks(std::span<const ActiveChunk> visible_chunks);
    void end_frame();
    void shutdown();
    void toggle_wireframe();
    void toggle_wireframe_textures();
    void toggle_section_culling();
    void toggle_occlusion_culling();
    bool wireframe_enabled() const;
    void set_target_block(const std::optional<BlockHit>& target_block);
    void set_hotbar_state(std::size_t selected_slot, std::size_t slot_count);
    void set_debug_hud(bool enabled, const DebugHudData& data);

private:
    struct GpuBuffer {
        VkBuffer buffer {VK_NULL_HANDLE};
        VkDeviceMemory memory {VK_NULL_HANDLE};
        VkDeviceSize size {0};
        VkBufferUsageFlags usage {0};
        VkMemoryPropertyFlags properties {0};
    };

    struct RenderSection {
        GpuBuffer opaque_vertex_buffer;
        GpuBuffer opaque_index_buffer;
        std::uint32_t opaque_index_count {0};
        std::uint32_t opaque_vertex_count {0};
        GpuBuffer cutout_vertex_buffer;
        GpuBuffer cutout_index_buffer;
        std::uint32_t cutout_index_count {0};
        std::uint32_t cutout_vertex_count {0};
        GpuBuffer transparent_vertex_buffer;
        GpuBuffer transparent_index_buffer;
        std::uint32_t transparent_index_count {0};
        std::uint32_t transparent_vertex_count {0};
        bool has_geometry {false};
        bool has_opaque_geometry {false};
        ChunkSectionVisibility visibility {};
    };

    struct ChunkRenderData {
        std::array<RenderSection, kChunkSectionCount> sections {};
    };

    struct DeferredChunkBuffers {
        ChunkRenderData render_data;
        std::uint32_t frames_remaining {0};
    };

    struct FrameResources {
        VkCommandBuffer command_buffer {VK_NULL_HANDLE};
        VkSemaphore image_available {VK_NULL_HANDLE};
        VkSemaphore render_finished {VK_NULL_HANDLE};
        VkFence in_flight {VK_NULL_HANDLE};
    };

    struct MenuTexture {
        VkImage image {VK_NULL_HANDLE};
        VkDeviceMemory memory {VK_NULL_HANDLE};
        VkImageView view {VK_NULL_HANDLE};
        VkSampler sampler {VK_NULL_HANDLE};
        VkDescriptorPool descriptor_pool {VK_NULL_HANDLE};
        VkDescriptorSet descriptor_set {VK_NULL_HANDLE};
        std::uint32_t width {0};
        std::uint32_t height {0};
    };

    struct MenuGlyph {
        float u0 {0.0f};
        float v0 {0.0f};
        float u1 {0.0f};
        float v1 {0.0f};
        float width {0.0f};
        float height {0.0f};
        float advance {0.0f};
        float bearing_x {0.0f};
        float bearing_y {0.0f};
    };

    struct MenuFont {
        MenuTexture texture {};
        std::array<MenuGlyph, 1280> glyphs {};
        float pixel_height {32.0f};
        bool loaded {false};
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
        bool alpha_blend,
        VkPipeline* output_pipeline
    );
    bool create_depth_resources();
    bool create_framebuffers();
    bool create_command_pool();
    bool create_command_buffers();
    bool create_sync_objects();
    void destroy_swapchain_objects();
    VkExtent2D current_surface_extent(const VkSurfaceCapabilitiesKHR& capabilities) const;
    VkExtent2D logical_extent() const;
    bool recreate_swapchain_if_needed();
    void defer_destroy_chunk_buffers(ChunkRenderData&& render_data);
    void retire_deferred_chunk_buffers();
    void destroy_deferred_chunk_buffers_immediate();
    void upload_mesh_section(const MeshSection& mesh, GpuBuffer& vertex_buffer, GpuBuffer& index_buffer, std::uint32_t& index_count, std::uint32_t& vertex_count);
    void destroy_render_section(RenderSection& section);
    GpuBuffer acquire_chunk_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    void release_chunk_buffer(GpuBuffer& buffer);
    void destroy_pooled_chunk_buffers();
    GpuBuffer create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties);
    void destroy_buffer(GpuBuffer& buffer);
    std::uint32_t find_memory_type(std::uint32_t type_filter, VkMemoryPropertyFlags properties) const;
    VkShaderModule create_shader_module(const std::vector<char>& code) const;
    std::vector<char> read_binary_file(const std::string& path) const;
    Aabb chunk_bounds(ChunkCoord coord) const;
    Aabb chunk_section_bounds(ChunkCoord coord, int section_index) const;
    bool aabb_visible_in_current_frustum(const Aabb& bounds) const;
    Mat4 chunk_view_proj(ChunkCoord coord) const;
    void update_chunk_outline_buffer(std::span<const ActiveChunk> visible_chunks);
    void draw_chunk_outlines(const FrameResources& frame);
    void update_target_block_outline_buffer();
    void draw_target_block_outline(const FrameResources& frame);
    void update_crosshair_buffer();
    void draw_crosshair(const FrameResources& frame);
    void update_hotbar_buffer();
    void draw_hotbar(const FrameResources& frame);
    void update_debug_hud_buffer();
    void draw_debug_hud(const FrameResources& frame);
    void update_startup_splash_buffers(float time_seconds, float fade_multiplier);
    void update_main_menu_buffers(float time_seconds, bool use_night_panorama, int hovered_button);
    void draw_textured_buffer(const FrameResources& frame, const GpuBuffer& buffer, std::uint32_t vertex_count, VkDescriptorSet descriptor_set);
    void draw_colored_buffer(const FrameResources& frame, const GpuBuffer& buffer, std::uint32_t vertex_count, VkPipeline pipeline);
    void upload_dynamic_buffer(GpuBuffer& buffer, const std::vector<Vertex>& vertices);
    bool load_textures();
    bool load_ui_textures();
    bool load_menu_textures();
    bool load_menu_texture(const std::string& path, bool repeat, bool pixelated, MenuTexture& texture);
    bool load_menu_texture_from_rgba(const std::vector<std::uint8_t>& pixels, std::uint32_t width, std::uint32_t height, bool repeat, bool pixelated, MenuTexture& texture);
    bool load_menu_font();
    void destroy_menu_texture(MenuTexture& texture);
    void destroy_textures();
    void mark_dynamic_hud_dirty();
    float menu_font_text_width(const std::string& text, float pixel_height) const;
    void append_menu_font_text(std::vector<Vertex>& vertices, const std::string& text, float x, float y, float pixel_height, float width, float height, Vec3 color, float rotation_radians = 0.0f) const;

    VkInstance instance_ {VK_NULL_HANDLE};
    SDL_Window* window_handle_ {nullptr};
    VkPhysicalDevice physical_device_ {VK_NULL_HANDLE};
    VkDevice device_ {VK_NULL_HANDLE};
    VkQueue graphics_queue_ {VK_NULL_HANDLE};
    VkSurfaceKHR surface_ {VK_NULL_HANDLE};
    VkQueue present_queue_ {VK_NULL_HANDLE};
    std::uint32_t graphics_family_index_ {0};
    VkSwapchainKHR swapchain_ {VK_NULL_HANDLE};
    VkFormat swapchain_format_ {VK_FORMAT_UNDEFINED};
    bool swapchain_srgb_ {false};
    VkExtent2D swapchain_extent_ {};
    VkExtent2D logical_extent_ {};
    VkSurfaceTransformFlagBitsKHR swapchain_pre_transform_ {VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR};
    std::vector<VkImage> swapchain_images_;
    std::vector<VkImageView> swapchain_image_views_;
    std::vector<VkFramebuffer> swapchain_framebuffers_;
    VkRenderPass render_pass_ {VK_NULL_HANDLE};
    VkPipelineLayout pipeline_layout_ {VK_NULL_HANDLE};
    VkPipelineLayout hud_pipeline_layout_ {VK_NULL_HANDLE};
    VkPipelineLayout ui_pipeline_layout_ {VK_NULL_HANDLE};
    VkPipeline fill_pipeline_ {VK_NULL_HANDLE};
    VkPipeline cutout_pipeline_ {VK_NULL_HANDLE};
    VkPipeline water_pipeline_ {VK_NULL_HANDLE};
    VkPipeline wireframe_pipeline_ {VK_NULL_HANDLE};
    VkPipeline chunk_outline_pipeline_ {VK_NULL_HANDLE};
    VkPipeline block_outline_pipeline_ {VK_NULL_HANDLE};
    VkPipeline hotbar_fill_pipeline_ {VK_NULL_HANDLE};
    VkPipeline hotbar_outline_pipeline_ {VK_NULL_HANDLE};
    VkPipeline hotbar_texture_pipeline_ {VK_NULL_HANDLE};
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

    VkImage ui_texture_ {VK_NULL_HANDLE};
    VkDeviceMemory ui_texture_memory_ {VK_NULL_HANDLE};
    VkImageView ui_texture_view_ {VK_NULL_HANDLE};
    VkSampler ui_texture_sampler_ {VK_NULL_HANDLE};
    VkDescriptorSetLayout ui_descriptor_set_layout_ {VK_NULL_HANDLE};
    VkDescriptorPool ui_descriptor_pool_ {VK_NULL_HANDLE};
    VkDescriptorSet ui_descriptor_set_ {VK_NULL_HANDLE};
    MenuTexture menu_panorama_day_ {};
    MenuTexture menu_panorama_night_ {};
    MenuTexture menu_button_ {};
    MenuTexture menu_button_highlighted_ {};
    MenuTexture menu_logo_ {};
    MenuTexture startup_pic_ {};
    MenuTexture startup_mojang_ {};
    MenuTexture startup_king_ {};
    MenuFont menu_font_ {};

    std::unordered_map<ChunkCoord, ChunkRenderData, ChunkCoordHasher> chunk_buffers_;
    std::vector<GpuBuffer> chunk_buffer_pool_;
    std::vector<DeferredChunkBuffers> deferred_chunk_buffers_;
    GpuBuffer chunk_outline_vertex_buffer_ {};
    std::uint32_t chunk_outline_vertex_count_ {0};
    GpuBuffer target_block_outline_vertex_buffer_ {};
    std::uint32_t target_block_outline_vertex_count_ {0};
    GpuBuffer hotbar_fill_vertex_buffer_ {};
    std::uint32_t hotbar_fill_vertex_count_ {0};
    GpuBuffer hotbar_outline_vertex_buffer_ {};
    std::uint32_t hotbar_outline_vertex_count_ {0};
    GpuBuffer hotbar_texture_vertex_buffer_ {};
    std::uint32_t hotbar_texture_vertex_count_ {0};
    GpuBuffer crosshair_vertex_buffer_ {};
    std::uint32_t crosshair_vertex_count_ {0};
    GpuBuffer debug_hud_vertex_buffer_ {};
    std::uint32_t debug_hud_vertex_count_ {0};
    GpuBuffer menu_panorama_vertex_buffer_ {};
    std::uint32_t menu_panorama_vertex_count_ {0};
    GpuBuffer menu_logo_vertex_buffer_ {};
    std::uint32_t menu_logo_vertex_count_ {0};
    GpuBuffer menu_button_vertex_buffer_ {};
    std::uint32_t menu_button_vertex_count_ {0};
    GpuBuffer menu_button_highlight_vertex_buffer_ {};
    std::uint32_t menu_button_highlight_vertex_count_ {0};
    GpuBuffer menu_overlay_vertex_buffer_ {};
    std::uint32_t menu_overlay_vertex_count_ {0};
    GpuBuffer menu_text_vertex_buffer_ {};
    std::uint32_t menu_text_vertex_count_ {0};
    GpuBuffer menu_font_vertex_buffer_ {};
    std::uint32_t menu_font_vertex_count_ {0};
    GpuBuffer startup_splash_vertex_buffer_ {};
    std::uint32_t startup_splash_vertex_count_ {0};
    GpuBuffer startup_splash_background_vertex_buffer_ {};
    std::uint32_t startup_splash_background_vertex_count_ {0};
    std::uint32_t startup_splash_texture_index_ {0};
    VkViewport viewport_ {};
    VkRect2D scissor_ {};
    VkExtent2D dynamic_hud_extent_ {};
    bool logged_push_constant_size_ {false};
    bool logged_draw_stats_ {false};
    std::size_t logged_upload_count_ {0};
    bool wireframe_supported_ {false};
    bool wide_lines_supported_ {false};
    bool wireframe_enabled_ {false};
    bool wireframe_textures_enabled_ {false};
    bool section_culling_enabled_ {true};
    bool occlusion_culling_enabled_ {false};
    bool logged_wireframe_support_ {false};
    std::size_t logged_occlusion_warning_count_ {0};
    std::size_t logged_cave_culling_warning_count_ {0};
    std::optional<BlockHit> target_block_ {};
    std::size_t hotbar_selected_slot_ {0};
    std::size_t hotbar_slot_count_ {9};
    bool debug_hud_enabled_ {false};
    DebugHudData debug_hud_data_ {};
    CaveVisibilityFrame cave_visibility_frame_ {};
    std::size_t last_drawn_chunks_ {0};
    bool target_block_dirty_ {true};
    bool hotbar_dirty_ {true};
    bool crosshair_dirty_ {true};
    bool debug_hud_dirty_ {true};
};

}
