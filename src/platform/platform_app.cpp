#include "platform/platform_app.hpp"

#include "common/log.hpp"

#include <SDL3/SDL_vulkan.h>

#include <iostream>
#include <sstream>

namespace ml {

void log_message(LogLevel level, std::string_view message) {
    const char* prefix = "[info]";
    if (level == LogLevel::Warning) {
        prefix = "[warn]";
    } else if (level == LogLevel::Error) {
        prefix = "[error]";
    }

    std::cout << prefix << ' ' << message << '\n';
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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        log_message(LogLevel::Error, SDL_GetError());
        return false;
    }

    window_.handle = SDL_CreateWindow(
        "minecraft_legacy",
        static_cast<int>(window_.width),
        static_cast<int>(window_.height),
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
    );

    if (window_.handle == nullptr) {
        log_message(LogLevel::Error, SDL_GetError());
        return false;
    }

    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        log_message(LogLevel::Error, SDL_GetError());
        return false;
    }

    last_counter_ = SDL_GetPerformanceCounter();
    input_.capture_mouse = false;
    update_relative_mouse_mode();
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
    input_.jump_pressed = false;
    input_.break_block_pressed = false;
    input_.left_click_pressed = false;
    input_.place_block_pressed = false;
    input_.selected_hotbar_slot = -1;
    input_.hotbar_scroll_delta = 0;

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
            window_.width = static_cast<std::uint32_t>(event.window.data1);
            window_.height = static_cast<std::uint32_t>(event.window.data2);
            break;
        case SDL_EVENT_KEY_DOWN:
            if (!event.key.repeat) {
                if (event.key.scancode == SDL_SCANCODE_ESCAPE) {
                    input_.capture_mouse = !input_.capture_mouse;
                    input_.toggle_mouse_pressed = true;
                    update_relative_mouse_mode();
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
                    input_.keys[static_cast<std::size_t>(*mapped)] = true;
                }
            }
            break;
        case SDL_EVENT_KEY_UP:
            if (const auto mapped = map_scancode(event.key.scancode); mapped.has_value()) {
                input_.keys[static_cast<std::size_t>(*mapped)] = false;
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            input_.mouse_position = {event.motion.x, event.motion.y};
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
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!event.button.down) {
                break;
            }
            input_.mouse_position = {event.button.x, event.button.y};
            if (event.button.button == SDL_BUTTON_LEFT) {
                input_.break_block_pressed = true;
                input_.break_block_held = true;
                input_.left_click_pressed = true;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                input_.place_block_pressed = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
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
    if (window_.handle != nullptr) {
        SDL_DestroyWindow(window_.handle);
        window_.handle = nullptr;
    }
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
}

bool PlatformApp::initialize_audio() {
    const bool loaded_press = load_ui_sound("assets/sound/ui/press.wav", "press", press_sound_);
    const bool loaded_focus = load_ui_sound("assets/sound/ui/focus.wav", "focus", focus_sound_);
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

void PlatformApp::update_relative_mouse_mode() {
    SDL_SetWindowRelativeMouseMode(window_.handle, input_.capture_mouse);
    if (input_.capture_mouse) {
        SDL_HideCursor();
    } else {
        SDL_ShowCursor();
    }
}

}
