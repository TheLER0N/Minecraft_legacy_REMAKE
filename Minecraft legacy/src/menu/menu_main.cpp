#include "menu_main.h"
#include "imgui.h"
#include "menu_state.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <string>

namespace MenuInternal
{
namespace
{
// Эти коэффициенты позволяют подгонять экран под референс, не меняя базовые размеры текстур.
constexpr float kMainMenuBaseScale = 0.8f;
constexpr float kButtonScaleAdjust = 0.95f;
constexpr ImU32 kButtonNormalTint = IM_COL32(255, 255, 255, 255);
constexpr ImU32 kButtonHoverTint = IM_COL32(156, 164, 214, 255);
constexpr ImU32 kButtonTextColor = IM_COL32(243, 243, 243, 255);
constexpr ImU32 kButtonHoverTextColor = IM_COL32(255, 255, 85, 255);

bool HasValidTexture(const TextureSlot& texture_slot)
{
    return texture_slot.Texture != nullptr && texture_slot.Width > 0 && texture_slot.Height > 0;
}

// Кнопка рисуется как готовая текстура без девятислайса, чтобы не ломать пиксельную сетку.
void DrawButtonTexture(ImDrawList* draw_list, const TextureSlot& texture_slot, const ImVec2& min, const ImVec2& max, ImU32 tint)
{
    if (texture_slot.Texture == nullptr || texture_slot.Width <= 0 || texture_slot.Height <= 0)
    {
        return;
    }

    draw_list->AddImage(texture_slot.Texture->GetTexRef(), min, max, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f), tint);
}
}

