#include "menu_play_game.h"

#include "imgui.h"
#include "menu_state.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace MenuInternal
{
// При входе в Play Game всегда начинаем с вкладки Load и верхнего элемента списка.
void OpenPlayGameMenu()
{
    g_CurrentMenuScreen = MenuScreen::PlayGame;
    g_SelectedPlayGameTab = static_cast<int>(PlayGameTab::Load);
    g_SelectedPlayGameWorld = 0;
    g_PlayGameScrollOffset = 0;
    g_LastActivatedMenuItem = -1;
    g_LastActivatedMenuTime = 0;
}

// Возврат в стартовое меню также восстанавливает стандартный выделенный пункт.
void ClosePlayGameMenu()
{
    g_CurrentMenuScreen = MenuScreen::Start;
    g_SelectedMenuItem = 0;
    g_LastActivatedMenuItem = -1;
    g_LastActivatedMenuTime = 0;
}

// Пока полноценный переход есть только у Play Game, остальные пункты дают временное подтверждение.
void ActivateStartMenuItem(int index)
{
    g_SelectedMenuItem = index;
    if (index == 0)
    {
        OpenPlayGameMenu();
        return;
    }

    g_LastActivatedMenuItem = index;
    g_LastActivatedMenuTime = SDL_GetTicks();
}

void DrawPlayGameMenu(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
    {
        ClosePlayGameMenu();
        return;
    }

    // Поддерживаем одинаковую навигацию стрелками и геймпадом.
    const int tab_count = static_cast<int>(g_PlayGameTabs.size());
    const int world_count = static_cast<int>(g_PlayGameWorlds.size());
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, false))
        g_SelectedPlayGameTab = (g_SelectedPlayGameTab + 1) % tab_count;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, false))
        g_SelectedPlayGameTab = (g_SelectedPlayGameTab + tab_count - 1) % tab_count;

    if (g_SelectedPlayGameTab == static_cast<int>(PlayGameTab::Load) && world_count > 0)
    {
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickDown, false))
            g_SelectedPlayGameWorld = (g_SelectedPlayGameWorld + 1) % world_count;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickUp, false))
            g_SelectedPlayGameWorld = (g_SelectedPlayGameWorld + world_count - 1) % world_count;

        // Прокрутка всегда подгоняется так, чтобы выделенный мир не уходил за пределы окна.
        const int visible_world_count = 5;
        const int max_scroll_offset = std::max(0, world_count - visible_world_count);
        if (g_SelectedPlayGameWorld < g_PlayGameScrollOffset)
            g_PlayGameScrollOffset = g_SelectedPlayGameWorld;
        else if (g_SelectedPlayGameWorld >= g_PlayGameScrollOffset + visible_world_count)
            g_PlayGameScrollOffset = g_SelectedPlayGameWorld - visible_world_count + 1;
        g_PlayGameScrollOffset = std::clamp(g_PlayGameScrollOffset, 0, max_scroll_offset);
    }

    const float scale = std::min(viewport_size.x / 1280.0f, viewport_size.y / 720.0f);
    const ImVec2 panel_size = ImVec2(std::clamp(viewport_size.x * 0.39f, 430.0f * scale, 520.0f * scale), std::clamp(viewport_size.y * 0.48f, 340.0f * scale, 420.0f * scale));
    const ImVec2 panel_pos = ImVec2(viewport_pos.x + viewport_size.x * 0.5f - panel_size.x * 0.5f, viewport_pos.y + viewport_size.y * 0.52f - panel_size.y * 0.5f);
    const ImVec2 viewport_max = ImVec2(viewport_pos.x + viewport_size.x, viewport_pos.y + viewport_size.y);

    ImGui::GetForegroundDrawList()->AddRectFilled(viewport_pos, viewport_max, IM_COL32(0, 0, 0, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowPos(panel_pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(panel_size, ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    ImGui::Begin("##play_game_menu", nullptr, flags);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 window_pos = ImGui::GetWindowPos();
    const ImVec2 window_max = ImVec2(window_pos.x + panel_size.x, window_pos.y + panel_size.y);
    const float tab_height = 30.0f * scale;
    const float tab_gap = 4.0f * scale;
    const float frame_padding = 9.0f * scale;
    const ImVec2 body_min = ImVec2(window_pos.x, window_pos.y + tab_height - 2.0f * scale);
    const ImVec2 body_max = window_max;
    const ImVec2 inner_min = ImVec2(body_min.x + frame_padding, body_min.y + 10.0f * scale);
    const ImVec2 inner_max = ImVec2(body_max.x - frame_padding, body_max.y - 10.0f * scale);
    const ImVec2 list_min = ImVec2(inner_min.x + 8.0f * scale, inner_min.y + 8.0f * scale);
    const ImVec2 list_max = ImVec2(inner_max.x - 8.0f * scale, inner_max.y - 8.0f * scale);

    // Простейший helper для рамок в стиле Legacy Console: светлая кромка сверху и тёмная снизу.
    auto draw_bevel_box = [&](const ImVec2& min, const ImVec2& max, ImU32 top_fill, ImU32 bottom_fill, ImU32 light_edge, ImU32 dark_edge, ImU32 outline)
    {
        draw_list->AddRectFilledMultiColor(min, max, top_fill, top_fill, bottom_fill, bottom_fill);
        draw_list->AddRect(min, max, outline);
        draw_list->AddLine(min, ImVec2(max.x - 1.0f, min.y), light_edge, 1.0f);
        draw_list->AddLine(min, ImVec2(min.x, max.y - 1.0f), light_edge, 1.0f);
        draw_list->AddLine(ImVec2(min.x, max.y - 1.0f), ImVec2(max.x - 1.0f, max.y - 1.0f), dark_edge, 1.0f);
        draw_list->AddLine(ImVec2(max.x - 1.0f, min.y), ImVec2(max.x - 1.0f, max.y - 1.0f), dark_edge, 1.0f);
    };

    // Временные превью миров рисуются процедурно, пока нет отдельных миниатюр из сохранений.
    auto draw_thumbnail = [&](const ImVec2& min, const ImVec2& max, int variant)
    {
        draw_bevel_box(min, max, IM_COL32(188, 190, 194, 255), IM_COL32(122, 124, 128, 255), IM_COL32(248, 248, 248, 255), IM_COL32(82, 84, 88, 255), IM_COL32(40, 42, 46, 255));
        const ImVec2 inner_thumb_min = ImVec2(min.x + 2.0f * scale, min.y + 2.0f * scale);
        const ImVec2 inner_thumb_max = ImVec2(max.x - 2.0f * scale, max.y - 2.0f * scale);
        draw_list->AddRectFilledMultiColor(inner_thumb_min, inner_thumb_max, IM_COL32(154, 212, 246, 255), IM_COL32(154, 212, 246, 255), IM_COL32(106, 182, 112, 255), IM_COL32(106, 182, 112, 255));

        if (variant == 0)
        {
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x, inner_thumb_min.y + 10.0f * scale), ImVec2(inner_thumb_min.x + 10.0f * scale, inner_thumb_max.y), IM_COL32(78, 204, 194, 255));
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x + 11.0f * scale, inner_thumb_min.y + 8.0f * scale), ImVec2(inner_thumb_max.x, inner_thumb_max.y - 2.0f * scale), IM_COL32(114, 178, 82, 255));
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x + 17.0f * scale, inner_thumb_min.y + 5.0f * scale), ImVec2(inner_thumb_min.x + 22.0f * scale, inner_thumb_min.y + 12.0f * scale), IM_COL32(94, 64, 42, 255));
        }
        else if (variant == 1)
        {
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x, inner_thumb_min.y + 12.0f * scale), inner_thumb_max, IM_COL32(134, 178, 96, 255));
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x + 4.0f * scale, inner_thumb_min.y + 6.0f * scale), ImVec2(inner_thumb_min.x + 9.0f * scale, inner_thumb_max.y), IM_COL32(88, 64, 42, 255));
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x + 2.0f * scale, inner_thumb_min.y + 2.0f * scale), ImVec2(inner_thumb_min.x + 16.0f * scale, inner_thumb_min.y + 12.0f * scale), IM_COL32(76, 150, 82, 255));
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x + 17.0f * scale, inner_thumb_min.y + 11.0f * scale), inner_thumb_max, IM_COL32(146, 154, 178, 255));
        }
        else
        {
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x, inner_thumb_min.y + 13.0f * scale), inner_thumb_max, IM_COL32(120, 208, 120, 255));
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x + 13.0f * scale, inner_thumb_min.y + 8.0f * scale), ImVec2(inner_thumb_max.x, inner_thumb_max.y), IM_COL32(82, 150, 212, 255));
            draw_list->AddRectFilled(ImVec2(inner_thumb_min.x + 5.0f * scale, inner_thumb_min.y + 7.0f * scale), ImVec2(inner_thumb_min.x + 12.0f * scale, inner_thumb_min.y + 13.0f * scale), IM_COL32(90, 90, 98, 255));
        }
    };

    draw_bevel_box(body_min, body_max, IM_COL32(208, 212, 220, 255), IM_COL32(176, 180, 188, 255), IM_COL32(255, 255, 255, 255), IM_COL32(82, 86, 92, 255), IM_COL32(36, 38, 42, 255));
    draw_bevel_box(inner_min, inner_max, IM_COL32(182, 185, 192, 255), IM_COL32(152, 156, 162, 255), IM_COL32(236, 238, 242, 255), IM_COL32(88, 92, 98, 255), IM_COL32(54, 56, 62, 255));
    draw_bevel_box(list_min, list_max, IM_COL32(126, 129, 134, 255), IM_COL32(98, 101, 106, 255), IM_COL32(208, 210, 214, 255), IM_COL32(72, 74, 78, 255), IM_COL32(42, 44, 48, 255));

    const float tab_width = (panel_size.x - 12.0f * scale - tab_gap * static_cast<float>(g_PlayGameTabs.size() - 1)) / static_cast<float>(g_PlayGameTabs.size());
    for (size_t i = 0; i < g_PlayGameTabs.size(); ++i)
    {
        const ImVec2 tab_min = ImVec2(window_pos.x + 4.0f * scale + static_cast<float>(i) * (tab_width + tab_gap), window_pos.y);
        const ImVec2 tab_max = ImVec2(tab_min.x + tab_width, tab_min.y + tab_height);
        const bool is_selected = static_cast<int>(i) == g_SelectedPlayGameTab;

        ImGui::SetCursorScreenPos(tab_min);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##play_game_tab", ImVec2(tab_width, tab_height)))
            g_SelectedPlayGameTab = static_cast<int>(i);
        ImGui::PopID();

        draw_bevel_box(tab_min, tab_max, is_selected ? IM_COL32(226, 228, 234, 255) : IM_COL32(190, 193, 198, 255), is_selected ? IM_COL32(182, 186, 194, 255) : IM_COL32(146, 150, 156, 255), IM_COL32(255, 255, 255, 255), IM_COL32(86, 88, 94, 255), IM_COL32(40, 42, 46, 255));

        const float tab_text_size = 18.0f * scale;
        const ImVec2 tab_label_size = MeasureText(g_FontSubtitle, g_PlayGameTabs[i], tab_text_size);
        DrawTextOutlined(draw_list, g_FontSubtitle, tab_text_size, ImVec2(tab_min.x + (tab_width - tab_label_size.x) * 0.5f, tab_min.y + (tab_height - tab_label_size.y) * 0.5f - 1.0f * scale), IM_COL32(32, 34, 38, 255), IM_COL32(248, 248, 248, 80), 0.7f * scale, g_PlayGameTabs[i]);
    }

    const float scroll_track_width = 12.0f * scale;
    const ImVec2 scroll_track_min = ImVec2(list_max.x - scroll_track_width - 5.0f * scale, list_min.y + 6.0f * scale);
    const ImVec2 scroll_track_max = ImVec2(list_max.x - 5.0f * scale, list_max.y - 6.0f * scale);
    const float rows_left = list_min.x + 8.0f * scale;
    const float rows_right = scroll_track_min.x - 6.0f * scale;
    const float rows_top = list_min.y + 8.0f * scale;
    const float rows_bottom = list_max.y - 8.0f * scale;

    if (g_SelectedPlayGameTab == static_cast<int>(PlayGameTab::Load))
    {
        const float row_spacing = 4.0f * scale;
        const int visible_world_count = std::min(world_count, 5);
        const int max_scroll_offset = std::max(0, world_count - visible_world_count);
        g_PlayGameScrollOffset = std::clamp(g_PlayGameScrollOffset, 0, max_scroll_offset);
        const float row_height = std::clamp((rows_bottom - rows_top - row_spacing * static_cast<float>(visible_world_count - 1)) / static_cast<float>(visible_world_count), 28.0f * scale, 36.0f * scale);
        const int first_visible_world = g_PlayGameScrollOffset;
        const int last_visible_world = std::min(world_count, first_visible_world + visible_world_count);
        for (int world_index = first_visible_world; world_index < last_visible_world; ++world_index)
        {
            const int visible_index = world_index - first_visible_world;
            const float row_y = rows_top + static_cast<float>(visible_index) * (row_height + row_spacing);
            const ImVec2 row_min = ImVec2(rows_left, row_y);
            const ImVec2 row_max = ImVec2(rows_right, row_y + row_height);
            const bool is_selected = world_index == g_SelectedPlayGameWorld;
            const PlayGameWorld& world = g_PlayGameWorlds[world_index];

            ImGui::SetCursorScreenPos(row_min);
            ImGui::PushID(world_index);
            if (ImGui::InvisibleButton("##world_row", ImVec2(row_max.x - row_min.x, row_height)))
                g_SelectedPlayGameWorld = world_index;
            if (ImGui::IsItemHovered())
                g_SelectedPlayGameWorld = world_index;
            ImGui::PopID();

            draw_bevel_box(row_min, row_max, is_selected ? IM_COL32(198, 202, 242, 255) : IM_COL32(182, 184, 188, 255), is_selected ? IM_COL32(156, 162, 214, 255) : IM_COL32(148, 150, 154, 255), IM_COL32(242, 242, 246, 255), IM_COL32(92, 96, 104, 255), IM_COL32(54, 56, 62, 255));

            const ImVec2 preview_min = ImVec2(row_min.x + 6.0f * scale, row_min.y + 4.0f * scale);
            const ImVec2 preview_max = ImVec2(preview_min.x + 34.0f * scale, row_max.y - 4.0f * scale);
            draw_thumbnail(preview_min, preview_max, world_index % 3);

            const float text_size = 19.0f * scale;
            const ImVec2 text_metrics = MeasureText(g_FontMenu, world.Name, text_size);
            DrawTextOutlined(draw_list, g_FontMenu, text_size, ImVec2(preview_max.x + 9.0f * scale, row_min.y + (row_height - text_metrics.y) * 0.5f - 1.0f * scale), is_selected ? IM_COL32(255, 240, 94, 255) : IM_COL32(244, 244, 244, 255), IM_COL32(46, 48, 54, 255), 1.0f * scale, world.Name);
        }

        draw_bevel_box(scroll_track_min, scroll_track_max, IM_COL32(212, 214, 218, 255), IM_COL32(184, 186, 190, 255), IM_COL32(255, 255, 255, 255), IM_COL32(98, 102, 108, 255), IM_COL32(54, 56, 62, 255));
        const float up_arrow_center_x = (scroll_track_min.x + scroll_track_max.x) * 0.5f;
        draw_list->AddTriangleFilled(ImVec2(up_arrow_center_x, scroll_track_min.y + 4.0f * scale), ImVec2(scroll_track_min.x + 3.0f * scale, scroll_track_min.y + 10.0f * scale), ImVec2(scroll_track_max.x - 3.0f * scale, scroll_track_min.y + 10.0f * scale), IM_COL32(255, 234, 54, 255));
        draw_list->AddTriangleFilled(ImVec2(scroll_track_min.x + 3.0f * scale, scroll_track_max.y - 10.0f * scale), ImVec2(scroll_track_max.x - 3.0f * scale, scroll_track_max.y - 10.0f * scale), ImVec2(up_arrow_center_x, scroll_track_max.y - 4.0f * scale), IM_COL32(255, 234, 54, 255));

        const float thumb_track_top = scroll_track_min.y + 14.0f * scale;
        const float thumb_track_bottom = scroll_track_max.y - 14.0f * scale;
        const float thumb_track_height = thumb_track_bottom - thumb_track_top;
        // Размер ползунка зависит от того, сколько миров видно сейчас по отношению ко всему списку.
        const float thumb_height = max_scroll_offset > 0 ? std::max(34.0f * scale, thumb_track_height * (static_cast<float>(visible_world_count) / static_cast<float>(world_count))) : thumb_track_height;
        const float thumb_offset = max_scroll_offset > 0 ? (thumb_track_height - thumb_height) * (static_cast<float>(g_PlayGameScrollOffset) / static_cast<float>(max_scroll_offset)) : 0.0f;
        const ImVec2 thumb_min = ImVec2(scroll_track_min.x + 2.0f * scale, thumb_track_top + thumb_offset);
        const ImVec2 thumb_max = ImVec2(scroll_track_max.x - 2.0f * scale, thumb_min.y + thumb_height);
        draw_bevel_box(thumb_min, thumb_max, IM_COL32(186, 190, 236, 255), IM_COL32(148, 154, 212, 255), IM_COL32(242, 244, 255, 255), IM_COL32(92, 98, 142, 255), IM_COL32(68, 72, 104, 255));
    }
    else
    {
        const char* const* items = nullptr;
        int item_count = 0;
        static constexpr const char* create_items[] = { "Create New World", "Play Tutorial", "More Options" };
        static constexpr const char* join_items[] = { "Join Game", "Friends", "Local Network" };

        if (g_SelectedPlayGameTab == static_cast<int>(PlayGameTab::Create))
        {
            items = create_items;
            item_count = static_cast<int>(IM_ARRAYSIZE(create_items));
        }
        else
        {
            items = join_items;
            item_count = static_cast<int>(IM_ARRAYSIZE(join_items));
        }

        const float row_spacing = 6.0f * scale;
        const float row_height = 38.0f * scale;
        for (int i = 0; i < item_count; ++i)
        {
            const float row_y = rows_top + static_cast<float>(i) * (row_height + row_spacing) + 18.0f * scale;
            const ImVec2 row_min = ImVec2(rows_left + 10.0f * scale, row_y);
            const ImVec2 row_max = ImVec2(rows_right - 10.0f * scale, row_y + row_height);
            const bool is_primary = i == 0;

            draw_bevel_box(row_min, row_max, is_primary ? IM_COL32(198, 202, 242, 255) : IM_COL32(182, 184, 188, 255), is_primary ? IM_COL32(156, 162, 214, 255) : IM_COL32(148, 150, 154, 255), IM_COL32(242, 242, 246, 255), IM_COL32(92, 96, 104, 255), IM_COL32(54, 56, 62, 255));

            const float text_size = 20.0f * scale;
            const ImVec2 text_metrics = MeasureText(g_FontMenu, items[i], text_size);
            DrawTextOutlined(draw_list, g_FontMenu, text_size, ImVec2(row_min.x + 14.0f * scale, row_min.y + (row_height - text_metrics.y) * 0.5f - 1.0f * scale), is_primary ? IM_COL32(255, 240, 94, 255) : IM_COL32(244, 244, 244, 255), IM_COL32(46, 48, 54, 255), 1.0f * scale, items[i]);
        }
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
}
}
