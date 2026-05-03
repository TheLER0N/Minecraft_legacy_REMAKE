#include "app/application.hpp"

#include "common/log.hpp"
#include "game/world_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <random>

namespace ml {

namespace {

constexpr bool kUseFixedDebugCamera = true;
constexpr std::size_t kPendingUploadLogLimit = 8;
constexpr std::size_t kPlacementFailureLogLimit = 8;
constexpr float kBlockTargetDistance = 5.0f;
constexpr float kPlayerEyeHeight = 1.62f;
constexpr float kPlacementBodyPadding = 0.02f;
constexpr float kDebugFpsRefreshSeconds = 0.25f;
constexpr float kBreakRepeatInitialDelaySeconds = 0.35f;
constexpr float kBreakRepeatIntervalSeconds = 0.18f;

#ifdef __ANDROID__
constexpr std::size_t kChunkUploadByteBudgetPerFrame = 1024ull * 1024ull;
constexpr std::size_t kChunkUploadBacklogBudgetPerFrame = 1024ull * 1024ull;
constexpr std::size_t kChunkUploadMaxCountPerFrame = 1;
constexpr int kInitialChunkRadius = 4;
#else
constexpr std::size_t kChunkUploadByteBudgetPerFrame = 2ull * 1024ull * 1024ull;
constexpr std::size_t kChunkUploadBacklogBudgetPerFrame = 2ull * 1024ull * 1024ull;
constexpr std::size_t kChunkUploadMaxCountPerFrame = 2;
constexpr int kInitialChunkRadius = 10;
#endif

constexpr int kPlayGameButtonIndex = 0;
constexpr int kExitGameButtonIndex = 5;
constexpr float kMenuExitDelaySeconds = 0.18f;
constexpr float kStartupSplashTotalSeconds = 11.8f;
constexpr float kStartupSplashSkipFadeSeconds = 0.25f;
constexpr int kCaveVisibilityRoofThreshold = 8;
constexpr int kCaveVisibilityHysteresisFrames = 8;

constexpr float kMenuVirtualWidth = 640.0f;
constexpr float kMenuVirtualHeight = 360.0f;
constexpr float kMenuButtonWidth = 224.0f;
constexpr float kMenuButtonHeight = 20.0f;
constexpr float kMenuButtonGap = 5.0f;
constexpr float kMenuFirstButtonTop = 126.0f;

float menu_layout_scale(float width, float height) {
    const float fit_scale = std::min(width / kMenuVirtualWidth, height / kMenuVirtualHeight);
    return std::max(1.0f, fit_scale);
}

const char* leaves_render_mode_name(LeavesRenderMode mode) {
    switch (mode) {
    case LeavesRenderMode::Fast:
        return "FAST";
    case LeavesRenderMode::Fancy:
        return "FANCY";
    default:
        return "UNKNOWN";
    }
}

std::size_t wrap_hotbar_slot(std::size_t current, int delta, std::size_t slot_count) {
    if (slot_count == 0 || delta == 0) {
        return current;
    }

    const int count = static_cast<int>(slot_count);
    int wrapped = static_cast<int>(current) + delta;
    wrapped %= count;
    if (wrapped < 0) {
        wrapped += count;
    }
    return static_cast<std::size_t>(wrapped);
}

bool aabb_intersects_block(const Aabb& box, const Int3& block) {
    const float block_min_x = static_cast<float>(block.x);
    const float block_min_y = static_cast<float>(block.y);
    const float block_min_z = static_cast<float>(block.z);
    const float block_max_x = block_min_x + 1.0f;
    const float block_max_y = block_min_y + 1.0f;
    const float block_max_z = block_min_z + 1.0f;

    return box.min.x < block_max_x && box.max.x > block_min_x &&
        box.min.y < block_max_y && box.max.y > block_min_y &&
        box.min.z < block_max_z && box.max.z > block_min_z;
}

bool has_valid_placement_hit(const BlockHit& hit) {
    const Int3 expected {
        hit.block.x + hit.normal.x,
        hit.block.y + hit.normal.y,
        hit.block.z + hit.normal.z
    };

    return (hit.normal.x != 0 || hit.normal.y != 0 || hit.normal.z != 0) &&
        hit.placement_block == expected;
}

Aabb placement_safety_bounds(const Aabb& box) {
    return {
        {box.min.x + kPlacementBodyPadding, box.min.y, box.min.z + kPlacementBodyPadding},
        {box.max.x - kPlacementBodyPadding, box.max.y, box.max.z - kPlacementBodyPadding}
    };
}

bool break_target_block(WorldStreamer& world, const BlockHit& hit) {
    return world.set_block_at_world(hit.block.x, hit.block.y, hit.block.z, BlockId::Air) == SetBlockResult::Success;
}

const char* set_block_result_message(SetBlockResult result) {
    switch (result) {
    case SetBlockResult::Success:
        return "success";
    case SetBlockResult::ChunkUnloaded:
        return "target chunk unloaded";
    case SetBlockResult::OutOfBounds:
        return "target outside world height";
    case SetBlockResult::Occupied:
        return "target occupied";
    case SetBlockResult::NoChange:
        return "no change";
    case SetBlockResult::IntersectsPlayer:
        return "target intersects player";
    case SetBlockResult::InvalidPlacementHit:
        return "invalid placement hit";
    }
    return "unknown";
}

int floor_div_chunk_coord(int value, int divisor) {
    if (value >= 0) {
        return value / divisor;
    }
    return -(((-value) + divisor - 1) / divisor);
}

std::size_t mesh_byte_count(const ChunkMesh& mesh) {
    const std::size_t vertices = mesh.opaque_mesh.vertices.size() + mesh.cutout_mesh.vertices.size() + mesh.transparent_mesh.vertices.size();
    const std::size_t indices = mesh.opaque_mesh.indices.size() + mesh.cutout_mesh.indices.size() + mesh.transparent_mesh.indices.size();
    return vertices * sizeof(Vertex) + indices * sizeof(std::uint32_t);
}

}

Application::Application() = default;

Application::~Application() {
    if (world_streamer_ != nullptr) {
        world_streamer_->flush_all_dirty_chunks();
    }
    renderer_.shutdown();
    platform_.shutdown();
}

bool Application::initialize() {
    log_message(LogLevel::Info, "Application: initialize platform");
    if (!platform_.initialize()) {
        log_message(LogLevel::Error, "Application: platform initialization failed");
        return false;
    }

    log_message(LogLevel::Info, "Application: initialize renderer");
    if (!renderer_.initialize(platform_.window(), platform_.shader_directory())) {
        log_message(LogLevel::Error, "Renderer initialization failed");
        return false;
    }
    renderer_.debug_log_draw_stats = false;

    std::random_device random_device;
    std::mt19937 rng(random_device());
    std::uniform_int_distribution<int> panorama_roll(1, 100);
    menu_uses_night_panorama_ = panorama_roll(rng) > 90;
    platform_.set_mouse_capture(false);

    if (kUseFixedDebugCamera) {
        camera_.set_pose({32.0f, 110.0f, 80.0f}, -90.0f, -22.0f);
    }
    player_.set_body_position({32.0f, 100.0f, 80.0f});
    player_.set_view_from_forward(camera_.forward());
    platform_.start_menu_music();
    return true;
}

void Application::start_world() {
    if (world_streamer_ == nullptr) {
        world_save_ = std::make_unique<WorldSave>(platform_.save_root_directory() / "default");
        const WorldMetadata metadata = world_save_->load_or_create_metadata();
        world_streamer_ = std::make_unique<WorldStreamer>(metadata.world_seed, block_registry_, kInitialChunkRadius, world_save_.get());
        world_streamer_->set_leaves_render_mode(leaves_render_mode_);
        const WorldGenerator spawn_generator {block_registry_};
        const int spawn_x = 32;
        const int spawn_z = 80;
        const int surface_y = spawn_generator.surface_height_at(spawn_x, spawn_z, metadata.world_seed);
        const float spawn_y = static_cast<float>(std::max(surface_y, kSeaLevel) + 2);
        player_.set_body_position({static_cast<float>(spawn_x), spawn_y, static_cast<float>(spawn_z)});
        camera_.set_pose({static_cast<float>(spawn_x), spawn_y + kPlayerEyeHeight, static_cast<float>(spawn_z)}, -90.0f, -22.0f);
        player_.set_view_from_forward(camera_.forward());
        log_message(LogLevel::Info, "Application: world streamer created seed=" + std::to_string(metadata.world_seed));
    }
    platform_.set_mouse_capture(true);
    platform_.enter_world_music();
    app_state_ = AppState::InWorld;
}

Renderer::CaveVisibilityFrame Application::update_cave_visibility_frame(Vec3 observer_position) {
    const int camera_x = static_cast<int>(std::floor(observer_position.x));
    const int camera_y = static_cast<int>(std::floor(observer_position.y));
    const int camera_z = static_cast<int>(std::floor(observer_position.z));
    constexpr std::array<std::array<int, 2>, 5> offsets {{
        {{0, 0}},
        {{1, 0}},
        {{-1, 0}},
        {{0, 1}},
        {{0, -1}}
    }};

    int best_roof_blocks = std::numeric_limits<int>::max();
    bool sampled_loaded_column = false;
    for (const auto& offset : offsets) {
        int roof_blocks = 0;
        bool blocked = false;
        bool column_loaded = false;
        for (int y = std::clamp(camera_y + 1, kWorldMinY, kWorldMaxY); y <= kWorldMaxY; ++y) {
            const BlockQueryResult query = world_streamer_->query_block_at_world(camera_x + offset[0], y, camera_z + offset[1]);
            if (query.status == BlockQueryStatus::Unloaded) {
                break;
            }
            if (query.status == BlockQueryStatus::Loaded) {
                column_loaded = true;
                if (block_registry_.is_opaque(query.block)) {
                    ++roof_blocks;
                    blocked = true;
                }
            }
        }
        if (!column_loaded) {
            continue;
        }
        sampled_loaded_column = true;
        best_roof_blocks = std::min(best_roof_blocks, blocked ? roof_blocks : 0);
    }

        if (!sampled_loaded_column) {
        return Renderer::CaveVisibilityFrame {
            cave_visibility_cave_mode_,
            floor_div_chunk_coord(camera_x, kChunkWidth),
            floor_div_chunk_coord(camera_z, kChunkDepth),
            camera_y,
            cave_visibility_roof_blocks_
        };
    }
    const bool candidate_cave_mode = best_roof_blocks >= kCaveVisibilityRoofThreshold;
    if (candidate_cave_mode != cave_visibility_pending_mode_) {
        cave_visibility_pending_mode_ = candidate_cave_mode;
        cave_visibility_pending_frames_ = 1;
    } else if (cave_visibility_pending_frames_ < kCaveVisibilityHysteresisFrames) {
        ++cave_visibility_pending_frames_;
    }
    if (cave_visibility_pending_frames_ >= kCaveVisibilityHysteresisFrames) {
        cave_visibility_cave_mode_ = cave_visibility_pending_mode_;
    }
    cave_visibility_roof_blocks_ = best_roof_blocks;

    return Renderer::CaveVisibilityFrame {
        cave_visibility_cave_mode_,
        floor_div_chunk_coord(camera_x, kChunkWidth),
        floor_div_chunk_coord(camera_z, kChunkDepth),
        camera_y,
        cave_visibility_roof_blocks_
    };
}

std::array<Application::MenuButton, 6> Application::menu_buttons() const {
    const PlatformWindow& window = platform_.window();
    const float width = static_cast<float>(window.width);
    const float height = static_cast<float>(window.height == 0 ? 1 : window.height);
    const float scale = menu_layout_scale(width, height);
    const float origin_x = (width - kMenuVirtualWidth * scale) * 0.5f;
    const float origin_y = (height - kMenuVirtualHeight * scale) * 0.5f;
    const float button_width = kMenuButtonWidth * scale;
    const float button_height = kMenuButtonHeight * scale;
    const float gap = kMenuButtonGap * scale;
    const float left = origin_x + (kMenuVirtualWidth - kMenuButtonWidth) * 0.5f * scale;
    const float first_top = origin_y + kMenuFirstButtonTop * scale;

    std::array<MenuButton, 6> buttons {};
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        const float top = first_top + static_cast<float>(i) * (button_height + gap);
        buttons[i] = {left, top, left + button_width, top + button_height};
    }
    return buttons;
}

