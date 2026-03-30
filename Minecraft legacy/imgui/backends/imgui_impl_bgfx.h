// dear imgui: Renderer Backend for modern OpenGL with shaders / programmatic pipeline
// - Desktop GL: 2.x 3.x 4.x
// - Embedded GL: ES 2.0 (WebGL 1.0), ES 3.0 (WebGL 2.0)
// This needs to be used along with a Platform Backend (e.g. GLFW, SDL, Win32, custom..)

// Implemented features:
//  [X] Renderer: User texture binding. Use 'GLuint' OpenGL texture identifier as void*/ImTextureID. Read the FAQ about ImTextureID!
//  [X] Renderer: Multi-viewport support (multiple windows). Enable with 'io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable'.
//  [x] Renderer: Large meshes support (64k+ vertices) with 16-bit indices (Desktop OpenGL only).

// You can use unmodified imgui_impl_* files in your project. See examples/ folder for examples of using this.
// Prefer including the entire imgui/ repository into your project (either as a copy or as a submodule), and only build the backends you need.
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

// About GLSL version:
//  The 'glsl_version' initialization parameter should be nullptr (default) or a "#version XXX" string.
//  On computer platform the GLSL version default to "#version 130". On OpenGL ES 3 platform it defaults to "#version 300 es"
//  Only override if your GL version doesn't handle this GLSL version. See GLSL version table at the top of imgui_impl_Bgfx.cpp.

#pragma once
#include "imgui.h"      // IMGUI_IMPL_API
#include <stdint.h>

#include <bgfx/bgfx.h>

// Backend API
IMGUI_IMPL_API bool     ImGui_ImplBgfx_Init(uint16_t viewId);
IMGUI_IMPL_API void     ImGui_ImplBgfx_Shutdown();
IMGUI_IMPL_API void     ImGui_ImplBgfx_NewFrame();
IMGUI_IMPL_API void     ImGui_ImplBgfx_RenderDrawData(ImDrawData* draw_data);

// (Optional) Called by Init/NewFrame/Shutdown
IMGUI_IMPL_API bool     ImGui_ImplBgfx_CreateFontsTexture();
IMGUI_IMPL_API void     ImGui_ImplBgfx_DestroyFontsTexture();
IMGUI_IMPL_API bool     ImGui_ImplBgfx_CreateDeviceObjects();
IMGUI_IMPL_API void     ImGui_ImplBgfx_DestroyDeviceObjects();

namespace ImGui
{
    struct TextureBgfx
    {
        bgfx::TextureHandle handle;
        uint8_t  flags;
        uint8_t  mip;
        uint32_t unused;
    };

    IMGUI_IMPL_API void     Image(bgfx::TextureHandle handle, const ImVec2& size, const ImVec2& uv0 = ImVec2(0.0f, 0.0f), const ImVec2& uv1 = ImVec2(1.0f, 1.0f), const ImVec4& tintCol = ImVec4(1.0f, 1.0f, 1.0f, 1.0f), const ImVec4& borderCol = ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
}
