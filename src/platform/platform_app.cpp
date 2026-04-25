#include "platform/platform_app.hpp"

#include "common/asset_pack.hpp"
#include "common/log.hpp"

#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.h"
#undef STB_VORBIS_HEADER_ONLY

#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>

namespace ml {

namespace {

constexpr const char* kMusicDirectory = "sound/music/game/unused";
constexpr float kMinWorldMusicDelaySeconds = 30.0f;
constexpr float kMaxWorldMusicDelaySeconds = 180.0f;
constexpr float kGamepadDeadzone = 0.24f;
constexpr float kGamepadMenuThreshold = 0.55f;
constexpr float kGamepadLookPixelsPerSecond = 720.0f;
constexpr Sint16 kGamepadTriggerThreshold = 16000;

float normalize_gamepad_axis(Sint16 value) {
    constexpr float deadzone = 8000.0f;
    const float axis = static_cast<float>(value);
    if (std::abs(axis) <= deadzone) {
        return 0.0f;
    }
    if (axis > 0.0f) {
        return std::min(1.0f, (axis - deadzone) / (32767.0f - deadzone));
    }
    return std::max(-1.0f, (axis + deadzone) / (32768.0f - deadzone));
}

}

void log_message(LogLevel level, std::string_view message) {
    const char* prefix = "[info]";
    if (level == LogLevel::Warning) {
        prefix = "[warn]";
    } else if (level == LogLevel::Error) {
        prefix = "[error]";
    }

    std::cout << prefix << ' ' << message << std::endl;
}

std::optional<Key> PlatformApp::map_scancode(SDL_Scancode scancode) {
    switch (scancode) {
    case SDL_SCANCODE_W: return Key::Forward;
    case SDL_SCANCODE_S: return Key::Backward;
    case SDL_SCANCODE_A: return Key::Left;
    case SDL_SCANCODE_D: return Key::Right;
    case SDL_SCANCODE_SPACE: return Key::Up;
    case SDL_SCANCODE_LCTRL: return Key::Down;
    case SDL_SCANCODE_LSHIFT: return Key::Fast;
    default: return std::nullopt;
    }
}

bool PlatformApp::initialize() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        log_message(LogLevel::Error, SDL_GetError());
        return false;
    }

    set_initial_window_size();
    window_.handle = SDL_CreateWindow(
        "minecraft_legacy",
        static_cast<int>(window_.width),
        static_cast<int>(window_.height),
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_MAXIMIZED
    );

    if (window_.handle == nullptr) {
        log_message(LogLevel::Error, SDL_GetError());
        return false;
    }
    fullscreen_ = true;
    if (!SDL_SetWindowFullscreen(window_.handle, true)) {
        fullscreen_ = false;
        log_message(LogLevel::Warning, std::string("PlatformApp: failed to enter fullscreen at startup: ") + SDL_GetError());
    }
    update_window_pixel_size();

    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        log_message(LogLevel::Error, SDL_GetError());
        return false;
    }

    last_counter_ = SDL_GetPerformanceCounter();
    input_.capture_mouse = false;
    update_relative_mouse_mode();
    open_first_gamepad();
    initialize_audio();
    return true;
}