int Application::hovered_menu_button(const InputState& input) const {
    if (!input.mouse_inside_window) {
        return -1;
    }
    const auto buttons = menu_buttons();
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        const MenuButton& button = buttons[i];
        if (input.mouse_position.x >= button.left && input.mouse_position.x <= button.right &&
            input.mouse_position.y >= button.top && input.mouse_position.y <= button.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int Application::run() {
    std::size_t pending_upload_log_count = 0;
    std::size_t placement_failure_log_count = 0;
    while (!platform_.should_close()) {
        platform_.pump_events();
        const InputState& input = platform_.current_input();
        const float dt = platform_.frame_delta_seconds();
        platform_.update_music(dt);

        if (app_state_ == AppState::StartupSplash) {
            platform_.set_mouse_capture(false);
            startup_splash_seconds_ += dt;
            if (input.jump_pressed) {
                startup_skip_requested_ = true;
            }
            float fade_multiplier = 1.0f;
            if (startup_skip_requested_) {
                startup_skip_fade_seconds_ += dt;
                fade_multiplier = std::max(0.0f, 1.0f - startup_skip_fade_seconds_ / kStartupSplashSkipFadeSeconds);
            }

            if (startup_splash_seconds_ >= kStartupSplashTotalSeconds || startup_skip_fade_seconds_ >= kStartupSplashSkipFadeSeconds) {
                app_state_ = AppState::MainMenu;
                menu_time_seconds_ = 0.0f;
                continue;
            }

            const CameraFrameData splash_camera {
                Mat4::identity(),
                Mat4::identity(),
                Mat4::identity(),
                {},
                {0.0f, 0.0f, -1.0f},
                {1.0f, 0.0f, 0.0f},
                {0.0f, 1.0f, 0.0f}
            };
            renderer_.begin_frame(splash_camera);
            renderer_.draw_startup_splash(startup_splash_seconds_, fade_multiplier);
            renderer_.end_frame();
            continue;
        }

        if (app_state_ == AppState::MainMenu) {
            platform_.set_mouse_capture(false);
            menu_time_seconds_ += dt;
            if (menu_exit_requested_) {
                menu_exit_delay_seconds_ -= dt;
                if (menu_exit_delay_seconds_ <= 0.0f) {
                    return 0;
                }
            }
            const int hovered_button = hovered_menu_button(input);
            const int previous_selected_menu_button = selected_menu_button_;
            if (hovered_button != -1) {
                selected_menu_button_ = hovered_button;
            }
            if (input.menu_up_pressed) {
                selected_menu_button_ = (selected_menu_button_ + 5) % 6;
            }
            if (input.menu_down_pressed) {
                selected_menu_button_ = (selected_menu_button_ + 1) % 6;
            }
            if (selected_menu_button_ != previous_selected_menu_button ||
                (hovered_button != -1 && hovered_button != last_hovered_menu_button_)) {
                platform_.play_ui_focus_sound();
            }
            last_hovered_menu_button_ = hovered_button;
            const int activated_button = input.left_click_pressed && hovered_button != -1
                ? hovered_button
                : (input.menu_confirm_pressed ? selected_menu_button_ : -1);
            if (!menu_exit_requested_ && activated_button != -1) {
                if (activated_button == kPlayGameButtonIndex) {
                    platform_.play_ui_press_sound();
                    start_world();
                } else if (activated_button == kExitGameButtonIndex) {
                    platform_.play_ui_press_sound();
                    menu_exit_requested_ = true;
                    menu_exit_delay_seconds_ = kMenuExitDelaySeconds;
                }
            }

            const PlatformWindow& window = platform_.window();
            const float aspect_ratio = static_cast<float>(window.width) / static_cast<float>(window.height == 0 ? 1 : window.height);
            const CameraFrameData menu_camera {
                Mat4::identity(),
                Mat4::identity(),
                Mat4::identity(),
                {},
                {0.0f, 0.0f, -1.0f},
                {1.0f, 0.0f, 0.0f},
                {0.0f, 1.0f, 0.0f}
            };
            (void)aspect_ratio;
            renderer_.begin_frame(menu_camera);
            renderer_.draw_main_menu(menu_time_seconds_, menu_uses_night_panorama_, selected_menu_button_);
            renderer_.end_frame();
            continue;
        }

        if (input.escape_pressed || input.gamepad_start_pressed) {
            world_streamer_->flush_dirty_chunks(8);
            platform_.set_mouse_capture(false);
            platform_.start_menu_music();
            app_state_ = AppState::MainMenu;
            hovered_block_.reset();
            block_break_.target.reset();
            block_break_.repeat_seconds = 0.0f;
            debug_fly_enabled_ = false;
            continue;
        }
        if (platform_.current_input().toggle_wireframe_pressed) {
            renderer_.toggle_wireframe();
        }
        if (input.toggle_wireframe_textures_pressed && renderer_.wireframe_enabled()) {
            renderer_.toggle_wireframe_textures();
        }
        if (input.toggle_debug_hud_pressed) {
            debug_hud_enabled_ = !debug_hud_enabled_;
        }
        if (input.toggle_leaves_render_mode_pressed) {
            leaves_render_mode_ = leaves_render_mode_ == LeavesRenderMode::Fancy
                ? LeavesRenderMode::Fast
                : LeavesRenderMode::Fancy;
            world_streamer_->set_leaves_render_mode(leaves_render_mode_);
            log_message(
                LogLevel::Info,
                std::string("Application: leaves render mode switched to ") + leaves_render_mode_name(leaves_render_mode_) +
                    " [hotkey=F4]"
                );
        }
        if (input.toggle_section_culling_pressed) {
            renderer_.toggle_section_culling();
        }
        if (input.toggle_occlusion_culling_pressed) {
            renderer_.toggle_occlusion_culling();
        }
        if (input.render_distance_delta != 0) {
            world_streamer_->set_chunk_radius(world_streamer_->chunk_radius() + input.render_distance_delta);
        }
        const bool debug_hud_toggled = input.toggle_debug_hud_pressed || input.toggle_leaves_render_mode_pressed;
        if (input.toggle_debug_fly_pressed) {
            debug_fly_enabled_ = !debug_fly_enabled_;
            if (debug_fly_enabled_) {
                camera_.set_pose(player_.eye_position(), -90.0f, -22.0f);
                camera_.set_view_from_forward(player_.forward());
            } else {
                player_.set_body_position({
                    camera_.position().x,
                    camera_.position().y - kPlayerEyeHeight,
                    camera_.position().z
                });
                player_.set_view_from_forward(camera_.forward());
            }
        }

        if (input.selected_hotbar_slot >= 0 &&
            input.selected_hotbar_slot < static_cast<int>(hotbar_.size())) {
            selected_hotbar_slot_ = static_cast<std::size_t>(input.selected_hotbar_slot);
        }
        if (input.hotbar_scroll_delta != 0) {
            selected_hotbar_slot_ = wrap_hotbar_slot(
                selected_hotbar_slot_,
                -input.hotbar_scroll_delta,
                hotbar_.size()
            );
        }
        renderer_.set_hotbar_state(selected_hotbar_slot_, hotbar_.size());

        if (debug_fly_enabled_) {
            camera_.update(input, platform_.frame_delta_seconds());
        } else {
            player_.update(input, platform_.frame_delta_seconds(), *world_streamer_);
        }

        const Vec3 observer_position = debug_fly_enabled_ ? camera_.position() : player_.position();
        const Vec3 observer_forward = debug_fly_enabled_ ? camera_.forward() : player_.forward();
        world_streamer_->update_observer(observer_position, observer_forward);
        const Renderer::CaveVisibilityFrame cave_visibility_frame = update_cave_visibility_frame(observer_position);
        renderer_.set_cave_visibility_frame(cave_visibility_frame);
        for (const ChunkCoord& coord : world_streamer_->drain_pending_unloads()) {
            renderer_.unload_chunk_mesh(coord);
        }
        world_streamer_->tick_generation_jobs();

        const WorldStreamer::StreamingStats pre_upload_stats = world_streamer_->stats();
        const std::size_t upload_budget = pre_upload_stats.pending_uploads > 4
            ? kChunkUploadBacklogBudgetPerFrame
            : kChunkUploadByteBudgetPerFrame;
        auto pending_uploads = world_streamer_->drain_pending_uploads_by_budget(
            upload_budget,
            kChunkUploadMaxCountPerFrame,
            observer_position,
            observer_forward
        );        if (!pending_uploads.empty() && pending_upload_log_count < kPendingUploadLogLimit) {
            log_message(LogLevel::Info, std::string("Application: pending chunk uploads=") + std::to_string(pending_uploads.size()));
            ++pending_upload_log_count;
        }

        std::size_t uploaded_bytes_this_frame = 0;
        const auto upload_start = std::chrono::steady_clock::now();
        //
        for (PendingChunkUpload& upload : pending_uploads) {
            uploaded_bytes_this_frame += mesh_byte_count(upload.mesh);
            renderer_.upload_chunk_mesh(upload.coord, upload.mesh, upload.visibility);
        }
        world_streamer_->refresh_visible_chunks();
        const auto upload_end = std::chrono::steady_clock::now();
        //
        const float upload_ms = std::chrono::duration<float, std::milli>(upload_end - upload_start).count();

        const Vec3 ray_origin = debug_fly_enabled_ ? camera_.position() : player_.eye_position();
        const Vec3 ray_direction = debug_fly_enabled_ ? camera_.forward() : player_.forward();
        hovered_block_ = world_streamer_->raycast(ray_origin, ray_direction, kBlockTargetDistance);

        if (!input.break_block_held || !hovered_block_.has_value()) {
            block_break_.target.reset();
            block_break_.repeat_seconds = 0.0f;
        } else if (input.break_block_pressed) {
            const BlockHit& hit = *hovered_block_;
            if (break_target_block(*world_streamer_, hit)) {
                hovered_block_.reset();
            }
            block_break_.target = hit.block;
            block_break_.repeat_seconds = kBreakRepeatInitialDelaySeconds;
        } else {
            block_break_.repeat_seconds -= dt;
            if (block_break_.repeat_seconds <= 0.0f) {
                const BlockHit& hit = *hovered_block_;
                if (break_target_block(*world_streamer_, hit)) {
                    hovered_block_.reset();
                }
                block_break_.target = hit.block;
                block_break_.repeat_seconds = kBreakRepeatIntervalSeconds;
            }
        }
        const std::size_t uploads_this_frame = pending_uploads.size();

        if (hovered_block_.has_value() && input.place_block_pressed) {
            const BlockHit& hit = *hovered_block_;
            const Int3 place_block = hit.placement_block;
            const BlockQueryResult query = world_streamer_->query_block_at_world(place_block.x, place_block.y, place_block.z);
            SetBlockResult placement_result = SetBlockResult::Success;

            if (!has_valid_placement_hit(hit)) {
                placement_result = SetBlockResult::InvalidPlacementHit;
            } else if (aabb_intersects_block(placement_safety_bounds(player_.bounds()), place_block)) {
                placement_result = SetBlockResult::IntersectsPlayer;
            } else if (query.status == BlockQueryStatus::Unloaded) {
                placement_result = SetBlockResult::ChunkUnloaded;
            } else if (query.status == BlockQueryStatus::OutOfBounds) {
                placement_result = SetBlockResult::OutOfBounds;
            } else if (!block_registry_.is_replaceable(query.block)) {
                placement_result = SetBlockResult::Occupied;
            } else {
                placement_result = world_streamer_->set_block_at_world(
                    place_block.x,
                    place_block.y,
                    place_block.z,
                    hotbar_[selected_hotbar_slot_]
                );
            }

            if (placement_result != SetBlockResult::Success && placement_failure_log_count < kPlacementFailureLogLimit) {
                log_message(
                    LogLevel::Info,
                    std::string("Application: block placement denied [reason=") + set_block_result_message(placement_result) +
                        ", origin=(" + std::to_string(ray_origin.x) + "," + std::to_string(ray_origin.y) + "," + std::to_string(ray_origin.z) + ")" +
                        ", dir=(" + std::to_string(ray_direction.x) + "," + std::to_string(ray_direction.y) + "," + std::to_string(ray_direction.z) + ")" +
                        ", hit=(" + std::to_string(hit.block.x) + "," + std::to_string(hit.block.y) + "," + std::to_string(hit.block.z) + ")" +
                        ", normal=(" + std::to_string(hit.normal.x) + "," + std::to_string(hit.normal.y) + "," + std::to_string(hit.normal.z) + ")" +
                        ", place=(" + std::to_string(place_block.x) + "," + std::to_string(place_block.y) + "," + std::to_string(place_block.z) + ")" +
                        ", query=" + std::to_string(static_cast<int>(query.block)) + "]"
                );
                ++placement_failure_log_count;
            }
        }

        renderer_.set_target_block(hovered_block_);

        debug_fps_accumulator_ += dt;
        ++debug_fps_frames_;
        bool debug_hud_refresh = false;
        if (debug_fps_accumulator_ >= kDebugFpsRefreshSeconds) {
            debug_fps_ = static_cast<float>(debug_fps_frames_) / debug_fps_accumulator_;
            debug_fps_accumulator_ = 0.0f;
            debug_fps_frames_ = 0;
            debug_hud_refresh = true;
        }
        const Vec3 debug_position = debug_fly_enabled_ ? camera_.position() : player_.position();
        const WorldStreamer::StreamingStats streaming_stats = world_streamer_->stats();
        if (debug_hud_toggled || (debug_hud_enabled_ && debug_hud_refresh)) {
            renderer_.set_debug_hud(
                debug_hud_enabled_,
                Renderer::DebugHudData {
                    debug_fps_,
                    debug_position,
                    debug_fly_enabled_,
                    streaming_stats.visible_chunks,
                    streaming_stats.pending_uploads,
                    uploads_this_frame,
                    streaming_stats.queued_rebuilds,
                    streaming_stats.queued_generates,
                    streaming_stats.queued_decorates,
                    streaming_stats.queued_lights,
                    streaming_stats.queued_meshes,
                    streaming_stats.queued_fast_meshes,
                    streaming_stats.queued_final_meshes,
                    streaming_stats.pending_upload_bytes,
                    uploaded_bytes_this_frame,
                    streaming_stats.stale_results,
                    streaming_stats.stale_uploads,
                    streaming_stats.provisional_uploads,
                    streaming_stats.light_stale_results,
                    streaming_stats.edge_fixups,
                    streaming_stats.dropped_jobs,
                    streaming_stats.dirty_save_chunks,
                    streaming_stats.observer_light_borders_ready,
                    streaming_stats.observer_light_border_status,
                    streaming_stats.last_generate_ms,
                    streaming_stats.last_light_ms,
                    streaming_stats.last_mesh_ms,
                    upload_ms,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    0,
                    cave_visibility_frame.cave_mode,
                    cave_visibility_frame.roof_blocks,
                    0,
                    0,
                    0,
                    0,
                    leaves_render_mode_ == LeavesRenderMode::Fancy
                }
            );
        }

        const PlatformWindow& window = platform_.window();
        const float aspect_ratio = static_cast<float>(window.width) / static_cast<float>(window.height == 0 ? 1 : window.height);
        const CameraFrameData camera_frame = debug_fly_enabled_
            ? camera_.frame_data(aspect_ratio)
            : player_.camera_frame_data(aspect_ratio);
        renderer_.begin_frame(camera_frame);
        renderer_.draw_visible_chunks(world_streamer_->visible_chunks());
        renderer_.end_frame();
    }

    return 0;
}

}
