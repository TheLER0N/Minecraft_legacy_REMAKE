#include "menu_fonts.h"

#include "imgui.h"
#include "menu_state.h"

#include <array>

namespace MenuInternal
{
namespace
{
constexpr const char* kPrimaryMenuFontFile = "assets\\fonts\\Mojangles.ttf";
constexpr const char* kRussianMenuFallbackFontFile = "assets\\fonts\\RU\\SpaceMace.ttf";

// Для пиксельных шрифтов уменьшаем oversampling и включаем pixel snap.
ImFontConfig BuildFontConfig(bool pixel_snap)
{
    ImFontConfig config = {};
    config.OversampleH = pixel_snap ? 1 : 3;
    config.OversampleV = pixel_snap ? 1 : 3;
    config.PixelSnapH = pixel_snap;
    config.RasterizerMultiply = 1.12f;
    return config;
}

// Кириллицу подмешиваем во второй проход, чтобы сохранить основной стиль Mojangles.
void MergeCyrillicFallback(float size_pixels, bool pixel_snap)
{
    ImGuiIO& io = ImGui::GetIO();
    if (io.Fonts->Fonts.empty())
    {
        return;
    }

    ImFontConfig merge_config = BuildFontConfig(pixel_snap);
    merge_config.MergeMode = true;
    merge_config.DstFont = io.Fonts->Fonts.back();

    const std::string path = GetProjectAssetPath(kRussianMenuFallbackFontFile);
    io.Fonts->AddFontFromFileTTF(path.c_str(), size_pixels, &merge_config, io.Fonts->GetGlyphRangesCyrillic());
}
}

// Сначала пробуем основной шрифт, затем резервный. Это позволяет не падать на отсутствующем asset-файле.
ImFont* LoadFontWithFallback(const char* primary_path, const char* fallback_path, float size_pixels, bool pixel_snap)
{
    ImFontConfig config = BuildFontConfig(pixel_snap);

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

void LoadMenuFonts(float main_scale)
{
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // Загружаем все размеры меню единым пакетом, чтобы они использовали одинаковый стиль и fallback'и.
    g_FontTitle = LoadFontWithFallback(kPrimaryMenuFontFile, "imgui\\misc\\fonts\\DroidSans.ttf", 122.0f * main_scale, true);
    g_FontSubtitle = LoadFontWithFallback(kPrimaryMenuFontFile, "imgui\\misc\\fonts\\DroidSans.ttf", 26.0f * main_scale, true);
    if (g_FontSubtitle != nullptr)
    {
        MergeCyrillicFallback(26.0f * main_scale, true);
    }

    g_FontMenu = LoadFontWithFallback(kPrimaryMenuFontFile, "imgui\\misc\\fonts\\DroidSans.ttf", 30.0f * main_scale, true);
    if (g_FontMenu != nullptr)
    {
        MergeCyrillicFallback(30.0f * main_scale, true);
    }

    g_FontSplash = LoadFontWithFallback(kPrimaryMenuFontFile, "imgui\\misc\\fonts\\DroidSans.ttf", 42.0f * main_scale, true);

    if (g_FontTitle == nullptr || g_FontSubtitle == nullptr || g_FontMenu == nullptr || g_FontSplash == nullptr)
    {
        io.Fonts->Clear();
        g_FontMenu = io.Fonts->AddFontDefault();
        g_FontTitle = g_FontMenu;
        g_FontSubtitle = g_FontMenu;
        g_FontSplash = g_FontMenu;
    }
}

ImFont* ResolveFont(ImFont* font)
{
    return font != nullptr ? font : ImGui::GetFont();
}

ImVec2 MeasureText(ImFont* font, const char* text, float size)
{
    font = ResolveFont(font);
    const float font_size = size > 0.0f ? size : font->LegacySize;
    return font->CalcTextSizeA(font_size, 4096.0f, 0.0f, text);
}

// Обводка рисуется отдельными смещёнными копиями текста, чтобы сохранить старый консольный стиль.
void DrawTextOutlined(ImDrawList* draw_list, ImFont* font, float size, const ImVec2& position, ImU32 color, ImU32 outline_color, float outline_thickness, const char* text)
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

// Splash рисуется посимвольно, чтобы можно было сдвигать каждую букву по диагонали.
void DrawSlantedText(ImDrawList* draw_list, ImFont* font, float size, ImVec2 position, ImU32 color, ImU32 outline_color, float outline_thickness, float slope, const char* text)
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
}
