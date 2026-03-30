#ifndef IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#endif

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

#define APP_VK_GLOBAL_FUNCTIONS(X) \
    X(vkEnumerateInstanceExtensionProperties) \
    X(vkCreateInstance)

#define APP_VK_INSTANCE_FUNCTIONS(X) \
    X(vkGetDeviceProcAddr) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkDestroySurfaceKHR)

#define APP_VK_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) \
    X(vkGetDeviceQueue) \
    X(vkCreateDescriptorPool) \
    X(vkDestroyDescriptorPool) \
    X(vkAcquireNextImageKHR) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkResetCommandPool) \
    X(vkBeginCommandBuffer) \
    X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) \
    X(vkEndCommandBuffer) \
    X(vkQueueSubmit) \
    X(vkQueuePresentKHR) \
    X(vkDeviceWaitIdle)

#define APP_DECLARE_VK_FUNCTION(func) static PFN_##func func = nullptr;
APP_VK_GLOBAL_FUNCTIONS(APP_DECLARE_VK_FUNCTION)
APP_VK_INSTANCE_FUNCTIONS(APP_DECLARE_VK_FUNCTION)
APP_VK_DEVICE_FUNCTIONS(APP_DECLARE_VK_FUNCTION)
#undef APP_DECLARE_VK_FUNCTION

static PFN_vkGetInstanceProcAddr g_VkGetInstanceProcAddr = nullptr;

static VkAllocationCallbacks* g_Allocator = nullptr;
static VkInstance g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice g_Device = VK_NULL_HANDLE;
static uint32_t g_QueueFamily = static_cast<uint32_t>(-1);
static VkQueue g_Queue = VK_NULL_HANDLE;
static VkPipelineCache g_PipelineCache = VK_NULL_HANDLE;
static VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;

static ImGui_ImplVulkanH_Window g_MainWindowData;
static uint32_t g_MinImageCount = 2;
static bool g_SwapChainRebuild = false;

struct VulkanLoaderContext
{
    VkInstance Instance = VK_NULL_HANDLE;
    VkDevice Device = VK_NULL_HANDLE;
};

static VulkanLoaderContext g_LoaderContext;

static ImFont* g_FontTitle = nullptr;
static ImFont* g_FontMenu = nullptr;
static ImFont* g_FontSubtitle = nullptr;
static ImFont* g_FontSplash = nullptr;
static int g_SelectedMenuItem = 0;
static int g_LastActivatedMenuItem = -1;
static uint64_t g_LastActivatedMenuTime = 0;

struct TextureSlot
{
    ImTextureData* Texture = nullptr;
    int Width = 0;
    int Height = 0;
};

enum class PanoramaVariant
{
    Day,
    Night,
};

static TextureSlot g_PanoramaTexture = {};
static TextureSlot g_MenuLogoTexture = {};
static bool g_PanoramaTexturesLoaded = false;
static bool g_PanoramaLoadAttempted = false;
static bool g_MenuLogoLoadAttempted = false;
static PanoramaVariant g_SelectedPanoramaVariant = PanoramaVariant::Day;

static constexpr const char* g_PanoramaDayFile = "assets\\panorama\\panorama_tu69_day.png";
static constexpr const char* g_PanoramaNightFile = "assets\\panorama\\panorama_tu69_night.png";
static constexpr const char* g_MenuLogoFile = "assets\\ui\\logo\\legacy_edition_logo.png";

static constexpr std::array<const char*, 5> g_MenuItems =
{
    "Play Game",
    "Mini Games",
    "Leaderboards",
    "Help & Options",
    "Minecraft Store",
};

static constexpr const char* g_SplashText = "What DOES the fox say?";

static std::string GetProjectAssetPath(const char* relative_path);
static void DrawBlurredBackground(const ImVec2& viewport_pos, const ImVec2& viewport_size);

struct Vec3
{
    float x;
    float y;
    float z;
};

struct ProjectedVertex
{
    ImVec2 position;
    float depth;
};

struct FaceDrawData
{
    std::array<ImVec2, 4> points;
    float depth;
    ImU32 color;
};

static void Fail(const char* message)
{
    std::fprintf(stderr, "%s\n", message);
    std::abort();
}

static void CheckVkResult(VkResult err)
{
    if (err == VK_SUCCESS)
    {
        return;
    }

    std::fprintf(stderr, "[vulkan] Error: VkResult = %d\n", err);
    if (err < 0)
    {
        std::abort();
    }
}

static PFN_vkVoidFunction LoadVulkanFunction(const char* function_name, void* user_data)
{
    const auto* loader_context = static_cast<const VulkanLoaderContext*>(user_data);
    if (loader_context != nullptr && loader_context->Device != VK_NULL_HANDLE && vkGetDeviceProcAddr != nullptr)
    {
        if (PFN_vkVoidFunction function = vkGetDeviceProcAddr(loader_context->Device, function_name))
        {
            return function;
        }
    }

    if (g_VkGetInstanceProcAddr != nullptr)
    {
        if (PFN_vkVoidFunction function = g_VkGetInstanceProcAddr(loader_context != nullptr ? loader_context->Instance : VK_NULL_HANDLE, function_name))
        {
            return function;
        }

        return g_VkGetInstanceProcAddr(VK_NULL_HANDLE, function_name);
    }

    return nullptr;
}

static bool LoadGlobalVulkanFunctions()
{
#define APP_LOAD_GLOBAL_FUNCTION(func) \
    func = reinterpret_cast<PFN_##func>(g_VkGetInstanceProcAddr(VK_NULL_HANDLE, #func)); \
    if (func == nullptr) \
    { \
        return false; \
    }
    APP_VK_GLOBAL_FUNCTIONS(APP_LOAD_GLOBAL_FUNCTION)
#undef APP_LOAD_GLOBAL_FUNCTION
    return true;
}

static bool LoadInstanceVulkanFunctions(VkInstance instance)
{
#define APP_LOAD_INSTANCE_FUNCTION(func) \
    func = reinterpret_cast<PFN_##func>(g_VkGetInstanceProcAddr(instance, #func)); \
    if (func == nullptr) \
    { \
        return false; \
    }
    APP_VK_INSTANCE_FUNCTIONS(APP_LOAD_INSTANCE_FUNCTION)
#undef APP_LOAD_INSTANCE_FUNCTION
    return true;
}

