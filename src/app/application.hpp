#pragma once

#include "game/block.hpp"
#include "game/debug_camera.hpp"
#include "game/player_controller.hpp"
#include "game/world_streamer.hpp"
#include "platform/platform_app.hpp"
#include "render/renderer.hpp"

#include <array>
#include <memory>
#include <optional>

namespace ml {

class Application {
public:
    Application();
    ~Application();

    bool initialize();
    int run();

private:
    enum class AppState {
        StartupSplash,
        MainMenu,
        InWorld
    };

    struct MenuButton {
        float left {0.0f};
        float top {0.0f};
        float right {0.0f};
        float bottom {0.0f};
    };

    struct BlockBreakState {
        std::optional<Int3> target {};
        float repeat_seconds {0.0f};
    };

    void start_world();
    int hovered_menu_button(const InputState& input) const;
    std::array<MenuButton, 6> menu_buttons() const;

    PlatformApp platform_;
    Renderer renderer_;
    BlockRegistry block_registry_;
    DebugCamera camera_;
    PlayerController player_;
    std::unique_ptr<WorldStreamer> world_streamer_;
    AppState app_state_ {AppState::StartupSplash};
    bool menu_uses_night_panorama_ {false};
    bool menu_exit_requested_ {false};
    bool startup_skip_requested_ {false};
    float startup_splash_seconds_ {0.0f};
    float startup_skip_fade_seconds_ {0.0f};
    float menu_time_seconds_ {0.0f};
    float menu_exit_delay_seconds_ {0.0f};
    int last_hovered_menu_button_ {-1};
    int selected_menu_button_ {0};
    std::optional<BlockHit> hovered_block_;
    BlockBreakState block_break_ {};
    LeavesRenderMode leaves_render_mode_ {LeavesRenderMode::Fancy};
    bool debug_fly_enabled_ {false};
    bool debug_hud_enabled_ {false};
    float debug_fps_ {0.0f};
    float debug_fps_accumulator_ {0.0f};
    int debug_fps_frames_ {0};
    std::array<BlockId, 9> hotbar_ {
        BlockId::Grass,
        BlockId::Dirt,
        BlockId::Stone,
        BlockId::Sand,
        BlockId::Gravel,
        BlockId::OakLog,
        BlockId::OakLeaves,
        BlockId::Stone,
        BlockId::Stone
    };
    std::size_t selected_hotbar_slot_ {0};
};

}
