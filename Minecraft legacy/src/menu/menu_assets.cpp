#include "menu_assets.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"
#include "menu_state.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_filesystem.h>

#include <cstring>
#include <random>
#include <string>
#include <ctime>

namespace MenuInternal
{
// Билд запускается из x64/Debug или x64/Release, поэтому assets ищем относительно exe.
std::string GetProjectAssetPath(const char* relative_path)
{
    const char* base_path = SDL_GetBasePath();
    if (base_path == nullptr || base_path[0] == '\0')
    {
        return std::string(relative_path);
    }

    return std::string(base_path) + "..\\..\\" + relative_path;
}

// Перед удалением texture slot нужно отвязать от ImGui и обнулить размеры.
void ResetTextureSlot(TextureSlot& slot)
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

void ResetPanoramaTextures()
{
    ResetTextureSlot(g_PanoramaTexture);
    g_PanoramaTexturesLoaded = false;
}

void ResetButtonTextures()
{
    ResetTextureSlot(g_ButtonTexture);
    ResetTextureSlot(g_ButtonHighlightedTexture);
    ResetTextureSlot(g_ButtonDisabledTexture);
    g_ButtonTexturesLoaded = false;
    g_ButtonLoadAttempted = false;
}

void ResetMenuLogoTexture()
{
    ResetTextureSlot(g_MenuLogoTexture);
    g_MenuLogoLoadAttempted = false;
}

// Интро использует несколько экранов, поэтому сбрасываем сразу весь набор текстур.
void ResetIntroPhotoTextures()
{
    for (TextureSlot& slot : g_IntroPhotoTextures)
    {
        ResetTextureSlot(slot);
    }

    g_IntroPhotoLoadAttempted.fill(false);
}

// Вариант панорамы выбирается один раз на инициализацию меню.
PanoramaVariant ChoosePanoramaVariant()
{
    std::random_device random_device;
    std::mt19937 generator(random_device());
    std::uniform_int_distribution<int> distribution(1, 100);
    return distribution(generator) <= 10 ? PanoramaVariant::Night : PanoramaVariant::Day;
}

const char* GetPanoramaFileForVariant(PanoramaVariant variant)
{
    return variant == PanoramaVariant::Night ? g_PanoramaNightFile : g_PanoramaDayFile;
}

const char* GetMenuLogoFile()
{
    const time_t now = time(nullptr);
    tm local_time = {};
    localtime_s(&local_time, &now);

    const bool is_holiday_season =
        (local_time.tm_mon == 11 && local_time.tm_mday >= 20) ||
        (local_time.tm_mon == 0 && local_time.tm_mday <= 10);

    return is_holiday_season ? g_MenuLogoHolidayFile : g_MenuLogoFile;
}

// Загружает изображение через SDL и превращает его в пользовательскую ImGui/Vulkan texture.
bool LoadTextureSlot(TextureSlot& slot, const char* relative_path, bool use_nearest_sampling)
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
    // Для панорамы и pixel-art UI оставляем nearest, чтобы не размывать картинку.
    texture->UseNearestSampling = use_nearest_sampling;

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

// Панорама подгружается лениво и запоминает выбранный day/night вариант.
bool EnsurePanoramaTexturesLoaded()
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

    if (!LoadTextureSlot(g_PanoramaTexture, GetPanoramaFileForVariant(g_SelectedPanoramaVariant), true))
    {
        ResetPanoramaTextures();
        return false;
    }

    g_PanoramaTexturesLoaded = true;
    return true;
}

// Базовая кнопка обязательна, а highlighted/disabled считаются опциональными улучшениями.
bool EnsureButtonTexturesLoaded()
{
    if (g_ButtonTexturesLoaded)
    {
        return true;
    }

    if (g_ButtonLoadAttempted)
    {
        return false;
    }

    g_ButtonLoadAttempted = true;
    if (!LoadTextureSlot(g_ButtonTexture, g_ButtonNormalFile, true))
    {
        ResetButtonTextures();
        return false;
    }

    if (!LoadTextureSlot(g_ButtonHighlightedTexture, g_ButtonHighlightedFile, true))
    {
        ResetTextureSlot(g_ButtonHighlightedTexture);
    }

    if (!LoadTextureSlot(g_ButtonDisabledTexture, g_ButtonDisabledFile, true))
    {
        ResetTextureSlot(g_ButtonDisabledTexture);
    }

    g_ButtonTexturesLoaded = true;
    return true;
}

// Логотип меню один, но его файл может меняться в зависимости от сезона.
bool EnsureMenuLogoTextureLoaded()
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
    if (!LoadTextureSlot(g_MenuLogoTexture, GetMenuLogoFile()))
    {
        ResetMenuLogoTexture();
        return false;
    }

    return true;
}

// Сразу подготавливаем все стартовые экраны и просто пропускаем отсутствующие файлы.
bool EnsureIntroPhotoTexturesLoaded()
{
    bool has_any_intro_texture = false;
    for (std::size_t i = 0; i < g_IntroPhotoCount; ++i)
    {
        if (g_IntroPhotoTextures[i].Texture != nullptr)
        {
            has_any_intro_texture = true;
            continue;
        }

        if (g_IntroPhotoLoadAttempted[i])
        {
            continue;
        }

        g_IntroPhotoLoadAttempted[i] = true;
        if (LoadTextureSlot(g_IntroPhotoTextures[i], g_IntroPhotoFiles[i]))
        {
            has_any_intro_texture = true;
        }
        else
        {
            ResetTextureSlot(g_IntroPhotoTextures[i]);
        }
    }

    return has_any_intro_texture;
}
}
