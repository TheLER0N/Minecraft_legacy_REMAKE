#include "menu_state.h"

namespace MenuInternal
{
ImFont* g_FontTitle = nullptr;
ImFont* g_FontMenu = nullptr;
ImFont* g_FontSubtitle = nullptr;
ImFont* g_FontSplash = nullptr;
int g_SelectedMenuItem = 0;
int g_LastActivatedMenuItem = -1;
uint64_t g_LastActivatedMenuTime = 0;

TextureSlot g_PanoramaTexture = {};
TextureSlot g_ButtonTexture = {};
TextureSlot g_ButtonHighlightedTexture = {};
TextureSlot g_ButtonDisabledTexture = {};
TextureSlot g_MenuLogoTexture = {};
std::array<TextureSlot, g_IntroPhotoCount> g_IntroPhotoTextures = {};
bool g_PanoramaTexturesLoaded = false;
bool g_ButtonTexturesLoaded = false;
bool g_PanoramaLoadAttempted = false;
bool g_ButtonLoadAttempted = false;
bool g_MenuLogoLoadAttempted = false;
std::array<bool, g_IntroPhotoCount> g_IntroPhotoLoadAttempted = {};
PanoramaVariant g_SelectedPanoramaVariant = PanoramaVariant::Day;
MenuScreen g_CurrentMenuScreen = MenuScreen::Start;
int g_SelectedPlayGameTab = static_cast<int>(PlayGameTab::Load);
int g_SelectedPlayGameWorld = 0;
int g_PlayGameScrollOffset = 0;
uint64_t g_IntroStartTime = 0;
uint64_t g_PanoramaAnimationStartTime = 0;
bool g_IntroFinished = false;
}
