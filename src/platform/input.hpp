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
    Vec2 mouse_position {};
    bool capture_mouse {true};
    bool mouse_inside_window {false};
    bool toggle_mouse_pressed {false};
    bool toggle_wireframe_pressed {false};
    bool toggle_wireframe_textures_pressed {false};
    bool toggle_debug_hud_pressed {false};
    bool toggle_debug_fly_pressed {false};
    bool toggle_leaves_render_mode_pressed {false};
    bool toggle_section_culling_pressed {false};
    bool toggle_occlusion_culling_pressed {false};
    bool escape_pressed {false};
    bool jump_pressed {false};
    bool break_block_pressed {false};
    bool break_block_held {false};
    bool left_click_pressed {false};
    bool place_block_pressed {false};
    bool menu_up_pressed {false};
    bool menu_down_pressed {false};
    bool menu_confirm_pressed {false};
    bool gamepad_start_pressed {false};
    int selected_hotbar_slot {-1};
    int hotbar_scroll_delta {0};
    int render_distance_delta {0};
    bool close_requested {false};

    bool pressed(Key key) const {
        return keys[static_cast<std::size_t>(key)];
    }
};

}
