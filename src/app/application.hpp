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
    struct BlockBreakState {
        std::optional<Int3> target {};
        float repeat_seconds {0.0f};
    };

    PlatformApp platform_;
    Renderer renderer_;
    BlockRegistry block_registry_;
    DebugCamera camera_;
    PlayerController player_;
    std::unique_ptr<WorldStreamer> world_streamer_;
    std::optional<BlockHit> hovered_block_;
    BlockBreakState block_break_ {};
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
