#include "menu_background.h"

#include "imgui.h"
#include "menu_state.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace MenuInternal
{
namespace
{
// Эти коэффициенты задают базовую плотность фоновых элементов относительно окна.
constexpr float kMainMenuBaseScale = 0.8f;
constexpr float kLogoScaleAdjust = 0.924f;

// Проверяем, что texture slot действительно готов к отрисовке.
bool HasValidTexture(const TextureSlot& slot)
{
    return slot.Texture != nullptr && slot.Width > 0 && slot.Height > 0;
}

// Режим cover закрывает весь экран без чёрных полос, даже если часть изображения обрежется.
void DrawCoverTexture(ImDrawList* draw_list, const TextureSlot& slot, const ImVec2& viewport_pos, const ImVec2& viewport_size, ImU32 tint)
{
    if (!HasValidTexture(slot))
    {
        return;
    }

    const float texture_width = static_cast<float>(slot.Width);
    const float texture_height = static_cast<float>(slot.Height);
    const float scale = std::max(viewport_size.x / texture_width, viewport_size.y / texture_height);
    const ImVec2 image_size = ImVec2(texture_width * scale, texture_height * scale);
    const ImVec2 image_pos = ImVec2(viewport_pos.x + (viewport_size.x - image_size.x) * 0.5f, viewport_pos.y + (viewport_size.y - image_size.y) * 0.5f);
    draw_list->AddImage(slot.Texture->GetTexRef(), image_pos, ImVec2(image_pos.x + image_size.x, image_pos.y + image_size.y), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
}
}

// Этот файл отвечает за всё, что рисуется "под" или "вокруг" меню:
// стартовые заставки, панораму, логотип и жёлтый splash.

// Панорама не просто растягивается на экран, а медленно "дышит" и плавает по горизонтали.
void DrawPanoramaLayer(ImDrawList* draw_list, const TextureSlot& slot, const ImVec2& viewport_pos, const ImVec2& viewport_size, float time, float zoom, float pan_speed, float phase, float vertical_sway, ImU32 tint)
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
    // pan_phase двигает изображение слева направо и обратно без резкого скачка в конце цикла.
    const float pan_cycle = std::fmod(time * pan_speed, 2.0f);
    const float pan_phase = pan_cycle <= 1.0f ? pan_cycle : 2.0f - pan_cycle;
    // Дополнительный небольшой sway нужен, чтобы фон не выглядел как строго линейный скролл.
    const float sway_phase = std::sin(time * (pan_speed * 0.55f) + phase * 1.7f);
    const float sway_range = std::min(viewport_size.y * vertical_sway, y_range * 0.5f);
    const float draw_x = viewport_pos.x - x_range * pan_phase;
    const float draw_y = viewport_pos.y - y_range * 0.5f - sway_phase * sway_range;

    draw_list->AddImage(slot.Texture->GetTexRef(), ImVec2(draw_x, draw_y), ImVec2(draw_x + draw_width, draw_y + draw_height), ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
}

// Одна и та же кривая прозрачности используется для каждого интро-экрана: fade in -> hold -> fade out.
float GetIntroPhotoAlpha(uint64_t elapsed_ms)
{
    if (elapsed_ms >= g_IntroSceneDurationMs)
    {
        return 0.0f;
    }

    if (elapsed_ms < g_IntroFadeInDurationMs)
    {
        return static_cast<float>(elapsed_ms) / static_cast<float>(g_IntroFadeInDurationMs);
    }

    if (elapsed_ms < g_IntroFadeInDurationMs + g_IntroHoldDurationMs)
    {
        return 1.0f;
    }

    const uint64_t fade_out_elapsed = elapsed_ms - g_IntroFadeInDurationMs - g_IntroHoldDurationMs;
    return 1.0f - static_cast<float>(fade_out_elapsed) / static_cast<float>(g_IntroFadeOutDurationMs);
}

void DrawPanoramaFallbackBackground(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const ImVec2 bottom_right = ImVec2(viewport_pos.x + viewport_size.x, viewport_pos.y + viewport_size.y);

    // If panorama loading fails, keep the background flat instead of falling back to a soft-painted image.
    draw_list->AddRectFilled(viewport_pos, bottom_right, IM_COL32(0, 0, 0, 255));
}

// Интро блокирует меню, пока не будут показаны все стартовые экраны по порядку.
bool DrawStartupIntro(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    if (g_IntroFinished)
    {
        return false;
    }

    // Если ни один экран интро не загрузился, не зависаем на чёрном фоне, а сразу идём в меню.
    if (!EnsureIntroPhotoTexturesLoaded())
    {
        g_IntroFinished = true;
        return false;
    }

    // Собираем только те сцены, которые реально удалось загрузить.
    std::array<const TextureSlot*, g_IntroPhotoCount> active_intro_textures = {};
    std::size_t active_intro_count = 0;
    for (const TextureSlot& slot : g_IntroPhotoTextures)
    {
        if (HasValidTexture(slot))
        {
            active_intro_textures[active_intro_count++] = &slot;
        }
    }

    if (active_intro_count == 0)
    {
        g_IntroFinished = true;
        return false;
    }

    const uint64_t now = SDL_GetTicks();
    const uint64_t elapsed_ms = now - g_IntroStartTime;
    const uint64_t intro_total_duration_ms = g_IntroSceneDurationMs * static_cast<uint64_t>(active_intro_count);
    if (elapsed_ms >= intro_total_duration_ms)
    {
        // Как только вся последовательность отыграла, начиная со следующего кадра рендерится обычное меню.
        g_IntroFinished = true;
        return false;
    }

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const ImVec2 bottom_right = ImVec2(viewport_pos.x + viewport_size.x, viewport_pos.y + viewport_size.y);
    draw_list->AddRectFilled(viewport_pos, bottom_right, IM_COL32(0, 0, 0, 255));

    // Каждые g_IntroSceneDurationMs переключаемся на следующий экран из активной последовательности.
    const std::size_t scene_index = static_cast<std::size_t>(elapsed_ms / g_IntroSceneDurationMs);
    const uint64_t scene_elapsed_ms = elapsed_ms % g_IntroSceneDurationMs;
    const float alpha = std::clamp(GetIntroPhotoAlpha(scene_elapsed_ms), 0.0f, 1.0f);
    const int alpha_u8 = static_cast<int>(std::round(alpha * 255.0f));
    // Рисуем текущий экран интро поверх чёрного фона с вычисленной прозрачностью.
    DrawCoverTexture(draw_list, *active_intro_textures[scene_index], viewport_pos, viewport_size, IM_COL32(255, 255, 255, alpha_u8));

    return true;
}

void DrawPanoramaBackground(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    if (!EnsurePanoramaTexturesLoaded())
    {
        DrawPanoramaFallbackBackground(viewport_pos, viewport_size);
        return;
    }

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const ImVec2 bottom_right = ImVec2(viewport_pos.x + viewport_size.x, viewport_pos.y + viewport_size.y);
    // Таймер анимации запускается только после того, как интро полностью закончилось.
    if (g_PanoramaAnimationStartTime == 0)
    {
        g_PanoramaAnimationStartTime = SDL_GetTicks();
    }

    const float time = static_cast<float>(SDL_GetTicks() - g_PanoramaAnimationStartTime) * 0.001f;
    const bool is_night = g_SelectedPanoramaVariant == PanoramaVariant::Night;

    // PushClipRect не даёт случайно вылезти изображению за пределы окна при cover-отрисовке.
    draw_list->PushClipRect(viewport_pos, bottom_right, false);
    DrawPanoramaLayer(draw_list, g_PanoramaTexture, viewport_pos, viewport_size, time, 1.0f, is_night ? 0.010f : 0.012f, is_night ? 0.95f : 0.45f, 0.004f, IM_COL32(255, 255, 255, 255));
    draw_list->PopClipRect();
}

// Логотип и splash привязаны к экрану отдельно от фоновой панорамы.
void DrawMinecraftLogo(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    if (!EnsureMenuLogoTextureLoaded())
    {
        return;
    }

    const float scale = std::min(viewport_size.x / 1280.0f, viewport_size.y / 720.0f) * kMainMenuBaseScale;
    const float max_logo_width = std::clamp(viewport_size.x * 0.78f * kMainMenuBaseScale, 420.0f * scale, 760.0f * scale) * kLogoScaleAdjust;
    const float logo_aspect = static_cast<float>(g_MenuLogoTexture.Width) / static_cast<float>(g_MenuLogoTexture.Height);
    const ImVec2 logo_size = ImVec2(max_logo_width, max_logo_width / logo_aspect);
    // Позиция логотипа вручную подгоняется под референс меню Legacy Console.
    const ImVec2 logo_pos = ImVec2(viewport_pos.x + viewport_size.x * 0.5f - logo_size.x * 0.5f, viewport_pos.y + 85.8f * scale);

    draw_list->AddImage(g_MenuLogoTexture.Texture->GetTexRef(), logo_pos, ImVec2(logo_pos.x + logo_size.x, logo_pos.y + logo_size.y));

    // Splash жёстко привязан к логотипу, чтобы при смене масштаба не "уплывать" отдельно.
    const float splash_size = 42.0f * scale * kLogoScaleAdjust;
    const ImVec2 splash_pos = ImVec2(logo_pos.x + logo_size.x * 0.74f, logo_pos.y + 12.0f * scale * kLogoScaleAdjust);
    DrawSlantedText(draw_list, g_FontSplash, splash_size, splash_pos, IM_COL32(255, 242, 76, 255), IM_COL32(82, 70, 0, 255), 1.6f * scale, -0.18f, g_SplashText);
}
}
