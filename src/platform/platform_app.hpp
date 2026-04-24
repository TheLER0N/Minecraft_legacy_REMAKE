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
    void set_mouse_capture(bool enabled);
    void play_ui_press_sound();
    void play_ui_focus_sound();
    void shutdown();

private:
    struct UiSound {
        SDL_AudioStream* stream {nullptr};
        Uint8* buffer {nullptr};
        Uint32 length {0};
    };

    bool initialize_audio();
    bool load_ui_sound(const char* path, const char* name, UiSound& sound);
    void play_ui_sound(UiSound& sound);
    void shutdown_ui_sound(UiSound& sound);
    void shutdown_audio();
    void update_relative_mouse_mode();
    static std::optional<Key> map_scancode(SDL_Scancode scancode);

    PlatformWindow window_ {};
    InputState input_ {};
    UiSound press_sound_ {};
    UiSound focus_sound_ {};
    bool should_close_ {false};
    std::uint64_t last_counter_ {0};
    float frame_delta_seconds_ {0.0f};
};

}
