#include "game.h"

#include "../menu/menu.h"
#include "../menu/menu_state.h"
#include "../world/world.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_mouse.h>

namespace
{
// Сейчас "игра" по сути означает активную оболочку меню, поднятую поверх готового runtime.
bool g_GameStarted = false;
bool g_InWorld = false;
float g_MainScale = 1.0f;

void ReturnToMenu(SDL_Window* window)
{
    SaveCurrentWorld();
    LeaveWorld();
    if (window != nullptr)
    {
        SDL_SetWindowRelativeMouseMode(window, false);
        SDL_SetWindowMouseGrab(window, false);
        SDL_ShowCursor();
    }
    g_InWorld = false;
    MenuInternal::g_CurrentMenuScreen = MenuInternal::MenuScreen::Start;
    MenuInternal::RefreshPlayGameWorldEntries();
}
}

// Инициализирует верхний уровень приложения после того, как SDL/Vulkan/ImGui уже готовы.
bool StartGame(float main_scale)
{
    if (g_GameStarted)
    {
        ShutdownGame();
    }

    g_MainScale = main_scale;
    InitializeMenu(main_scale);
    if (!InitializeWorldSystem(main_scale))
    {
        ShutdownMenu();
        return false;
    }

    MenuInternal::RefreshPlayGameWorldEntries();
    g_InWorld = false;
    g_GameStarted = true;
    return true;
}

void HandleGameEvent(const SDL_Event& event, SDL_Window* window, bool& request_quit)
{
    if (!g_GameStarted)
    {
        return;
    }

    if (g_InWorld)
    {
        HandleWorldEvent(event, window);
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_ESCAPE)
        {
            ReturnToMenu(window);
        }
        return;
    }

    (void)request_quit;
}

void UpdateGame(float delta_seconds, SDL_Window* window)
{
    if (!g_GameStarted)
    {
        return;
    }

    if (!g_InWorld)
    {
        if (MenuInternal::g_PendingWorldAction == MenuInternal::PendingWorldAction::Load)
        {
            if (LoadWorldFromMenu(MenuInternal::g_PendingWorldDirectory.c_str()))
            {
                g_InWorld = true;
            }
            MenuInternal::g_PendingWorldAction = MenuInternal::PendingWorldAction::None;
            MenuInternal::g_PendingWorldDirectory.clear();
        }
        else if (MenuInternal::g_PendingWorldAction == MenuInternal::PendingWorldAction::Create)
        {
            if (CreateWorldFromMenu(MenuInternal::g_CreateWorldNameBuffer.data(), MenuInternal::g_CreateWorldSeedBuffer.data()))
            {
                g_InWorld = true;
            }
            MenuInternal::g_PendingWorldAction = MenuInternal::PendingWorldAction::None;
        }
    }

    if (g_InWorld)
    {
        UpdateWorld(delta_seconds, window);
    }
}

// Пока игровой рендер сводится к отрисовке меню и его экранов.
void RenderGame()
{
    if (!g_GameStarted)
    {
        return;
    }

    if (g_InWorld && IsWorldLoaded())
    {
        RenderWorld();
        return;
    }

    RenderMenu();
}

bool IsGameInWorld()
{
    return g_GameStarted && g_InWorld && IsWorldLoaded();
}

// Освобождает состояние UI и возвращает игру в "не запущено".
void ShutdownGame()
{
    if (!g_GameStarted)
    {
        return;
    }

    SaveCurrentWorld();
    LeaveWorld();
    ShutdownWorldSystem();
    ShutdownMenu();
    g_InWorld = false;
    g_GameStarted = false;
}
