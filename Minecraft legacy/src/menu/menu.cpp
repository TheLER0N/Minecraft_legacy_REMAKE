#include "menu.h"

#include "imgui.h"
#include "menu_state.h"

#include "menu_assets.h"
#include "menu_background.h"
#include "menu_fonts.h"
#include "menu_main.h"
#include "menu_play_game.h"

#include <SDL3/SDL.h>

namespace MenuInternal
{
// Главный экран рисуется слоями: сначала интро, затем фон, затем активный экран меню.
void DrawStartMenu()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 viewport_pos = viewport->Pos;
    const ImVec2 viewport_size = viewport->Size;
    if (viewport_size.x <= 0.0f || viewport_size.y <= 0.0f)
        return;

    // Пока идёт интро, меню и панорама полностью скрыты.
    if (DrawStartupIntro(viewport_pos, viewport_size))
        return;

    // После интро включаем живой фон и нужный поверх него экран меню.
    DrawPanoramaBackground(viewport_pos, viewport_size);
    if (g_CurrentMenuScreen == MenuScreen::PlayGame)
        DrawPlayGameMenu(viewport_pos, viewport_size);

    if (g_CurrentMenuScreen == MenuScreen::Start)
    {
        DrawMinecraftLogo(viewport_pos, viewport_size);
        DrawMenuButtons(viewport_pos, viewport_size);
    }
}
}

void InitializeMenu(float main_scale)
{
    using namespace MenuInternal;

    // Полный сброс состояния меню нужен, чтобы повторный запуск начинался с чистого экрана.
    ResetPanoramaTextures();
    g_PanoramaLoadAttempted = false;
    ResetButtonTextures();
    ResetMenuLogoTexture();
    ResetIntroPhotoTextures();

    LoadMenuFonts(main_scale);

    g_SelectedMenuItem = 0;
    g_LastActivatedMenuItem = -1;
    g_LastActivatedMenuTime = 0;
    g_SelectedPanoramaVariant = PanoramaVariant::Day;
    g_CurrentMenuScreen = MenuScreen::Start;
    g_SelectedPlayGameTab = static_cast<int>(PlayGameTab::Load);
    g_SelectedPlayGameWorld = 0;
    g_PlayGameScrollOffset = 0;
    // Панорама должна стартовать только после завершения всей интро-последовательности.
    g_IntroStartTime = SDL_GetTicks();
    g_PanoramaAnimationStartTime = 0;
    g_IntroFinished = false;
}

void RenderMenu()
{
    MenuInternal::DrawStartMenu();
}

void ShutdownMenu()
{
    using namespace MenuInternal;

    g_PanoramaLoadAttempted = false;
    ResetPanoramaTextures();
    ResetButtonTextures();
    ResetMenuLogoTexture();
    ResetIntroPhotoTextures();

    g_FontTitle = nullptr;
    g_FontMenu = nullptr;
    g_FontSubtitle = nullptr;
    g_FontSplash = nullptr;
}
