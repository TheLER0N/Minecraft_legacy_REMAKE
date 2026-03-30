#include "game.h"

#include "../menu/menu.h"

namespace
{
// Сейчас "игра" по сути означает активную оболочку меню, поднятую поверх готового runtime.
bool g_GameStarted = false;
}

// Инициализирует верхний уровень приложения после того, как SDL/Vulkan/ImGui уже готовы.
bool StartGame(float main_scale)
{
    if (g_GameStarted)
    {
        ShutdownGame();
    }

    InitializeMenu(main_scale);
    g_GameStarted = true;
    return true;
}

// Пока игровой рендер сводится к отрисовке меню и его экранов.
void RenderGame()
{
    if (!g_GameStarted)
    {
        return;
    }

    RenderMenu();
}

// Освобождает состояние UI и возвращает игру в "не запущено".
void ShutdownGame()
{
    if (!g_GameStarted)
    {
        return;
    }

    ShutdownMenu();
    g_GameStarted = false;
}