static bool LoadDeviceVulkanFunctions(VkDevice device)
{
#define APP_LOAD_DEVICE_FUNCTION(func) \
    func = reinterpret_cast<PFN_##func>(vkGetDeviceProcAddr(device, #func)); \
    if (func == nullptr) \
    { \
        func = reinterpret_cast<PFN_##func>(g_VkGetInstanceProcAddr(g_Instance, #func)); \
    } \
    if (func == nullptr) \
    { \
        return false; \
    }
    APP_VK_DEVICE_FUNCTIONS(APP_LOAD_DEVICE_FUNCTION)
#undef APP_LOAD_DEVICE_FUNCTION
    return true;
}

static bool IsExtensionAvailable(const ImVector<VkExtensionProperties>& properties, const char* extension)
{
    for (const VkExtensionProperties& property : properties)
    {
        if (std::strcmp(property.extensionName, extension) == 0)
        {
            return true;
        }
    }

    return false;
}

static VkPhysicalDevice SelectPhysicalDevice(VkInstance instance)
{
    uint32_t gpu_count = 0;
    VkResult err = vkEnumeratePhysicalDevices(instance, &gpu_count, nullptr);
    CheckVkResult(err);

    if (gpu_count == 0)
    {
        Fail("No Vulkan-compatible GPU was found.");
    }

    ImVector<VkPhysicalDevice> gpus;
    gpus.resize(gpu_count);
    err = vkEnumeratePhysicalDevices(instance, &gpu_count, gpus.Data);
    CheckVkResult(err);

    for (VkPhysicalDevice& device : gpus)
    {
        VkPhysicalDeviceProperties properties = {};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        {
            return device;
        }
    }

    return gpus[0];
}

static uint32_t SelectQueueFamilyIndex(VkPhysicalDevice physical_device)
{
    uint32_t queue_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, nullptr);

    ImVector<VkQueueFamilyProperties> queue_properties;
    queue_properties.resize(queue_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_count, queue_properties.Data);

    for (uint32_t index = 0; index < queue_count; ++index)
    {
        if ((queue_properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            return index;
        }
    }

    return static_cast<uint32_t>(-1);
}

static void SetupVulkan(ImVector<const char*> instance_extensions)
{
    VkResult err;

    {
        VkInstanceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;

        uint32_t property_count = 0;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateInstanceExtensionProperties(nullptr, &property_count, nullptr);
        properties.resize(property_count);
        err = vkEnumerateInstanceExtensionProperties(nullptr, &property_count, properties.Data);
        CheckVkResult(err);

        if (IsExtensionAvailable(properties, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
        {
            instance_extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        }

#ifdef VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
        {
            instance_extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            create_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#endif

        create_info.enabledExtensionCount = static_cast<uint32_t>(instance_extensions.Size);
        create_info.ppEnabledExtensionNames = instance_extensions.Data;
        err = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        CheckVkResult(err);
    }

    g_LoaderContext.Instance = g_Instance;

    if (!LoadInstanceVulkanFunctions(g_Instance))
    {
        Fail("Failed to load Vulkan instance functions.");
    }

    g_PhysicalDevice = SelectPhysicalDevice(g_Instance);
    g_QueueFamily = SelectQueueFamilyIndex(g_PhysicalDevice);
    if (g_QueueFamily == static_cast<uint32_t>(-1))
    {
        Fail("Failed to find a Vulkan graphics queue family.");
    }

    {
        ImVector<const char*> device_extensions;
        device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

        uint32_t property_count = 0;
        ImVector<VkExtensionProperties> properties;
        vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &property_count, nullptr);
        properties.resize(property_count);
        err = vkEnumerateDeviceExtensionProperties(g_PhysicalDevice, nullptr, &property_count, properties.Data);
        CheckVkResult(err);

        if (!IsExtensionAvailable(properties, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        {
            Fail("The selected Vulkan device does not support VK_KHR_swapchain.");
        }

#ifdef VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
        if (IsExtensionAvailable(properties, VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME))
        {
            device_extensions.push_back(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME);
        }
#endif

        const float queue_priority[] = { 1.0f };
        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = g_QueueFamily;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = queue_priority;

        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_info;
        create_info.enabledExtensionCount = static_cast<uint32_t>(device_extensions.Size);
        create_info.ppEnabledExtensionNames = device_extensions.Data;

        err = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_Device);
        CheckVkResult(err);
    }

    g_LoaderContext.Device = g_Device;

    if (!LoadDeviceVulkanFunctions(g_Device))
    {
        Fail("Failed to load Vulkan device functions.");
    }

    vkGetDeviceQueue(g_Device, g_QueueFamily, 0, &g_Queue);

    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, LoadVulkanFunction, &g_LoaderContext))
    {
        Fail("Failed to initialize Dear ImGui Vulkan function loader.");
    }

    {
        constexpr uint32_t texture_descriptor_count = 64;
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texture_descriptor_count },
        };

        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = texture_descriptor_count;
        pool_info.poolSizeCount = static_cast<uint32_t>(IM_ARRAYSIZE(pool_sizes));
        pool_info.pPoolSizes = pool_sizes;
        err = vkCreateDescriptorPool(g_Device, &pool_info, g_Allocator, &g_DescriptorPool);
        CheckVkResult(err);
    }
}

static void SetupVulkanWindow(ImGui_ImplVulkanH_Window* window_data, VkSurfaceKHR surface, int width, int height)
{
    VkBool32 supports_present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_PhysicalDevice, g_QueueFamily, surface, &supports_present);
    if (supports_present != VK_TRUE)
    {
        Fail("The selected Vulkan queue family cannot present to the SDL surface.");
    }

    const VkFormat request_surface_image_format[] =
    {
        VK_FORMAT_B8G8R8A8_UNORM,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_FORMAT_B8G8R8_UNORM,
        VK_FORMAT_R8G8B8_UNORM,
    };

    const VkColorSpaceKHR request_surface_color_space = VK_COLORSPACE_SRGB_NONLINEAR_KHR;
    window_data->Surface = surface;
    window_data->SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        g_PhysicalDevice,
        window_data->Surface,
        request_surface_image_format,
        static_cast<int>(IM_ARRAYSIZE(request_surface_image_format)),
        request_surface_color_space
    );

    const VkPresentModeKHR present_modes[] = { VK_PRESENT_MODE_FIFO_KHR };
    window_data->PresentMode = ImGui_ImplVulkanH_SelectPresentMode(
        g_PhysicalDevice,
        window_data->Surface,
        present_modes,
        static_cast<int>(IM_ARRAYSIZE(present_modes))
    );

    ImGui_ImplVulkanH_CreateOrResizeWindow(
        g_Instance,
        g_PhysicalDevice,
        g_Device,
        window_data,
        g_QueueFamily,
        g_Allocator,
        width,
        height,
        g_MinImageCount,
        0
    );
}

