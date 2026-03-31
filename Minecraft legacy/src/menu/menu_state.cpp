#include "menu_state.h"

namespace MenuInternal
{
// Указатели на уже загруженные шрифты разных размеров.
ImFont* g_FontTitle = nullptr;
ImFont* g_FontMenu = nullptr;
ImFont* g_FontSubtitle = nullptr;
ImFont* g_FontSplash = nullptr;

// Текущее положение выделения и отметка о последнем активированном пункте.
int g_SelectedMenuItem = 0;
int g_LastActivatedMenuItem = -1;
uint64_t g_LastActivatedMenuTime = 0;

// Все texture slot'ы, которыми пользуется меню во время работы.
TextureSlot g_PanoramaTexture = {};
TextureSlot g_ButtonTexture = {};
TextureSlot g_ButtonHighlightedTexture = {};
TextureSlot g_ButtonDisabledTexture = {};
TextureSlot g_MenuLogoTexture = {};
std::array<TextureSlot, g_IntroPhotoCount> g_IntroPhotoTextures = {};

// Флаги показывают, что уже было загружено или хотя бы предпринята попытка загрузки.
bool g_PanoramaTexturesLoaded = false;
bool g_ButtonTexturesLoaded = false;
bool g_PanoramaLoadAttempted = false;
bool g_ButtonLoadAttempted = false;
bool g_MenuLogoLoadAttempted = false;
std::array<bool, g_IntroPhotoCount> g_IntroPhotoLoadAttempted = {};

// Текущее состояние экранов меню и внутренних списков.
PanoramaVariant g_SelectedPanoramaVariant = PanoramaVariant::Day;
MenuScreen g_CurrentMenuScreen = MenuScreen::Start;
std::vector<PlayGameWorld> g_PlayGameWorlds = {};
int g_SelectedPlayGameTab = static_cast<int>(PlayGameTab::Load);
int g_SelectedPlayGameWorld = 0;
int g_PlayGameScrollOffset = 0;
PendingWorldAction g_PendingWorldAction = PendingWorldAction::None;
std::string g_PendingWorldDirectory = {};
std::array<char, 64> g_CreateWorldNameBuffer = {};
std::array<char, 32> g_CreateWorldSeedBuffer = {};
std::string g_MenuStatusMessage = {};
uint64_t g_MenuStatusTime = 0;

// Таймеры интро и анимации панорамы хранятся отдельно, чтобы они не запускались одновременно.
uint64_t g_IntroStartTime = 0;
uint64_t g_PanoramaAnimationStartTime = 0;
bool g_IntroFinished = false;
}
