#pragma once

#include "common/math.hpp"

#include <array>
#include <cstdint>

namespace ml {

enum class Key : std::uint8_t {
    Forward,
    Backward,
    Left,
    Right,
    Up,
    Down,
    Fast,
    Count
};

struct InputState {
    std::array<bool, static_cast<std::size_t>(Key::Count)> keys {};
    Vec2 mouse_delta {};
    bool capture_mouse {true};
    bool toggle_mouse_pressed {false};
    bool toggle_wireframe_pressed {false};
    bool toggle_debug_fly_pressed {false};
    bool jump_pressed {false};
    bool break_block_pressed {false};
    bool place_block_pressed {false};
    int selected_hotbar_slot {-1};
    bool close_requested {false};

    bool pressed(Key key) const {
        return keys[static_cast<std::size_t>(key)];
    }
};

}
