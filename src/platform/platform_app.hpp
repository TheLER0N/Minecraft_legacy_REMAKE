#pragma once

#include "common/log.hpp"
#include "platform/input.hpp"

#include <SDL3/SDL.h>

#include <optional>
#include <string>

namespace ml {

struct PlatformWindow {
    SDL_Window* handle {nullptr};
    std::uint32_t width {1600};
    std::uint32_t height {900};
};

class PlatformApp {
public:
    bool initialize();
    void pump_events();
    bool should_close() const;
    const InputState& current_input() const;
    float frame_delta_seconds() const;
    const PlatformWindow& window() const;
    std::string shader_directory() const;
    void shutdown();

private:
    void update_relative_mouse_mode();
    static std::optional<Key> map_scancode(SDL_Scancode scancode);

    PlatformWindow window_ {};
    InputState input_ {};
    bool should_close_ {false};
    std::uint64_t last_counter_ {0};
    float frame_delta_seconds_ {0.0f};
};

}
