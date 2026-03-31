#pragma once

#include <SDL3/SDL.h>

struct AppVulkanContext;
typedef struct VkCommandBuffer_T* VkCommandBuffer;

bool InitializeWorldSystem(float main_scale);
void ShutdownWorldSystem();

bool InitializeWorldRenderer(const AppVulkanContext& context);
void ShutdownWorldRenderer();
void OnWorldRendererSwapchainChanged();

bool CreateWorldFromMenu(const char* world_name, const char* seed_text);
bool LoadWorldFromMenu(const char* world_directory_name);
void SaveCurrentWorld();
void LeaveWorld();
bool IsWorldLoaded();

void HandleWorldEvent(const SDL_Event& event, SDL_Window* window);
void UpdateWorld(float delta_seconds, SDL_Window* window);
void RenderWorld();
void RenderWorldVulkan(VkCommandBuffer command_buffer, int framebuffer_width, int framebuffer_height);