static void CleanupVulkan()
{
    if (g_DescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
        g_DescriptorPool = VK_NULL_HANDLE;
    }

    if (g_Device != VK_NULL_HANDLE)
    {
        vkDestroyDevice(g_Device, g_Allocator);
        g_Device = VK_NULL_HANDLE;
    }

    if (g_Instance != VK_NULL_HANDLE)
    {
        vkDestroyInstance(g_Instance, g_Allocator);
        g_Instance = VK_NULL_HANDLE;
    }
}

static void CleanupVulkanWindow(ImGui_ImplVulkanH_Window* window_data)
{
    ImGui_ImplVulkanH_DestroyWindow(g_Instance, g_Device, window_data, g_Allocator);
    if (window_data->Surface != VK_NULL_HANDLE)
    {
        vkDestroySurfaceKHR(g_Instance, window_data->Surface, g_Allocator);
        window_data->Surface = VK_NULL_HANDLE;
    }
}

static void FrameRender(ImGui_ImplVulkanH_Window* window_data, ImDrawData* draw_data)
{
    VkSemaphore image_acquired_semaphore = window_data->FrameSemaphores[window_data->SemaphoreIndex].ImageAcquiredSemaphore;
    VkSemaphore render_complete_semaphore = window_data->FrameSemaphores[window_data->SemaphoreIndex].RenderCompleteSemaphore;

    VkResult err = vkAcquireNextImageKHR(g_Device, window_data->Swapchain, UINT64_MAX, image_acquired_semaphore, VK_NULL_HANDLE, &window_data->FrameIndex);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    {
        g_SwapChainRebuild = true;
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return;
    }
    if (err != VK_SUBOPTIMAL_KHR)
    {
        CheckVkResult(err);
    }

    ImGui_ImplVulkanH_Frame* frame_data = &window_data->Frames[window_data->FrameIndex];
    err = vkWaitForFences(g_Device, 1, &frame_data->Fence, VK_TRUE, UINT64_MAX);
    CheckVkResult(err);

    err = vkResetFences(g_Device, 1, &frame_data->Fence);
    CheckVkResult(err);

    err = vkResetCommandPool(g_Device, frame_data->CommandPool, 0);
    CheckVkResult(err);

    VkCommandBufferBeginInfo begin_info = {};
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    err = vkBeginCommandBuffer(frame_data->CommandBuffer, &begin_info);
    CheckVkResult(err);

    VkRenderPassBeginInfo render_pass_info = {};
    render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_info.renderPass = window_data->RenderPass;
    render_pass_info.framebuffer = frame_data->Framebuffer;
    render_pass_info.renderArea.extent.width = window_data->Width;
    render_pass_info.renderArea.extent.height = window_data->Height;
    render_pass_info.clearValueCount = 1;
    render_pass_info.pClearValues = &window_data->ClearValue;
    vkCmdBeginRenderPass(frame_data->CommandBuffer, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(draw_data, frame_data->CommandBuffer);

    vkCmdEndRenderPass(frame_data->CommandBuffer);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &image_acquired_semaphore;
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &frame_data->CommandBuffer;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &render_complete_semaphore;

    err = vkEndCommandBuffer(frame_data->CommandBuffer);
    CheckVkResult(err);

    err = vkQueueSubmit(g_Queue, 1, &submit_info, frame_data->Fence);
    CheckVkResult(err);
}

static void FramePresent(ImGui_ImplVulkanH_Window* window_data)
{
    if (g_SwapChainRebuild)
    {
        return;
    }

    VkSemaphore render_complete_semaphore = window_data->FrameSemaphores[window_data->SemaphoreIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR present_info = {};
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &render_complete_semaphore;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &window_data->Swapchain;
    present_info.pImageIndices = &window_data->FrameIndex;

    VkResult err = vkQueuePresentKHR(g_Queue, &present_info);
    if (err == VK_ERROR_OUT_OF_DATE_KHR || err == VK_SUBOPTIMAL_KHR)
    {
        g_SwapChainRebuild = true;
    }
    if (err == VK_ERROR_OUT_OF_DATE_KHR)
    {
        return;
    }
    if (err != VK_SUBOPTIMAL_KHR)
    {
        CheckVkResult(err);
    }

    window_data->SemaphoreIndex = (window_data->SemaphoreIndex + 1) % window_data->SemaphoreCount;
}

static Vec3 Subtract(const Vec3& lhs, const Vec3& rhs)
{
    return { lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z };
}

static Vec3 Cross(const Vec3& lhs, const Vec3& rhs)
{
    return {
        lhs.y * rhs.z - lhs.z * rhs.y,
        lhs.z * rhs.x - lhs.x * rhs.z,
        lhs.x * rhs.y - lhs.y * rhs.x,
    };
}

static float Dot(const Vec3& lhs, const Vec3& rhs)
{
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

static Vec3 Normalize(const Vec3& value)
{
    const float length = std::sqrt(Dot(value, value));
    if (length <= 0.0f)
    {
        return { 0.0f, 0.0f, 0.0f };
    }

    return { value.x / length, value.y / length, value.z / length };
}

static Vec3 RotatePoint(const Vec3& value, float angle_x, float angle_y, float angle_z)
{
    const float cos_y = std::cos(angle_y);
    const float sin_y = std::sin(angle_y);
    Vec3 rotated =
    {
        value.x * cos_y + value.z * sin_y,
        value.y,
        -value.x * sin_y + value.z * cos_y,
    };

    const float cos_x = std::cos(angle_x);
    const float sin_x = std::sin(angle_x);
    rotated =
    {
        rotated.x,
        rotated.y * cos_x - rotated.z * sin_x,
        rotated.y * sin_x + rotated.z * cos_x,
    };

    const float cos_z = std::cos(angle_z);
    const float sin_z = std::sin(angle_z);
    return
    {
        rotated.x * cos_z - rotated.y * sin_z,
        rotated.x * sin_z + rotated.y * cos_z,
        rotated.z,
    };
}

static ProjectedVertex ProjectPoint(const Vec3& value, const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    constexpr float camera_distance = 4.2f;
    constexpr float field_of_view = 1.1f;
    const float depth = value.z + camera_distance;
    const float aspect = viewport_size.x / viewport_size.y;
    const float focal_length = 1.0f / std::tan(field_of_view * 0.5f);
    const float ndc_x = (value.x * focal_length / aspect) / depth;
    const float ndc_y = (value.y * focal_length) / depth;

    const float half_width = viewport_size.x * 0.5f;
    const float half_height = viewport_size.y * 0.5f;
    return {
        ImVec2(
            viewport_pos.x + half_width + ndc_x * half_width * 0.9f,
            viewport_pos.y + half_height - ndc_y * half_height * 0.9f
        ),
        depth,
    };
}

static ImU32 ScaleColor(ImU32 color, float factor)
{
    ImVec4 rgba = ImGui::ColorConvertU32ToFloat4(color);
    rgba.x = std::clamp(rgba.x * factor, 0.0f, 1.0f);
    rgba.y = std::clamp(rgba.y * factor, 0.0f, 1.0f);
    rgba.z = std::clamp(rgba.z * factor, 0.0f, 1.0f);
    rgba.w = 1.0f;
    return ImGui::ColorConvertFloat4ToU32(rgba);
}

static void ResetTextureSlot(TextureSlot& slot)
{
    if (slot.Texture != nullptr)
    {
        ImGui::UnregisterUserTexture(slot.Texture);
        IM_DELETE(slot.Texture);
        slot.Texture = nullptr;
    }

    slot.Width = 0;
    slot.Height = 0;
}

static void ResetPanoramaTextures()
{
    ResetTextureSlot(g_PanoramaTexture);

    g_PanoramaTexturesLoaded = false;
}

static void ResetMenuLogoTexture()
{
    ResetTextureSlot(g_MenuLogoTexture);
}

static PanoramaVariant ChoosePanoramaVariant()
{
    std::random_device random_device;
    std::mt19937 generator(random_device());
    std::uniform_int_distribution<int> distribution(1, 100);
    return distribution(generator) <= 10 ? PanoramaVariant::Night : PanoramaVariant::Day;
}

static const char* GetPanoramaFileForVariant(PanoramaVariant variant)
{
    return variant == PanoramaVariant::Night ? g_PanoramaNightFile : g_PanoramaDayFile;
}

static void DrawPanoramaLayer(
    ImDrawList* draw_list,
    const TextureSlot& slot,
    const ImVec2& viewport_pos,
    const ImVec2& viewport_size,
    float time,
    float zoom,
    float pan_speed,
    float phase,
    float vertical_sway,
    ImU32 tint)
{
    if (slot.Texture == nullptr || slot.Width <= 0 || slot.Height <= 0)
    {
        return;
    }

    const float texture_width = static_cast<float>(slot.Width);
    const float texture_height = static_cast<float>(slot.Height);
    const float fit_scale = std::max(viewport_size.x / texture_width, viewport_size.y / texture_height);
    const float scale = fit_scale * zoom;
    const float draw_width = texture_width * scale;
    const float draw_height = texture_height * scale;
    const float x_range = std::max(0.0f, draw_width - viewport_size.x);
    const float y_range = std::max(0.0f, draw_height - viewport_size.y);
    const float pan_phase = 0.5f + 0.5f * std::sin(time * pan_speed + phase);
    const float sway_phase = std::sin(time * (pan_speed * 0.55f) + phase * 1.7f);
    const float sway_range = std::min(viewport_size.y * vertical_sway, y_range * 0.5f);
    const float draw_x = viewport_pos.x - x_range * pan_phase;
    const float draw_y = viewport_pos.y - y_range * 0.5f - sway_phase * sway_range;

    draw_list->AddImage(
        slot.Texture->GetTexRef(),
        ImVec2(draw_x, draw_y),
        ImVec2(draw_x + draw_width, draw_y + draw_height),
        ImVec2(0.0f, 0.0f),
        ImVec2(1.0f, 1.0f),
        tint
    );
}

static bool LoadTextureSlot(TextureSlot& slot, const char* relative_path)
{
    const std::string path = GetProjectAssetPath(relative_path);
    SDL_Surface* source_surface = SDL_LoadSurface(path.c_str());
    if (source_surface == nullptr)
    {
        return false;
    }

    SDL_Surface* rgba_surface = SDL_ConvertSurface(source_surface, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(source_surface);
    if (rgba_surface == nullptr)
    {
        return false;
    }

    ImTextureData* texture = IM_NEW(ImTextureData)();
    texture->Create(ImTextureFormat_RGBA32, rgba_surface->w, rgba_surface->h);
    texture->UseColors = true;

    for (int y = 0; y < rgba_surface->h; ++y)
    {
        const unsigned char* src_row = static_cast<const unsigned char*>(rgba_surface->pixels) + y * rgba_surface->pitch;
        unsigned char* dst_row = texture->Pixels + y * texture->GetPitch();
        std::memcpy(dst_row, src_row, static_cast<size_t>(texture->GetPitch()));
    }

    texture->UsedRect = { 0, 0, static_cast<unsigned short>(texture->Width), static_cast<unsigned short>(texture->Height) };
    texture->UpdateRect = texture->UsedRect;
    ImGui::RegisterUserTexture(texture);
    ImGui_ImplVulkan_UpdateTexture(texture);

    SDL_DestroySurface(rgba_surface);

    if (texture->Status != ImTextureStatus_OK)
    {
        ImGui::UnregisterUserTexture(texture);
        IM_DELETE(texture);
        return false;
    }

    slot.Texture = texture;
    slot.Width = texture->Width;
    slot.Height = texture->Height;
    return true;
}

static bool EnsurePanoramaTexturesLoaded()
{
    if (g_PanoramaTexturesLoaded)
    {
        return true;
    }

    if (g_PanoramaLoadAttempted)
    {
        return false;
    }

    g_PanoramaLoadAttempted = true;
    g_SelectedPanoramaVariant = ChoosePanoramaVariant();

    if (!LoadTextureSlot(g_PanoramaTexture, GetPanoramaFileForVariant(g_SelectedPanoramaVariant)))
    {
        ResetPanoramaTextures();
        return false;
    }

    g_PanoramaTexturesLoaded = true;
    return true;
}

static bool EnsureMenuLogoTextureLoaded()
{
    if (g_MenuLogoTexture.Texture != nullptr)
    {
        return true;
    }

    if (g_MenuLogoLoadAttempted)
    {
        return false;
    }

    g_MenuLogoLoadAttempted = true;
    if (!LoadTextureSlot(g_MenuLogoTexture, g_MenuLogoFile))
    {
        ResetMenuLogoTexture();
        return false;
    }

    return true;
}

static void DrawPanoramaBackground(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    DrawBlurredBackground(viewport_pos, viewport_size);

    if (!EnsurePanoramaTexturesLoaded())
    {
        return;
    }

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const ImVec2 bottom_right = ImVec2(viewport_pos.x + viewport_size.x, viewport_pos.y + viewport_size.y);
    const float time = static_cast<float>(SDL_GetTicks()) * 0.001f;
    const bool is_night = g_SelectedPanoramaVariant == PanoramaVariant::Night;

    draw_list->PushClipRect(viewport_pos, bottom_right, false);
    DrawPanoramaLayer(
        draw_list,
        g_PanoramaTexture,
        viewport_pos,
        viewport_size,
        time,
        is_night ? 1.125f : 1.105f,
        is_night ? 0.040f : 0.050f,
        is_night ? 0.95f : 0.45f,
        0.012f,
        is_night ? IM_COL32(140, 164, 214, 22) : IM_COL32(255, 242, 214, 17)
    );
    DrawPanoramaLayer(
        draw_list,
        g_PanoramaTexture,
        viewport_pos,
        viewport_size,
        time,
        is_night ? 1.09f : 1.07f,
        is_night ? 0.040f : 0.050f,
        is_night ? 0.95f : 0.45f,
        0.010f,
        IM_COL32(255, 255, 255, 255)
    );
    draw_list->PopClipRect();

    draw_list->AddRectFilledMultiColor(
        viewport_pos,
        bottom_right,
        is_night ? IM_COL32(18, 28, 56, 88) : IM_COL32(118, 164, 214, 26),
        is_night ? IM_COL32(12, 22, 48, 78) : IM_COL32(112, 160, 208, 18),
        is_night ? IM_COL32(6, 10, 20, 198) : IM_COL32(24, 18, 12, 178),
        is_night ? IM_COL32(8, 12, 24, 208) : IM_COL32(32, 22, 14, 188)
    );
    draw_list->AddRectFilledMultiColor(
        viewport_pos,
        bottom_right,
        is_night ? IM_COL32(66, 90, 142, 24) : IM_COL32(255, 255, 255, 12),
        is_night ? IM_COL32(66, 90, 142, 18) : IM_COL32(255, 255, 255, 8),
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 0)
    );
    const float vignette_width = viewport_size.x * 0.20f;
    draw_list->AddRectFilledMultiColor(
        viewport_pos,
        ImVec2(viewport_pos.x + vignette_width, bottom_right.y),
        IM_COL32(0, 0, 0, is_night ? 70 : 34),
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, is_night ? 84 : 42)
    );
    draw_list->AddRectFilledMultiColor(
        ImVec2(bottom_right.x - vignette_width, viewport_pos.y),
        bottom_right,
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, is_night ? 66 : 30),
        IM_COL32(0, 0, 0, is_night ? 82 : 40),
        IM_COL32(0, 0, 0, 0)
    );
    draw_list->AddRectFilledMultiColor(
        ImVec2(viewport_pos.x, viewport_pos.y + viewport_size.y * 0.58f),
        bottom_right,
        IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 0),
        is_night ? IM_COL32(4, 6, 12, 96) : IM_COL32(14, 10, 8, 82),
        is_night ? IM_COL32(4, 6, 12, 104) : IM_COL32(14, 10, 8, 88)
    );
    draw_list->AddRectFilled(viewport_pos, bottom_right, IM_COL32(0, 0, 0, is_night ? 24 : 10));
}