void PlatformApp::pump_events() {
    input_.mouse_delta = {};
    input_.toggle_mouse_pressed = false;
    input_.toggle_wireframe_pressed = false;
    input_.toggle_wireframe_textures_pressed = false;
    input_.toggle_debug_hud_pressed = false;
    input_.toggle_debug_fly_pressed = false;
    input_.toggle_leaves_render_mode_pressed = false;
    input_.toggle_section_culling_pressed = false;
    input_.toggle_occlusion_culling_pressed = false;
    input_.escape_pressed = false;
    input_.jump_pressed = false;
    input_.break_block_pressed = false;
    input_.break_block_held = mouse_break_held_;
    input_.left_click_pressed = false;
    input_.place_block_pressed = false;
    input_.menu_up_pressed = false;
    input_.menu_down_pressed = false;
    input_.menu_confirm_pressed = false;
    input_.gamepad_start_pressed = false;
    input_.selected_hotbar_slot = -1;
    input_.hotbar_scroll_delta = 0;
    input_.render_distance_delta = 0;
    input_.keys = keyboard_keys_;

    const std::uint64_t current_counter = SDL_GetPerformanceCounter();
    const std::uint64_t freq = SDL_GetPerformanceFrequency();
    frame_delta_seconds_ = static_cast<float>(current_counter - last_counter_) / static_cast<float>(freq);
    last_counter_ = current_counter;

    SDL_Event event {};
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
            should_close_ = true;
            input_.close_requested = true;
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            window_.width = static_cast<std::uint32_t>(event.window.data1);
            window_.height = static_cast<std::uint32_t>(event.window.data2);
            update_window_pixel_size();
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_F11 ||
                    (event.key.scancode == SDL_SCANCODE_RETURN && (event.key.mod & SDL_KMOD_ALT) != 0)) {
                    toggle_fullscreen();
                }
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    input_.escape_pressed = true;
                }
                if (event.key.scancode == SDL_SCANCODE_F1) {
                    input_.toggle_wireframe_pressed = true;
                }
                if (event.key.scancode == SDL_SCANCODE_F2) {
                    input_.toggle_debug_fly_pressed = true;
                }
                if (event.key.scancode == SDL_SCANCODE_F3) {
                    input_.toggle_debug_hud_pressed = true;
                }
                if (event.key.scancode == SDL_SCANCODE_F4) {
                    input_.toggle_leaves_render_mode_pressed = true;
                }
                if (event.key.scancode == SDL_SCANCODE_F5) {
                    input_.toggle_section_culling_pressed = true;
                }
                if (event.key.scancode == SDL_SCANCODE_F6) {
                    input_.toggle_occlusion_culling_pressed = true;
                }
                if ((event.key.mod & SDL_KMOD_CTRL) != 0 && event.key.scancode == SDL_SCANCODE_F7) {
                    input_.render_distance_delta = -1;
                }
                if ((event.key.mod & SDL_KMOD_CTRL) != 0 && event.key.scancode == SDL_SCANCODE_F8) {
                    input_.render_distance_delta = 1;
                }
                if (event.key.scancode == SDL_SCANCODE_M) {
                    input_.toggle_wireframe_textures_pressed = true;
                }
                if (event.key.scancode == SDL_SCANCODE_SPACE) {
                    input_.jump_pressed = true;
                }
                if (event.key.scancode == SDL_SCANCODE_1) {
                    input_.selected_hotbar_slot = 0;
                }
                if (event.key.scancode == SDL_SCANCODE_2) {
                    input_.selected_hotbar_slot = 1;
                }
                if (event.key.scancode == SDL_SCANCODE_3) {
                    input_.selected_hotbar_slot = 2;
                }
                if (event.key.scancode == SDL_SCANCODE_4) {
                    input_.selected_hotbar_slot = 3;
                }
                if (event.key.scancode == SDL_SCANCODE_5) {
                    input_.selected_hotbar_slot = 4;
                }
                if (event.key.scancode == SDL_SCANCODE_6) {
                    input_.selected_hotbar_slot = 5;
                }
                if (event.key.scancode == SDL_SCANCODE_7) {
                    input_.selected_hotbar_slot = 6;
                }
                if (event.key.scancode == SDL_SCANCODE_8) {
                    input_.selected_hotbar_slot = 7;
                }
                if (event.key.scancode == SDL_SCANCODE_9) {
                    input_.selected_hotbar_slot = 8;
                }

                if (const auto mapped = map_scancode(event.key.scancode); mapped.has_value()) {
                    keyboard_keys_[static_cast<std::size_t>(*mapped)] = true;
                    input_.keys[static_cast<std::size_t>(*mapped)] = true;
                }
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (const auto mapped = map_scancode(event.key.scancode); mapped.has_value()) {
                keyboard_keys_[static_cast<std::size_t>(*mapped)] = false;
                input_.keys[static_cast<std::size_t>(*mapped)] = false;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            input_.mouse_position = window_to_pixel_position(event.motion.x, event.motion.y);
            input_.mouse_inside_window = true;
            if (input_.capture_mouse) {
                input_.mouse_delta.x += static_cast<float>(event.motion.xrel);
                input_.mouse_delta.y += static_cast<float>(event.motion.yrel);
            }
            break;
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
            input_.mouse_inside_window = true;
            break;
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            input_.mouse_inside_window = false;
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (active_gamepad_ == nullptr) {
                open_gamepad(event.gdevice.which);
            }
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (event.gdevice.which == active_gamepad_id_) {
                close_active_gamepad();
                open_first_gamepad();
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!event.button.down) {
                break;
            }
            input_.mouse_position = window_to_pixel_position(event.button.x, event.button.y);
            if (event.button.button == SDL_BUTTON_LEFT) {
                input_.break_block_pressed = true;
                mouse_break_held_ = true;
                input_.break_block_held = true;
                input_.left_click_pressed = true;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                input_.place_block_pressed = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                mouse_break_held_ = false;
                input_.break_block_held = false;
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            int scroll_y = event.wheel.y > 0.0f ? 1 : (event.wheel.y < 0.0f ? -1 : 0);
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                scroll_y = -scroll_y;
            }
            input_.hotbar_scroll_delta += scroll_y;
            break;
        }
        default:
            break;
        }
    }
    update_gamepad_input();
}

