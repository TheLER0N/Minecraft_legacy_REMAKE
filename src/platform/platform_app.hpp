#pragma once

#include "common/log.hpp"
#include "platform/input.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <optional>
#include <random>
#include <string>
#include <vector>

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
    void start_menu_music();
    void enter_world_music();
    void update_music(float dt);
    void shutdown();

private:
    struct UiSound {
        SDL_AudioStream* stream {nullptr};
        Uint8* buffer {nullptr};
        Uint32 length {0};
    };

    struct MusicTrack {
        std::string path {};
        std::string name {};
    };

    struct MusicPlayback {
        SDL_AudioStream* stream {nullptr};
        short* samples {nullptr};
        int byte_length {0};
    };

    bool initialize_audio();
    bool load_ui_sound(const char* path, const char* name, UiSound& sound);
    void play_ui_sound(UiSound& sound);
    void shutdown_ui_sound(UiSound& sound);
    void discover_music_tracks();
    void play_next_music_track();
    void stop_music_track();
    void schedule_next_music_track(bool immediate);
    bool is_music_playing() const;
    int choose_music_track_index();
    static std::string path_to_utf8(const std::filesystem::path& path);
    void shutdown_audio();
    void set_initial_window_size();
    void update_window_pixel_size();
    Vec2 window_to_pixel_position(float x, float y) const;
    void toggle_fullscreen();
    void update_relative_mouse_mode();
    void open_first_gamepad();
    void open_gamepad(SDL_JoystickID joystick_id);
    void close_active_gamepad();
    void update_gamepad_input();
    static std::optional<Key> map_scancode(SDL_Scancode scancode);

    PlatformWindow window_ {};
    InputState input_ {};
    std::array<bool, static_cast<std::size_t>(Key::Count)> keyboard_keys_ {};
    UiSound press_sound_ {};
    UiSound focus_sound_ {};
    std::vector<MusicTrack> music_tracks_ {};
    MusicPlayback music_ {};
    std::mt19937 music_rng_ {std::random_device {}()};
    int last_music_track_index_ {-1};
    float next_music_delay_seconds_ {0.0f};
    bool music_tracks_discovered_ {false};
    bool music_enabled_ {false};
    bool music_in_world_ {false};
    bool fullscreen_ {false};
    bool mouse_break_held_ {false};
    bool should_close_ {false};
    SDL_Gamepad* active_gamepad_ {nullptr};
    SDL_JoystickID active_gamepad_id_ {0};
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> previous_gamepad_buttons_ {};
    bool previous_left_trigger_down_ {false};
    bool previous_right_trigger_down_ {false};
    int previous_menu_stick_direction_ {0};
    std::uint64_t last_counter_ {0};
    float frame_delta_seconds_ {0.0f};
};

}