void DrawMenuButtons(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    const bool has_button_textures = EnsureButtonTexturesLoaded();

    // Масштабируем меню целыми шагами, чтобы pixel-art кнопки оставались читаемыми.
    const float layout_scale = std::min(viewport_size.x / 1280.0f, viewport_size.y / 720.0f) * kMainMenuBaseScale;
    const float source_button_width = has_button_textures ? static_cast<float>(g_ButtonTexture.Width) : 200.0f;
    const float source_button_height = has_button_textures ? static_cast<float>(g_ButtonTexture.Height) : 20.0f;
    const int width_scale = std::max(1, static_cast<int>((viewport_size.x * 0.52f * kMainMenuBaseScale) / source_button_width));
    const int height_scale = std::max(1, static_cast<int>((viewport_size.y * 0.48f * kMainMenuBaseScale) / (source_button_height * static_cast<float>(g_MenuItems.size()))));
    const int button_pixel_scale = std::clamp(std::min(width_scale, height_scale), 1, 4);
    const float button_visual_scale = static_cast<float>(button_pixel_scale) * kButtonScaleAdjust;
    const float menu_width = source_button_width * button_visual_scale;
    const float button_height = source_button_height * button_visual_scale;
    const float spacing = 16.0f * button_visual_scale / static_cast<float>(button_pixel_scale);
    const float total_height = button_height * static_cast<float>(g_MenuItems.size()) + spacing * static_cast<float>(g_MenuItems.size() - 1);
    const ImVec2 menu_pos = ImVec2(viewport_pos.x + viewport_size.x * 0.5f - menu_width * 0.5f, viewport_pos.y + viewport_size.y * 0.41f - total_height * 0.18f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
    ImGui::SetNextWindowPos(menu_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(menu_width, total_height), ImGuiCond_Always);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##start_menu", nullptr, flags);

    ImGui::PushFont(ResolveFont(g_FontMenu));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, spacing));

    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false))
        g_SelectedMenuItem = (g_SelectedMenuItem + 1) % static_cast<int>(g_MenuItems.size());
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false))
        g_SelectedMenuItem = (g_SelectedMenuItem + static_cast<int>(g_MenuItems.size()) - 1) % static_cast<int>(g_MenuItems.size());

    for (size_t i = 0; i < g_MenuItems.size(); ++i)
    {
        const bool is_selected = static_cast<int>(i) == g_SelectedMenuItem;

        // InvisibleButton даёт input-логику, а внешний вид мы полностью рисуем сами.
        ImGui::SetCursorPosX(0.0f);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##menu_button", ImVec2(menu_width, button_height)))
            ActivateStartMenuItem(static_cast<int>(i));
        const bool is_hovered = ImGui::IsItemHovered();
        if (is_hovered)
            g_SelectedMenuItem = static_cast<int>(i);

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        const ImVec2 item_min = ImGui::GetItemRectMin();
        const ImVec2 item_max = ImGui::GetItemRectMax();
        const bool is_emphasized = is_selected || is_hovered;

        if (has_button_textures)
        {
            // Если есть отдельная highlighted-текстура, используем именно её вместо простого tint.
            if (is_emphasized && HasValidTexture(g_ButtonHighlightedTexture))
            {
                DrawButtonTexture(draw_list, g_ButtonHighlightedTexture, item_min, item_max, kButtonNormalTint);
            }
            else
            {
                DrawButtonTexture(draw_list, g_ButtonTexture, item_min, item_max, is_emphasized ? kButtonHoverTint : kButtonNormalTint);
            }
        }
        else
        {
            draw_list->AddRectFilled(item_min, item_max, is_emphasized ? kButtonHoverTint : IM_COL32(176, 176, 176, 218));
            draw_list->AddRect(item_min, item_max, IM_COL32(28, 28, 28, 255), 0.0f, 0, 2.0f * layout_scale);
            draw_list->AddLine(item_min, ImVec2(item_max.x, item_min.y), IM_COL32(230, 230, 230, 86), 1.0f);
            draw_list->AddLine(ImVec2(item_min.x, item_max.y), item_max, IM_COL32(0, 0, 0, 120), 1.0f);
        }

        const float text_size = std::clamp(button_height * 0.52f, 20.0f * layout_scale, 34.0f * layout_scale);
        const ImVec2 text_size_px = MeasureText(g_FontMenu, g_MenuItems[i], text_size);
        const ImVec2 text_pos = ImVec2(
            item_min.x + (menu_width - text_size_px.x) * 0.5f,
            item_min.y + (button_height - text_size_px.y) * 0.5f - 1.0f * layout_scale
        );
        DrawTextOutlined(
            draw_list,
            g_FontMenu,
            text_size,
            text_pos,
            is_emphasized ? kButtonHoverTextColor : kButtonTextColor,
            IM_COL32(0, 0, 0, 255),
            1.0f * layout_scale,
            g_MenuItems[i]
        );

        ImGui::PopID();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false))
        ActivateStartMenuItem(g_SelectedMenuItem);

    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    // Короткое всплывающее подтверждение нужно для заглушек пунктов, у которых ещё нет отдельного экрана.
    if (g_LastActivatedMenuItem >= 0)
    {
        const uint64_t now = SDL_GetTicks();
        if (now - g_LastActivatedMenuTime < 2200)
        {
            ImDrawList* draw_list = ImGui::GetForegroundDrawList();
            const char* label = g_MenuItems[g_LastActivatedMenuItem];
            std::string message = std::string(label) + " selected";
            const ImVec2 message_size = MeasureText(g_FontSubtitle, message.c_str(), 28.0f * layout_scale);
            const ImVec2 message_pos = ImVec2(viewport_pos.x + viewport_size.x * 0.5f - message_size.x * 0.5f, viewport_pos.y + viewport_size.y * 0.88f);
            draw_list->AddRectFilled(ImVec2(message_pos.x - 22.0f * layout_scale, message_pos.y - 10.0f * layout_scale), ImVec2(message_pos.x + message_size.x + 22.0f * layout_scale, message_pos.y + message_size.y + 12.0f * layout_scale), IM_COL32(16, 20, 26, 196), 8.0f * layout_scale);
            DrawTextOutlined(draw_list, g_FontSubtitle, 28.0f * layout_scale, message_pos, IM_COL32(245, 245, 245, 255), IM_COL32(0, 0, 0, 255), 1.0f * layout_scale, message.c_str());
        }
    }

    // Нижняя подсказка имитирует консольный footer с кнопкой подтверждения.
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    const float footer_size = 24.0f * layout_scale;
    const ImVec2 footer_pos = ImVec2(viewport_pos.x + 32.0f * layout_scale, viewport_pos.y + viewport_size.y - 42.0f * layout_scale);
    draw_list->AddCircleFilled(ImVec2(footer_pos.x + 12.0f * layout_scale, footer_pos.y + 14.0f * layout_scale), 12.0f * layout_scale, IM_COL32(38, 55, 102, 245), 20);
    DrawTextOutlined(draw_list, g_FontSubtitle, footer_size, ImVec2(footer_pos.x + 5.0f * layout_scale, footer_pos.y + 2.0f * layout_scale), IM_COL32(216, 225, 255, 255), IM_COL32(0, 0, 0, 255), 1.0f * layout_scale, "X");
    DrawTextOutlined(draw_list, g_FontSubtitle, footer_size, ImVec2(footer_pos.x + 32.0f * layout_scale, footer_pos.y), IM_COL32(236, 236, 236, 255), IM_COL32(0, 0, 0, 255), 1.0f * layout_scale, "Select");
}
}
