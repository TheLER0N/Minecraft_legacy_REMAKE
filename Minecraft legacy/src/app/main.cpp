#ifndef IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#endif

#include "../game/game.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

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

static void ToggleWindowFullscreen(SDL_Window* window)
{
    const bool is_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!SDL_SetWindowFullscreen(window, !is_fullscreen))
    {
        std::fprintf(stderr, "Error: SDL_SetWindowFullscreen(): %s\n", SDL_GetError());
        return;
    }

    g_SwapChainRebuild = true;
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

    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    if (!SDL_SetWindowFullscreen(window, true))
    {
        std::fprintf(stderr, "Error: SDL_SetWindowFullscreen(): %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
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
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

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

    if (!StartGame(main_scale))
    {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        CleanupVulkanWindow(&g_MainWindowData);
        CleanupVulkan();
        SDL_DestroyWindow(window);
        SDL_Vulkan_UnloadLibrary();
        SDL_Quit();
        return 1;
    }

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
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.windowID == SDL_GetWindowID(window) && event.key.key == SDLK_F11 && !event.key.repeat)
            {
                ToggleWindowFullscreen(window);
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

        RenderGame();

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

    ShutdownGame();
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    CleanupVulkanWindow(&g_MainWindowData);
    CleanupVulkan();
    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return 0;
}