bool PlatformApp::should_close() const {
    return should_close_;
}

const InputState& PlatformApp::current_input() const {
    return input_;
}

float PlatformApp::frame_delta_seconds() const {
    return frame_delta_seconds_;
}

const PlatformWindow& PlatformApp::window() const {
    return window_;
}

std::string PlatformApp::shader_directory() const {
    const char* base_path = SDL_GetBasePath();
    if (base_path == nullptr) {
        return "shaders";
    }

    std::string path(base_path);
    if (!path.empty() && path.back() != '\\' && path.back() != '/') {
        path.push_back('/');
    }
    path += "shaders";
    return path;
}

void PlatformApp::set_mouse_capture(bool enabled) {
    if (input_.capture_mouse == enabled) {
        return;
    }
    input_.capture_mouse = enabled;
    update_relative_mouse_mode();
}

void PlatformApp::play_ui_press_sound() {
    play_ui_sound(press_sound_);
}

void PlatformApp::play_ui_focus_sound() {
    play_ui_sound(focus_sound_);
}

void PlatformApp::start_menu_music() {
    music_enabled_ = true;
    music_in_world_ = false;
    discover_music_tracks();
    if (music_.stream == nullptr) {
        schedule_next_music_track(true);
        play_next_music_track();
    }
}

void PlatformApp::enter_world_music() {
    music_enabled_ = true;
    music_in_world_ = true;
    discover_music_tracks();
    if (music_.stream == nullptr) {
        schedule_next_music_track(true);
        play_next_music_track();
    }
}

void PlatformApp::update_music(float dt) {
    if (!music_enabled_) {
        return;
    }

    if (music_.stream != nullptr && SDL_GetAudioStreamQueued(music_.stream) <= 0) {
        stop_music_track();
        schedule_next_music_track(false);
    }

    if (music_.stream != nullptr) {
        return;
    }

    next_music_delay_seconds_ = std::max(0.0f, next_music_delay_seconds_ - dt);
    if (next_music_delay_seconds_ <= 0.0f) {
        play_next_music_track();
    }
}

void PlatformApp::play_ui_sound(UiSound& sound) {
    if (sound.stream == nullptr || sound.buffer == nullptr || sound.length == 0) {
        return;
    }
    SDL_ClearAudioStream(sound.stream);
    SDL_PutAudioStreamData(sound.stream, sound.buffer, static_cast<int>(sound.length));
    SDL_ResumeAudioStreamDevice(sound.stream);
}

void PlatformApp::shutdown() {
    shutdown_audio();
    close_active_gamepad();
    if (window_.handle != nullptr) {
        SDL_DestroyWindow(window_.handle);
        window_.handle = nullptr;
    }
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
}

