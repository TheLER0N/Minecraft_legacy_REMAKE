#pragma once

#include "imgui.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace MenuInternal
{
// Этот header играет роль общей точки синхронизации всех menu-модулей.
// Здесь лежат:
// - общий runtime-state меню,
// - пути к ресурсам,
// - декларации helper-функций,
// - и минимальный набор данных, которыми модули обмениваются друг с другом.

// Текстура уже загружена в ImGui/Vulkan и готова к многократной отрисовке в меню.
struct TextureSlot
{
    ImTextureData* Texture = nullptr;
    int Width = 0;
    int Height = 0;
};

// Текущий вариант фоновой панорамы. Выбирается один раз при инициализации меню.
enum class PanoramaVariant
{
    Day,
    Night,
};

// Какой экран меню сейчас должен рисоваться поверх фона.
enum class MenuScreen
{
    Start,
    PlayGame,
};

// Внутренние вкладки подменю Play Game.
enum class PlayGameTab
{
    Load,
    Create,
    Join,
};

// Описание сохранённого мира для списка в Play Game.
struct PlayGameWorld
{
    std::string DirectoryName;
    std::string Name;
    std::string GameMode;
    std::string LastPlayed;
    std::string Description;
};

enum class PendingWorldAction
{
    None,
    Create,
    Load,
};

inline constexpr const char* g_PanoramaDayFile = "assets\\panorama\\panorama_tu69_day.png";
inline constexpr const char* g_PanoramaNightFile = "assets\\panorama\\panorama_tu69_night.png";
inline constexpr const char* g_ButtonNormalFile = "assets\\button\\button.png";
inline constexpr const char* g_ButtonHighlightedFile = "assets\\button\\button_highlighted.png";
inline constexpr const char* g_ButtonDisabledFile = "assets\\button\\button_disabled.png";
inline constexpr const char* g_MenuLogoFile = "assets\\sound\\ui\\logo\\legacy_console_edition_logo.png.png";
inline constexpr const char* g_MenuLogoHolidayFile = "assets\\sound\\ui\\logo\\legacy_console_edition_logo_holiday.png.png";
// Эти экраны последовательно показываются до появления живого меню и старта панорамы.
inline constexpr std::size_t g_IntroPhotoCount = 3;
inline constexpr std::array<const char*, g_IntroPhotoCount> g_IntroPhotoFiles =
{
    "assets\\photo\\pic.png",
    "assets\\photo\\mojang.png",
    "assets\\photo\\KING.png",
};
inline constexpr uint64_t g_IntroFadeInDurationMs = 1000;
inline constexpr uint64_t g_IntroHoldDurationMs = 2000;
inline constexpr uint64_t g_IntroFadeOutDurationMs = 1000;
inline constexpr uint64_t g_IntroSceneDurationMs = g_IntroFadeInDurationMs + g_IntroHoldDurationMs + g_IntroFadeOutDurationMs;
inline constexpr uint64_t g_IntroTotalDurationMs = g_IntroSceneDurationMs * g_IntroPhotoCount;

// Основные пункты стартового экрана и заготовки данных для подменю Play Game.
inline constexpr std::array<const char*, 5> g_MenuItems =
{
    "Play Game",
    "Mini Games",
    "Leaderboards",
    "Help & Options",
    "Minecraft Store",
};

inline constexpr std::array<const char*, 3> g_PlayGameTabs =
{
    "Load",
    "Create",
    "Join",
};

inline constexpr const char* g_SplashText = "What DOES the fox say?";

// Глобальные указатели на шрифты живут в state, потому что ими пользуются почти все экраны меню.
extern ImFont* g_FontTitle;
extern ImFont* g_FontMenu;
extern ImFont* g_FontSubtitle;
extern ImFont* g_FontSplash;

// Состояние выделения и последних действий пользователя в главном меню.
extern int g_SelectedMenuItem;
extern int g_LastActivatedMenuItem;
extern uint64_t g_LastActivatedMenuTime;

// Все GPU-текстуры, которыми пользуется меню.
extern TextureSlot g_PanoramaTexture;
extern TextureSlot g_ButtonTexture;
extern TextureSlot g_ButtonHighlightedTexture;
extern TextureSlot g_ButtonDisabledTexture;
extern TextureSlot g_MenuLogoTexture;
extern std::array<TextureSlot, g_IntroPhotoCount> g_IntroPhotoTextures;

// Флаги ленивой загрузки помогают не перечитывать одни и те же файлы на каждом кадре.
extern bool g_PanoramaTexturesLoaded;
extern bool g_ButtonTexturesLoaded;
extern bool g_PanoramaLoadAttempted;
extern bool g_ButtonLoadAttempted;
extern bool g_MenuLogoLoadAttempted;
extern std::array<bool, g_IntroPhotoCount> g_IntroPhotoLoadAttempted;

// Текущее состояние навигации по экранам и спискам меню.
extern PanoramaVariant g_SelectedPanoramaVariant;
extern MenuScreen g_CurrentMenuScreen;
extern std::vector<PlayGameWorld> g_PlayGameWorlds;
extern int g_SelectedPlayGameTab;
extern int g_SelectedPlayGameWorld;
extern int g_PlayGameScrollOffset;
extern PendingWorldAction g_PendingWorldAction;
extern std::string g_PendingWorldDirectory;
extern std::array<char, 64> g_CreateWorldNameBuffer;
extern std::array<char, 32> g_CreateWorldSeedBuffer;
extern std::string g_MenuStatusMessage;
extern uint64_t g_MenuStatusTime;

// Таймеры интро и панорамы разделены специально: панорама начинает двигаться только после интро.
extern uint64_t g_IntroStartTime;
extern uint64_t g_PanoramaAnimationStartTime;
extern bool g_IntroFinished;

// Загрузка и сброс runtime-ресурсов меню.
std::string GetProjectAssetPath(const char* relative_path);
void ResetTextureSlot(TextureSlot& slot);
void ResetPanoramaTextures();
void ResetButtonTextures();
void ResetMenuLogoTexture();
void ResetIntroPhotoTextures();
PanoramaVariant ChoosePanoramaVariant();
const char* GetPanoramaFileForVariant(PanoramaVariant variant);
const char* GetMenuLogoFile();
void DrawPanoramaLayer(ImDrawList* draw_list, const TextureSlot& slot, const ImVec2& viewport_pos, const ImVec2& viewport_size, float time, float zoom, float pan_speed, float phase, float vertical_sway, ImU32 tint);
bool LoadTextureSlot(TextureSlot& slot, const char* relative_path, bool use_nearest_sampling = false);
bool EnsurePanoramaTexturesLoaded();
bool EnsureButtonTexturesLoaded();
bool EnsureMenuLogoTextureLoaded();
bool EnsureIntroPhotoTexturesLoaded();

// Работа со шрифтами и текстом.
float GetIntroPhotoAlpha(uint64_t elapsed_ms);
ImFont* LoadFontWithFallback(const char* primary_path, const char* fallback_path, float size_pixels, bool pixel_snap);
void LoadMenuFonts(float main_scale);
ImFont* ResolveFont(ImFont* font);
ImVec2 MeasureText(ImFont* font, const char* text, float size = 0.0f);
void DrawTextOutlined(ImDrawList* draw_list, ImFont* font, float size, const ImVec2& position, ImU32 color, ImU32 outline_color, float outline_thickness, const char* text);
void DrawSlantedText(ImDrawList* draw_list, ImFont* font, float size, ImVec2 position, ImU32 color, ImU32 outline_color, float outline_thickness, float slope, const char* text);

// Фоновые слои и интро-последовательность.
void DrawPanoramaFallbackBackground(const ImVec2& viewport_pos, const ImVec2& viewport_size);
bool DrawStartupIntro(const ImVec2& viewport_pos, const ImVec2& viewport_size);
void DrawPanoramaBackground(const ImVec2& viewport_pos, const ImVec2& viewport_size);
void DrawMinecraftLogo(const ImVec2& viewport_pos, const ImVec2& viewport_size);

// Экраны и действия меню.
void RefreshPlayGameWorldEntries();
void OpenPlayGameMenu();
void ClosePlayGameMenu();
void ActivateStartMenuItem(int index);
void DrawMenuButtons(const ImVec2& viewport_pos, const ImVec2& viewport_size);
void DrawPlayGameMenu(const ImVec2& viewport_pos, const ImVec2& viewport_size);
}