static std::string GetProjectAssetPath(const char* relative_path)
{
    const char* base_path = SDL_GetBasePath();
    if (base_path == nullptr || base_path[0] == '\0')
    {
        return std::string(relative_path);
    }

    return std::string(base_path) + "..\\..\\" + relative_path;
}

static ImFont* LoadFontWithFallback(const char* primary_path, const char* fallback_path, float size_pixels, bool pixel_snap)
{
    ImFontConfig config = {};
    config.OversampleH = pixel_snap ? 1 : 3;
    config.OversampleV = pixel_snap ? 1 : 3;
    config.PixelSnapH = pixel_snap;
    config.RasterizerMultiply = 1.12f;

    ImGuiIO& io = ImGui::GetIO();
    if (primary_path != nullptr)
    {
        const std::string path = GetProjectAssetPath(primary_path);
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), size_pixels, &config))
        {
            return font;
        }
    }

    if (fallback_path != nullptr)
    {
        const std::string path = GetProjectAssetPath(fallback_path);
        if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), size_pixels, &config))
        {
            return font;
        }
    }

    return nullptr;
}

static void LoadMenuFonts(float main_scale)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    g_FontTitle = LoadFontWithFallback("bgfx\\examples\\runtime\\font\\visitor1.ttf", "imgui\\misc\\fonts\\DroidSans.ttf", 122.0f * main_scale, true);
    g_FontSubtitle = LoadFontWithFallback("bgfx\\examples\\runtime\\font\\visitor1.ttf", "imgui\\misc\\fonts\\ProggyClean.ttf", 26.0f * main_scale, true);
    g_FontMenu = LoadFontWithFallback("bgfx\\examples\\runtime\\font\\visitor1.ttf", "imgui\\misc\\fonts\\DroidSans.ttf", 30.0f * main_scale, true);
    g_FontSplash = LoadFontWithFallback("bgfx\\examples\\runtime\\font\\visitor1.ttf", "imgui\\misc\\fonts\\DroidSans.ttf", 42.0f * main_scale, true);

    if (g_FontTitle == nullptr || g_FontSubtitle == nullptr || g_FontMenu == nullptr || g_FontSplash == nullptr)
    {
        io.Fonts->Clear();
        g_FontMenu = io.Fonts->AddFontDefault();
        g_FontTitle = g_FontMenu;
        g_FontSubtitle = g_FontMenu;
        g_FontSplash = g_FontMenu;
    }
}