bool PlatformApp::initialize_audio() {
    const AssetPackResolver resolver;
    const std::string press_path = resolver.resolve_file_utf8("sound/ui/press.wav");
    const std::string focus_path = resolver.resolve_file_utf8("sound/ui/focus.wav");
    const bool loaded_press = load_ui_sound(press_path.c_str(), "press", press_sound_);
    const bool loaded_focus = load_ui_sound(focus_path.c_str(), "focus", focus_sound_);
    return loaded_press || loaded_focus;
}

bool PlatformApp::load_ui_sound(const char* path, const char* name, UiSound& sound) {
    SDL_AudioSpec spec {};
    if (!SDL_LoadWAV(path, &spec, &sound.buffer, &sound.length)) {
        log_message(LogLevel::Warning, std::string("PlatformApp: failed to load ") + name + " sound: " + SDL_GetError());
        return false;
    }

    sound.stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (sound.stream == nullptr) {
        log_message(LogLevel::Warning, std::string("PlatformApp: failed to open ") + name + " audio stream: " + SDL_GetError());
        SDL_free(sound.buffer);
        sound.buffer = nullptr;
        sound.length = 0;
        return false;
    }

    return true;
}

void PlatformApp::shutdown_audio() {
    stop_music_track();
    shutdown_ui_sound(press_sound_);
    shutdown_ui_sound(focus_sound_);
}

void PlatformApp::shutdown_ui_sound(UiSound& sound) {
    if (sound.stream != nullptr) {
        SDL_DestroyAudioStream(sound.stream);
        sound.stream = nullptr;
    }
    if (sound.buffer != nullptr) {
        SDL_free(sound.buffer);
        sound.buffer = nullptr;
    }
    sound.length = 0;
}

void PlatformApp::discover_music_tracks() {
    if (music_tracks_discovered_) {
        return;
    }
    music_tracks_discovered_ = true;

    const AssetPackResolver resolver;
    const std::vector<std::filesystem::path> search_paths = resolver.resolve_directories(kMusicDirectory);

    for (const std::filesystem::path& music_path : search_paths) {
        std::error_code error;
        if (!std::filesystem::exists(music_path, error) || !std::filesystem::is_directory(music_path, error)) {
            continue;
        }

        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(music_path, error)) {
            if (error) {
                break;
            }
            if (!entry.is_regular_file(error)) {
                continue;
            }
            std::filesystem::path path = entry.path();
            std::string extension = path.extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (extension != ".ogg") {
                continue;
            }

            music_tracks_.push_back({path_to_utf8(path), path_to_utf8(path.filename())});
        }

        if (!music_tracks_.empty()) {
            break;
        }
    }

    if (music_tracks_.empty()) {
        log_message(LogLevel::Warning, std::string("PlatformApp: no OGG music found in asset pack path ") + kMusicDirectory);
        return;
    }

    log_message(LogLevel::Info, std::string("PlatformApp: discovered music tracks=") + std::to_string(music_tracks_.size()));
}

