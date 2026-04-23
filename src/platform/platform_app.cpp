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
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
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
    input_.capture_mouse = true;
    update_relative_mouse_mode();
    return true;
}

void PlatformApp::pump_events() {
    input_.mouse_delta = {};
    input_.toggle_mouse_pressed = false;
    input_.toggle_wireframe_pressed = false;
    input_.toggle_wireframe_textures_pressed = false;
    input_.toggle_debug_hud_pressed = false;
    input_.toggle_debug_fly_pressed = false;
    input_.jump_pressed = false;
    input_.break_block_pressed = false;
    input_.place_block_pressed = false;
    input_.selected_hotbar_slot = -1;

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
            if (input_.capture_mouse) {
                input_.mouse_delta.x += static_cast<float>(event.motion.xrel);
                input_.mouse_delta.y += static_cast<float>(event.motion.yrel);
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (!event.button.down) {
                break;
            }
            if (event.button.button == SDL_BUTTON_LEFT) {
                input_.break_block_pressed = true;
                input_.break_block_held = true;
            } else if (event.button.button == SDL_BUTTON_RIGHT) {
                input_.place_block_pressed = true;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (event.button.button == SDL_BUTTON_LEFT) {
                input_.break_block_held = false;
            }
            break;
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

void PlatformApp::shutdown() {
    if (window_.handle != nullptr) {
        SDL_DestroyWindow(window_.handle);
        window_.handle = nullptr;
    }
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
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