static ImFont* ResolveFont(ImFont* font)
{
    return font != nullptr ? font : ImGui::GetFont();
}

static ImVec2 MeasureText(ImFont* font, const char* text, float size = 0.0f)
{
    font = ResolveFont(font);
    const float font_size = size > 0.0f ? size : font->LegacySize;
    return font->CalcTextSizeA(font_size, 4096.0f, 0.0f, text);
}

static void DrawTextOutlined(ImDrawList* draw_list, ImFont* font, float size, const ImVec2& position, ImU32 color, ImU32 outline_color, float outline_thickness, const char* text)
{
    font = ResolveFont(font);
    if (outline_thickness > 0.0f)
    {
        static constexpr std::array<ImVec2, 8> offsets =
        {
            ImVec2(-1.0f, 0.0f),
            ImVec2(1.0f, 0.0f),
            ImVec2(0.0f, -1.0f),
            ImVec2(0.0f, 1.0f),
            ImVec2(-1.0f, -1.0f),
            ImVec2(1.0f, -1.0f),
            ImVec2(-1.0f, 1.0f),
            ImVec2(1.0f, 1.0f),
        };

        for (const ImVec2& offset : offsets)
        {
            draw_list->AddText(
                font,
                size,
                ImVec2(position.x + offset.x * outline_thickness, position.y + offset.y * outline_thickness),
                outline_color,
                text
            );
        }
    }

    draw_list->AddText(font, size, position, color, text);
}