void PlatformApp::play_next_music_track() {
    if (music_tracks_.empty()) {
        discover_music_tracks();
    }
    if (music_tracks_.empty()) {
        return;
    }

    const int max_attempts = static_cast<int>(music_tracks_.size());
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const int track_index = choose_music_track_index();
        if (track_index < 0) {
            return;
        }

        const MusicTrack& track = music_tracks_[static_cast<std::size_t>(track_index)];
        std::size_t file_size = 0;
        void* file_data = SDL_LoadFile(track.path.c_str(), &file_size);
        if (file_data == nullptr) {
            log_message(LogLevel::Warning, std::string("PlatformApp: failed to load music ") + track.name + ": " + SDL_GetError());
            continue;
        }

        if (file_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            SDL_free(file_data);
            log_message(LogLevel::Warning, std::string("PlatformApp: music file too large ") + track.name);
            continue;
        }

        int channels = 0;
        int sample_rate = 0;
        short* samples = nullptr;
        const int sample_frames = stb_vorbis_decode_memory(
            static_cast<const unsigned char*>(file_data),
            static_cast<int>(file_size),
            &channels,
            &sample_rate,
            &samples
        );
        SDL_free(file_data);

        if (sample_frames <= 0 || samples == nullptr || channels <= 0 || sample_rate <= 0) {
            if (samples != nullptr) {
                std::free(samples);
            }
            log_message(LogLevel::Warning, std::string("PlatformApp: failed to decode music ") + track.name);
            continue;
        }

        const std::int64_t byte_length = static_cast<std::int64_t>(sample_frames) * channels * static_cast<int>(sizeof(short));
        if (byte_length <= 0 || byte_length > std::numeric_limits<int>::max()) {
            std::free(samples);
            log_message(LogLevel::Warning, std::string("PlatformApp: decoded music too large ") + track.name);
            continue;
        }

        const SDL_AudioSpec spec {SDL_AUDIO_S16, channels, sample_rate};
        SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (stream == nullptr) {
            std::free(samples);
            log_message(LogLevel::Warning, std::string("PlatformApp: failed to open music stream: ") + SDL_GetError());
            return;
        }

        if (!SDL_PutAudioStreamData(stream, samples, static_cast<int>(byte_length)) ||
            !SDL_FlushAudioStream(stream) ||
            !SDL_ResumeAudioStreamDevice(stream)) {
            SDL_DestroyAudioStream(stream);
            std::free(samples);
            log_message(LogLevel::Warning, std::string("PlatformApp: failed to start music ") + track.name + ": " + SDL_GetError());
            continue;
        }

        music_.stream = stream;
        music_.samples = samples;
        music_.byte_length = static_cast<int>(byte_length);
        last_music_track_index_ = track_index;
        next_music_delay_seconds_ = 0.0f;
        log_message(LogLevel::Info, std::string("PlatformApp: playing music ") + track.name);
        return;
    }

    schedule_next_music_track(false);
}

void PlatformApp::stop_music_track() {
    if (music_.stream != nullptr) {
        SDL_DestroyAudioStream(music_.stream);
        music_.stream = nullptr;
    }
    if (music_.samples != nullptr) {
        std::free(music_.samples);
        music_.samples = nullptr;
    }
    music_.byte_length = 0;
}

void PlatformApp::schedule_next_music_track(bool immediate) {
    if (immediate) {
        next_music_delay_seconds_ = 0.0f;
        return;
    }

    std::uniform_real_distribution<float> delay_distribution(kMinWorldMusicDelaySeconds, kMaxWorldMusicDelaySeconds);
    next_music_delay_seconds_ = delay_distribution(music_rng_);
    log_message(LogLevel::Info, std::string("PlatformApp: next music in ") + std::to_string(static_cast<int>(next_music_delay_seconds_)) + "s");
}

bool PlatformApp::is_music_playing() const {
    return music_.stream != nullptr && SDL_GetAudioStreamQueued(music_.stream) > 0;
}

int PlatformApp::choose_music_track_index() {
    if (music_tracks_.empty()) {
        return -1;
    }
    if (music_tracks_.size() == 1) {
        return 0;
    }

    std::uniform_int_distribution<int> track_distribution(0, static_cast<int>(music_tracks_.size() - 1));
    int track_index = track_distribution(music_rng_);
    if (track_index == last_music_track_index_) {
        track_index = (track_index + 1) % static_cast<int>(music_tracks_.size());
    }
    return track_index;
}

std::string PlatformApp::path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

void PlatformApp::set_initial_window_size() {
    SDL_DisplayID display = SDL_GetPrimaryDisplay();
    if (display == 0) {
        return;
    }

    const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(display);
    if (mode == nullptr || mode->w <= 0 || mode->h <= 0) {
        return;
    }

    window_.width = static_cast<std::uint32_t>(mode->w);
    window_.height = static_cast<std::uint32_t>(mode->h);
    log_message(
        LogLevel::Info,
        std::string("PlatformApp: desktop resolution ") + std::to_string(window_.width) + "x" + std::to_string(window_.height)
    );
}

