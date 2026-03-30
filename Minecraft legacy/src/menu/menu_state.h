#pragma once

#include "imgui.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace MenuInternal
{
// Текстура уже загружена в ImGui/Vulkan и готова к многократной отрисовке в меню.
struct TextureSlot
{
    ImTextureData* Texture = nullptr;
    int Width = 0;
    int Height = 0;
};

enum class PanoramaVariant
{
    Day,
    Night,
};

enum class MenuScreen
{
    Start,
    PlayGame,
};

enum class PlayGameTab
{
    Load,
    Create,
    Join,
};

struct PlayGameWorld
{
    const char* Name;
    const char* GameMode;
    const char* LastPlayed;
    const char* Description;
};

inline constexpr const char* g_PanoramaDayFile = "assets\\panorama\\panorama_tu69_day.png";
inline constexpr const char* g_PanoramaNightFile = "assets\\panorama\\panorama_tu69_night.png";
inline constexpr const char* g_ButtonNormalFile = "assets\\button\\button.png";
inline constexpr const char* g_ButtonHighlightedFile = "assets\\button\\button_highlighted.png";
inline constexpr const char* g_ButtonDisabledFile = "assets\\button\\button_disabled.png";
inline constexpr const char* g_MenuLogoFile = "assets\\ui\\logo\\legacy_console_edition_logo.png.png";
inline constexpr const char* g_MenuLogoHolidayFile = "assets\\ui\\logo\\legacy_console_edition_logo_holiday.png.png";
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

inline constexpr std::array<PlayGameWorld, 6> g_PlayGameWorlds =
{
    PlayGameWorld{ "New World", "Survival", "Today", "A standard survival world with the default texture pack." },
    PlayGameWorld{ "New World", "Creative", "Today", "A flat creative save used for fast block testing." },
    PlayGameWorld{ "Kit Pvp", "Adventure", "Yesterday", "A compact arena map with pre-made kits." },
    PlayGameWorld{ "New World", "Survival", "This Week", "Another survival save with a nearby village spawn." },
    PlayGameWorld{ "Tutorial", "Creative", "This Week", "A tutorial world inspired by Legacy Console Edition." },
    PlayGameWorld{ "Kit Pvp", "Adventure", "This Month", "A second versus map for local matches." },
};

inline constexpr const char* g_SplashText = "What DOES the fox say?";

extern ImFont* g_FontTitle;
extern ImFont* g_FontMenu;
extern ImFont* g_FontSubtitle;
extern ImFont* g_FontSplash;
extern int g_SelectedMenuItem;
extern int g_LastActivatedMenuItem;
extern uint64_t g_LastActivatedMenuTime;

extern TextureSlot g_PanoramaTexture;
extern TextureSlot g_ButtonTexture;
extern TextureSlot g_ButtonHighlightedTexture;
extern TextureSlot g_ButtonDisabledTexture;
extern TextureSlot g_MenuLogoTexture;
extern std::array<TextureSlot, g_IntroPhotoCount> g_IntroPhotoTextures;
extern bool g_PanoramaTexturesLoaded;
extern bool g_ButtonTexturesLoaded;
extern bool g_PanoramaLoadAttempted;
extern bool g_ButtonLoadAttempted;
extern bool g_MenuLogoLoadAttempted;
extern std::array<bool, g_IntroPhotoCount> g_IntroPhotoLoadAttempted;
extern PanoramaVariant g_SelectedPanoramaVariant;
extern MenuScreen g_CurrentMenuScreen;
extern int g_SelectedPlayGameTab;
extern int g_SelectedPlayGameWorld;
extern int g_PlayGameScrollOffset;
extern uint64_t g_IntroStartTime;
extern uint64_t g_PanoramaAnimationStartTime;
extern bool g_IntroFinished;

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
float GetIntroPhotoAlpha(uint64_t elapsed_ms);
ImFont* LoadFontWithFallback(const char* primary_path, const char* fallback_path, float size_pixels, bool pixel_snap);
void LoadMenuFonts(float main_scale);
ImFont* ResolveFont(ImFont* font);
ImVec2 MeasureText(ImFont* font, const char* text, float size = 0.0f);
void DrawTextOutlined(ImDrawList* draw_list, ImFont* font, float size, const ImVec2& position, ImU32 color, ImU32 outline_color, float outline_thickness, const char* text);
void DrawSlantedText(ImDrawList* draw_list, ImFont* font, float size, ImVec2 position, ImU32 color, ImU32 outline_color, float outline_thickness, float slope, const char* text);
void DrawPanoramaFallbackBackground(const ImVec2& viewport_pos, const ImVec2& viewport_size);
bool DrawStartupIntro(const ImVec2& viewport_pos, const ImVec2& viewport_size);
void DrawPanoramaBackground(const ImVec2& viewport_pos, const ImVec2& viewport_size);
void DrawMinecraftLogo(const ImVec2& viewport_pos, const ImVec2& viewport_size);
void OpenPlayGameMenu();
void ClosePlayGameMenu();
void ActivateStartMenuItem(int index);
void DrawMenuButtons(const ImVec2& viewport_pos, const ImVec2& viewport_size);
void DrawPlayGameMenu(const ImVec2& viewport_pos, const ImVec2& viewport_size);
}