static void DrawSlantedText(ImDrawList* draw_list, ImFont* font, float size, ImVec2 position, ImU32 color, ImU32 outline_color, float outline_thickness, float slope, const char* text)
{
    font = ResolveFont(font);
    for (const char* character = text; *character != '\0'; ++character)
    {
        if (*character == ' ')
        {
            position.x += size * 0.40f;
            position.y += slope * size * 0.40f;
            continue;
        }

        char glyph[2] = { *character, '\0' };
        DrawTextOutlined(draw_list, font, size, position, color, outline_color, outline_thickness, glyph);

        const ImVec2 glyph_size = font->CalcTextSizeA(size, 4096.0f, 0.0f, glyph);
        position.x += glyph_size.x * 0.78f;
        position.y += slope * glyph_size.x * 0.78f;
    }
}

static void DrawBlurredBackground(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const ImVec2 bottom_right = ImVec2(viewport_pos.x + viewport_size.x, viewport_pos.y + viewport_size.y);

    draw_list->AddRectFilledMultiColor(
        viewport_pos,
        bottom_right,
        IM_COL32(88, 103, 126, 255),
        IM_COL32(120, 116, 98, 255),
        IM_COL32(69, 74, 86, 255),
        IM_COL32(88, 95, 104, 255)
    );

    const float size_unit = std::min(viewport_size.x, viewport_size.y);
    const ImVec2 fortress_center = ImVec2(viewport_pos.x + viewport_size.x * 0.53f, viewport_pos.y + viewport_size.y * 0.55f);
    const ImU32 stone_dark = IM_COL32(88, 86, 88, 70);
    const ImU32 stone_mid = IM_COL32(148, 140, 130, 52);
    const ImU32 stone_light = IM_COL32(206, 190, 162, 34);

    draw_list->AddRectFilled(
        ImVec2(fortress_center.x - size_unit * 0.10f, viewport_pos.y + viewport_size.y * 0.15f),
        ImVec2(fortress_center.x + size_unit * 0.10f, viewport_pos.y + viewport_size.y * 0.95f),
        stone_mid,
        size_unit * 0.02f
    );
    draw_list->AddRectFilled(
        ImVec2(fortress_center.x - size_unit * 0.17f, viewport_pos.y + viewport_size.y * 0.28f),
        ImVec2(fortress_center.x - size_unit * 0.03f, viewport_pos.y + viewport_size.y * 0.70f),
        stone_dark,
        size_unit * 0.015f
    );
    draw_list->AddRectFilled(
        ImVec2(fortress_center.x + size_unit * 0.03f, viewport_pos.y + viewport_size.y * 0.28f),
        ImVec2(fortress_center.x + size_unit * 0.17f, viewport_pos.y + viewport_size.y * 0.70f),
        stone_dark,
        size_unit * 0.015f
    );
    draw_list->AddRectFilled(
        ImVec2(fortress_center.x - size_unit * 0.22f, viewport_pos.y + viewport_size.y * 0.58f),
        ImVec2(fortress_center.x + size_unit * 0.22f, viewport_pos.y + viewport_size.y * 0.67f),
        stone_light,
        size_unit * 0.018f
    );

    for (int i = 0; i < 14; ++i)
    {
        const float t = static_cast<float>(i) / 13.0f;
        const float x = viewport_pos.x + viewport_size.x * (0.12f + t * 0.76f);
        const float base_y = viewport_pos.y + viewport_size.y * (0.75f + std::sin(t * 12.0f) * 0.03f);
        const float width = size_unit * (0.03f + std::fmod(t * 5.3f, 1.0f) * 0.02f);
        const float height = size_unit * (0.12f + std::fmod(t * 7.1f, 1.0f) * 0.08f);
        draw_list->AddRectFilled(
            ImVec2(x - width, base_y - height),
            ImVec2(x + width, base_y + height * 0.18f),
            IM_COL32(184, 168, 132, 40),
            width * 0.35f
        );
    }

    for (int i = 0; i < 7; ++i)
    {
        const float t = static_cast<float>(i) / 6.0f;
        const float x = viewport_pos.x + viewport_size.x * (0.14f + t * 0.72f);
        const float trunk_w = size_unit * 0.016f;
        const float trunk_h = size_unit * (0.18f + (i % 3) * 0.02f);
        const float trunk_top = viewport_pos.y + viewport_size.y * (0.34f + (i % 2) * 0.04f);
        draw_list->AddRectFilled(
            ImVec2(x - trunk_w, trunk_top),
            ImVec2(x + trunk_w, trunk_top + trunk_h),
            IM_COL32(92, 73, 54, 48),
            trunk_w * 0.5f
        );
        draw_list->AddCircleFilled(ImVec2(x, trunk_top), size_unit * 0.07f, IM_COL32(84, 120, 82, 54), 24);
        draw_list->AddCircleFilled(ImVec2(x - size_unit * 0.04f, trunk_top + size_unit * 0.02f), size_unit * 0.05f, IM_COL32(96, 137, 92, 42), 24);
        draw_list->AddCircleFilled(ImVec2(x + size_unit * 0.04f, trunk_top + size_unit * 0.015f), size_unit * 0.048f, IM_COL32(72, 110, 78, 42), 24);
    }

    for (int i = 0; i < 5; ++i)
    {
        const float x = viewport_pos.x + viewport_size.x * (0.30f + i * 0.09f);
        const float top_y = viewport_pos.y + viewport_size.y * (0.78f + (i % 2) * 0.03f);
        draw_list->AddRectFilled(
            ImVec2(x - size_unit * 0.008f, top_y - size_unit * 0.10f),
            ImVec2(x + size_unit * 0.008f, top_y + size_unit * 0.02f),
            IM_COL32(62, 164, 198, 62),
            size_unit * 0.006f
        );
    }

    draw_list->AddRectFilledMultiColor(
        viewport_pos,
        bottom_right,
        IM_COL32(255, 255, 255, 0),
        IM_COL32(255, 255, 255, 0),
        IM_COL32(24, 18, 14, 140),
        IM_COL32(24, 18, 14, 140)
    );

    draw_list->AddRectFilled(viewport_pos, bottom_right, IM_COL32(180, 190, 210, 34));
}