void PlatformApp::update_window_pixel_size() {
    if (window_.handle == nullptr) {
        return;
    }

    int pixel_width = 0;
    int pixel_height = 0;
    if (SDL_GetWindowSizeInPixels(window_.handle, &pixel_width, &pixel_height) && pixel_width > 0 && pixel_height > 0) {
        window_.width = static_cast<std::uint32_t>(pixel_width);
        window_.height = static_cast<std::uint32_t>(pixel_height);
    }
}

Vec2 PlatformApp::window_to_pixel_position(float x, float y) const {
    if (window_.handle == nullptr) {
        return {x, y};
    }

    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSize(window_.handle, &window_width, &window_height) || window_width <= 0 || window_height <= 0) {
        return {x, y};
    }

    const float scale_x = static_cast<float>(window_.width) / static_cast<float>(window_width);
    const float scale_y = static_cast<float>(window_.height) / static_cast<float>(window_height);
    return {x * scale_x, y * scale_y};
}

void PlatformApp::toggle_fullscreen() {
    if (window_.handle == nullptr) {
        return;
    }

    fullscreen_ = !fullscreen_;
    if (!SDL_SetWindowFullscreen(window_.handle, fullscreen_)) {
        fullscreen_ = !fullscreen_;
        log_message(LogLevel::Warning, std::string("PlatformApp: failed to toggle fullscreen: ") + SDL_GetError());
        return;
    }

    update_window_pixel_size();
    log_message(LogLevel::Info, fullscreen_ ? "PlatformApp: fullscreen enabled" : "PlatformApp: fullscreen disabled");
}

void PlatformApp::open_first_gamepad() {
    int count = 0;
    SDL_JoystickID* gamepads = SDL_GetGamepads(&count);
    if (gamepads == nullptr) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (active_gamepad_ == nullptr) {
            open_gamepad(gamepads[i]);
        }
    }
    SDL_free(gamepads);
}

void PlatformApp::open_gamepad(SDL_JoystickID joystick_id) {
    if (active_gamepad_ != nullptr) {
        return;
    }
    if (!SDL_IsGamepad(joystick_id)) {
        return;
    }

    active_gamepad_ = SDL_OpenGamepad(joystick_id);
    if (active_gamepad_ == nullptr) {
        log_message(LogLevel::Warning, std::string("PlatformApp: failed to open gamepad: ") + SDL_GetError());
        return;
    }

    active_gamepad_id_ = SDL_GetGamepadID(active_gamepad_);
    previous_gamepad_buttons_.fill(false);
    previous_left_trigger_down_ = false;
    previous_right_trigger_down_ = false;
    previous_menu_stick_direction_ = 0;
    const char* name = SDL_GetGamepadName(active_gamepad_);
    log_message(LogLevel::Info, std::string("PlatformApp: gamepad connected: ") + (name != nullptr ? name : "unknown"));
}

void PlatformApp::close_active_gamepad() {
    if (active_gamepad_ == nullptr) {
        return;
    }

    const char* name = SDL_GetGamepadName(active_gamepad_);
    log_message(LogLevel::Info, std::string("PlatformApp: gamepad disconnected: ") + (name != nullptr ? name : "unknown"));
    SDL_CloseGamepad(active_gamepad_);
    active_gamepad_ = nullptr;
    active_gamepad_id_ = 0;
    previous_gamepad_buttons_.fill(false);
    previous_left_trigger_down_ = false;
    previous_right_trigger_down_ = false;
    previous_menu_stick_direction_ = 0;
}