static void DrawMinecraftLogo(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    if (!EnsureMenuLogoTextureLoaded())
    {
        return;
    }

    const float scale = std::min(viewport_size.x / 1280.0f, viewport_size.y / 720.0f);
    const float max_logo_width = std::clamp(viewport_size.x * 0.78f, 420.0f * scale, 760.0f * scale);
    const float logo_aspect = static_cast<float>(g_MenuLogoTexture.Width) / static_cast<float>(g_MenuLogoTexture.Height);
    const ImVec2 logo_size = ImVec2(max_logo_width, max_logo_width / logo_aspect);
    const ImVec2 logo_pos = ImVec2(viewport_pos.x + viewport_size.x * 0.5f - logo_size.x * 0.5f, viewport_pos.y + 26.0f * scale);

    draw_list->AddRectFilled(
        ImVec2(logo_pos.x + 12.0f * scale, logo_pos.y + logo_size.y * 0.55f),
        ImVec2(logo_pos.x + logo_size.x - 12.0f * scale, logo_pos.y + logo_size.y + 12.0f * scale),
        IM_COL32(12, 14, 18, 170),
        14.0f * scale
    );
    draw_list->AddImage(
        g_MenuLogoTexture.Texture->GetTexRef(),
        logo_pos,
        ImVec2(logo_pos.x + logo_size.x, logo_pos.y + logo_size.y)
    );

    const float splash_size = 42.0f * scale;
    const ImVec2 splash_pos = ImVec2(logo_pos.x + logo_size.x * 0.70f, logo_pos.y + 18.0f * scale);
    DrawSlantedText(draw_list, g_FontSplash, splash_size, splash_pos, IM_COL32(255, 242, 76, 255), IM_COL32(82, 70, 0, 255), 1.6f * scale, -0.18f, g_SplashText);
}

static void DrawMenuButtons(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

    const float scale = std::min(viewport_size.x / 1280.0f, viewport_size.y / 720.0f);
    const float menu_width = std::clamp(viewport_size.x * 0.35f, 380.0f * scale, 520.0f * scale);
    const float button_height = 52.0f * scale;
    const float spacing = 12.0f * scale;
    const float total_height = button_height * static_cast<float>(g_MenuItems.size()) + spacing * static_cast<float>(g_MenuItems.size() - 1);
    const ImVec2 menu_pos = ImVec2(viewport_pos.x + viewport_size.x * 0.5f - menu_width * 0.5f, viewport_pos.y + viewport_size.y * 0.46f - total_height * 0.22f);

    ImGui::SetNextWindowPos(menu_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(menu_width, total_height), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##start_menu", nullptr, flags);

    ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 original_button_text_align = style.ButtonTextAlign;
    style.ButtonTextAlign = ImVec2(0.5f, 0.5f);

    ImGui::PushFont(ResolveFont(g_FontMenu));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f * scale, 10.0f * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.5f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, spacing));
    ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(34, 34, 34, 255));
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(176, 176, 176, 218));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(175, 181, 236, 238));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(145, 151, 208, 244));
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(243, 243, 243, 255));

    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false))
    {
        g_SelectedMenuItem = (g_SelectedMenuItem + 1) % static_cast<int>(g_MenuItems.size());
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false))
    {
        g_SelectedMenuItem = (g_SelectedMenuItem + static_cast<int>(g_MenuItems.size()) - 1) % static_cast<int>(g_MenuItems.size());
    }

    for (size_t i = 0; i < g_MenuItems.size(); ++i)
    {
        const bool is_selected = static_cast<int>(i) == g_SelectedMenuItem;
        if (is_selected)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(186, 191, 242, 245));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(248, 242, 108, 255));
        }

        ImGui::SetCursorPosX(0.0f);
        if (ImGui::Button(g_MenuItems[i], ImVec2(menu_width, button_height)))
        {
            g_SelectedMenuItem = static_cast<int>(i);
            g_LastActivatedMenuItem = static_cast<int>(i);
            g_LastActivatedMenuTime = SDL_GetTicks();
        }

        if (ImGui::IsItemHovered())
        {
            g_SelectedMenuItem = static_cast<int>(i);
        }

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const ImVec2 item_max = ImGui::GetItemRectMax();
        draw_list->AddRect(item_min, item_max, IM_COL32(28, 28, 28, 255), 0.0f, 0, 2.0f * scale);
        draw_list->AddLine(item_min, ImVec2(item_max.x, item_min.y), IM_COL32(230, 230, 230, 86), 1.0f);
        draw_list->AddLine(ImVec2(item_min.x, item_max.y), item_max, IM_COL32(0, 0, 0, 120), 1.0f);

        if (is_selected)
        {
            draw_list->AddRectFilledMultiColor(
                item_min,
                item_max,
                IM_COL32(255, 255, 255, 18),
                IM_COL32(255, 255, 255, 18),
                IM_COL32(100, 112, 186, 16),
                IM_COL32(100, 112, 186, 16)
            );
            ImGui::PopStyleColor(2);
        }
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false))
    {
        g_LastActivatedMenuItem = g_SelectedMenuItem;
        g_LastActivatedMenuTime = SDL_GetTicks();
    }

    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    style.ButtonTextAlign = original_button_text_align;
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    if (g_LastActivatedMenuItem >= 0)
    {
        const uint64_t now = SDL_GetTicks();
        if (now - g_LastActivatedMenuTime < 2200)
        {
            ImDrawList* draw_list = ImGui::GetForegroundDrawList();
            const char* label = g_MenuItems[g_LastActivatedMenuItem];
            std::string message = std::string(label) + " selected";
            const ImVec2 message_size = MeasureText(g_FontSubtitle, message.c_str(), 28.0f * scale);
            const ImVec2 message_pos = ImVec2(viewport_pos.x + viewport_size.x * 0.5f - message_size.x * 0.5f, viewport_pos.y + viewport_size.y * 0.88f);
            draw_list->AddRectFilled(
                ImVec2(message_pos.x - 22.0f * scale, message_pos.y - 10.0f * scale),
                ImVec2(message_pos.x + message_size.x + 22.0f * scale, message_pos.y + message_size.y + 12.0f * scale),
                IM_COL32(16, 20, 26, 196),
                8.0f * scale
            );
            DrawTextOutlined(draw_list, g_FontSubtitle, 28.0f * scale, message_pos, IM_COL32(245, 245, 245, 255), IM_COL32(0, 0, 0, 255), 1.0f * scale, message.c_str());
        }
    }

    const char* footer = "X Select";
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    const float footer_size = 24.0f * scale;
    const ImVec2 footer_text_size = MeasureText(g_FontSubtitle, footer, footer_size);
    const ImVec2 footer_pos = ImVec2(viewport_pos.x + 32.0f * scale, viewport_pos.y + viewport_size.y - 42.0f * scale);
    draw_list->AddCircleFilled(ImVec2(footer_pos.x + 12.0f * scale, footer_pos.y + 14.0f * scale), 12.0f * scale, IM_COL32(38, 55, 102, 245), 20);
    DrawTextOutlined(draw_list, g_FontSubtitle, footer_size, ImVec2(footer_pos.x + 5.0f * scale, footer_pos.y + 2.0f * scale), IM_COL32(216, 225, 255, 255), IM_COL32(0, 0, 0, 255), 1.0f * scale, "X");
    DrawTextOutlined(draw_list, g_FontSubtitle, footer_size, ImVec2(footer_pos.x + 32.0f * scale, footer_pos.y), IM_COL32(236, 236, 236, 255), IM_COL32(0, 0, 0, 255), 1.0f * scale, "Select");
}