void PlatformApp::update_gamepad_input() {
    if (active_gamepad_ == nullptr) {
        return;
    }

    const auto button_down = [&](SDL_GamepadButton button) {
        return SDL_GetGamepadButton(active_gamepad_, button);
    };
    const auto button_pressed = [&](SDL_GamepadButton button) {
        const std::size_t index = static_cast<std::size_t>(button);
        return button_down(button) && !previous_gamepad_buttons_[index];
    };
    const auto set_key = [&](Key key, bool pressed) {
        if (pressed) {
            input_.keys[static_cast<std::size_t>(key)] = true;
        }
    };

    const float left_x = normalize_gamepad_axis(SDL_GetGamepadAxis(active_gamepad_, SDL_GAMEPAD_AXIS_LEFTX));
    const float left_y = normalize_gamepad_axis(SDL_GetGamepadAxis(active_gamepad_, SDL_GAMEPAD_AXIS_LEFTY));
    const float right_x = normalize_gamepad_axis(SDL_GetGamepadAxis(active_gamepad_, SDL_GAMEPAD_AXIS_RIGHTX));
    const float right_y = normalize_gamepad_axis(SDL_GetGamepadAxis(active_gamepad_, SDL_GAMEPAD_AXIS_RIGHTY));
    const Sint16 left_trigger = SDL_GetGamepadAxis(active_gamepad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    const Sint16 right_trigger = SDL_GetGamepadAxis(active_gamepad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    set_key(Key::Forward, left_y < -kGamepadDeadzone);
    set_key(Key::Backward, left_y > kGamepadDeadzone);
    set_key(Key::Left, left_x < -kGamepadDeadzone);
    set_key(Key::Right, left_x > kGamepadDeadzone);

    const bool south_down = button_down(SDL_GAMEPAD_BUTTON_SOUTH);
    set_key(Key::Up, south_down);
    if (button_pressed(SDL_GAMEPAD_BUTTON_SOUTH)) {
        input_.jump_pressed = true;
        input_.menu_confirm_pressed = true;
    }

    if (input_.capture_mouse) {
        input_.mouse_delta.x += right_x * kGamepadLookPixelsPerSecond * frame_delta_seconds_;
        input_.mouse_delta.y += right_y * kGamepadLookPixelsPerSecond * frame_delta_seconds_;
    }

    const bool left_trigger_down = left_trigger > kGamepadTriggerThreshold;
    if (left_trigger_down && !previous_left_trigger_down_) {
        input_.place_block_pressed = true;
    }
    previous_left_trigger_down_ = left_trigger_down;

    const bool right_trigger_down = right_trigger > kGamepadTriggerThreshold;
    if (right_trigger_down && !previous_right_trigger_down_) {
        input_.break_block_pressed = true;
    }
    if (right_trigger_down) {
        input_.break_block_held = true;
    }
    previous_right_trigger_down_ = right_trigger_down;

    if (button_pressed(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) {
        input_.hotbar_scroll_delta += 1;
    }
    if (button_pressed(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) {
        input_.hotbar_scroll_delta -= 1;
    }

    if (button_pressed(SDL_GAMEPAD_BUTTON_START)) {
        input_.menu_confirm_pressed = true;
        input_.gamepad_start_pressed = true;
    }

    if (button_pressed(SDL_GAMEPAD_BUTTON_DPAD_UP)) {
        input_.menu_up_pressed = true;
    }
    if (button_pressed(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) {
        input_.menu_down_pressed = true;
    }

    int menu_stick_direction = 0;
    if (left_y < -kGamepadMenuThreshold) {
        menu_stick_direction = -1;
    } else if (left_y > kGamepadMenuThreshold) {
        menu_stick_direction = 1;
    }
    if (menu_stick_direction != 0 && previous_menu_stick_direction_ == 0) {
        if (menu_stick_direction < 0) {
            input_.menu_up_pressed = true;
        } else {
            input_.menu_down_pressed = true;
        }
    }
    previous_menu_stick_direction_ = menu_stick_direction;

    for (std::size_t i = 0; i < previous_gamepad_buttons_.size(); ++i) {
        previous_gamepad_buttons_[i] = button_down(static_cast<SDL_GamepadButton>(i));
    }
}

void PlatformApp::update_relative_mouse_mode() {
    SDL_SetWindowRelativeMouseMode(window_.handle, input_.capture_mouse);
    if (input_.capture_mouse) {
        SDL_HideCursor();
    } else {
        SDL_ShowCursor();
    }
}

}