static void DrawStartMenu()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 viewport_pos = viewport->Pos;
    const ImVec2 viewport_size = viewport->Size;
    if (viewport_size.x <= 0.0f || viewport_size.y <= 0.0f)
    {
        return;
    }

    DrawPanoramaBackground(viewport_pos, viewport_size);
    DrawMinecraftLogo(viewport_pos, viewport_size);
    DrawMenuButtons(viewport_pos, viewport_size);
}

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
    {
        std::fprintf(stderr, "Error: SDL_Init(): %s\n", SDL_GetError());
        return 1;
    }

    if (!SDL_Vulkan_LoadLibrary(nullptr))
    {
        std::fprintf(stderr, "Error: SDL_Vulkan_LoadLibrary(): %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    g_VkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (g_VkGetInstanceProcAddr == nullptr || !LoadGlobalVulkanFunctions())
    {
        std::fprintf(stderr, "Error: failed to acquire Vulkan entry points.\n");
        SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
        return 1;
    }

    const float main_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    const SDL_WindowFlags window_flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    SDL_Window* window = SDL_CreateWindow("Minecraft Legacy", static_cast<int>(1280.0f * main_scale), static_cast<int>(800.0f * main_scale), window_flags);
    if (window == nullptr)
    {
        std::fprintf(stderr, "Error: SDL_CreateWindow(): %s\n", SDL_GetError());
        SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
        return 1;
    }

    ImVector<const char*> extensions;
    {
        uint32_t extension_count = 0;
        const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
        if (sdl_extensions == nullptr)
        {
            std::fprintf(stderr, "Error: SDL_Vulkan_GetInstanceExtensions(): %s\n", SDL_GetError());
            SDL_DestroyWindow(window);
            SDL_Vulkan_UnloadLibrary();
            SDL_Quit();
            return 1;
        }

        for (uint32_t index = 0; index < extension_count; ++index)
        {
            extensions.push_back(sdl_extensions[index]);
        }
    }

    SetupVulkan(extensions);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, g_Instance, g_Allocator, &surface))
    {
        std::fprintf(stderr, "Error: SDL_Vulkan_CreateSurface(): %s\n", SDL_GetError());
        CleanupVulkan();
        SDL_DestroyWindow(window);
        SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
        return 1;
    }

    int framebuffer_width = 0;
    int framebuffer_height = 0;
    SDL_GetWindowSizeInPixels(window, &framebuffer_width, &framebuffer_height);
    SetupVulkanWindow(&g_MainWindowData, surface, framebuffer_width, framebuffer_height);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    LoadMenuFonts(main_scale);

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    style.WindowRounding = 6.0f;
    style.FrameRounding = 0.0f;
    style.FrameBorderSize = 1.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.09f, 0.11f, 0.72f);

    ImGui_ImplSDL3_InitForVulkan(window);
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.ApiVersion = VK_API_VERSION_1_0;
    init_info.Instance = g_Instance;
    init_info.PhysicalDevice = g_PhysicalDevice;
    init_info.Device = g_Device;
    init_info.QueueFamily = g_QueueFamily;
    init_info.Queue = g_Queue;
    init_info.PipelineCache = g_PipelineCache;
    init_info.DescriptorPool = g_DescriptorPool;
    init_info.MinImageCount = g_MinImageCount;
    init_info.ImageCount = g_MainWindowData.ImageCount;
    init_info.Allocator = g_Allocator;
    init_info.PipelineInfoMain.RenderPass = g_MainWindowData.RenderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init_info.CheckVkResultFn = CheckVkResult;
    ImGui_ImplVulkan_Init(&init_info);

    ImVec4 clear_color = ImVec4(0.055f, 0.075f, 0.105f, 1.0f);
    bool done = false;
    while (!done)
    {
        SDL_Event event = {};
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT)
            {
                done = true;
            }
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))
            {
                done = true;
            }
        }

        if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0)
        {
            SDL_Delay(10);
            continue;
        }

        SDL_GetWindowSizeInPixels(window, &framebuffer_width, &framebuffer_height);
        if (framebuffer_width > 0 && framebuffer_height > 0 && (g_SwapChainRebuild || g_MainWindowData.Width != framebuffer_width || g_MainWindowData.Height != framebuffer_height))
        {
            ImGui_ImplVulkan_SetMinImageCount(g_MinImageCount);
            ImGui_ImplVulkanH_CreateOrResizeWindow(
                g_Instance,
                g_PhysicalDevice,
                g_Device,
                &g_MainWindowData,
                g_QueueFamily,
                g_Allocator,
                framebuffer_width,
                framebuffer_height,
                g_MinImageCount,
                0
            );
            g_MainWindowData.FrameIndex = 0;
            g_SwapChainRebuild = false;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        DrawStartMenu();

        ImGui::Render();
        ImDrawData* draw_data = ImGui::GetDrawData();
        const bool is_minimized = draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f;
        if (!is_minimized)
        {
            g_MainWindowData.ClearValue.color.float32[0] = clear_color.x * clear_color.w;
            g_MainWindowData.ClearValue.color.float32[1] = clear_color.y * clear_color.w;
            g_MainWindowData.ClearValue.color.float32[2] = clear_color.z * clear_color.w;
            g_MainWindowData.ClearValue.color.float32[3] = clear_color.w;
            FrameRender(&g_MainWindowData, draw_data);
            FramePresent(&g_MainWindowData);
        }
    }

    VkResult err = vkDeviceWaitIdle(g_Device);
    CheckVkResult(err);

    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ResetPanoramaTextures();
    ResetMenuLogoTexture();
    ImGui::DestroyContext();

    CleanupVulkanWindow(&g_MainWindowData);
    CleanupVulkan();
    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return 0;
}
