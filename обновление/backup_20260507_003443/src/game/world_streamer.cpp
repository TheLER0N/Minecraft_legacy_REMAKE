#include "game/world_streamer.hpp"

#include "game/world_runtime_tuning.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ml {

namespace {

constexpr float kRaycastEpsilon = 0.0001f;
constexpr float kRaycastTieEpsilon = 0.00001f;
constexpr int kMinChunkRadius = 2;
#ifdef __ANDROID__
constexpr int kMaxChunkRadius = 6;
#else
constexpr int kMaxChunkRadius = 100;
#endif

std::size_t max_new_chunk_requests_per_frame() {
    return world_runtime_tuning().max_new_chunk_requests_per_frame;
}

std::size_t max_completed_results_per_tick() {
    return world_runtime_tuning().max_completed_results_per_tick;
}

std::size_t max_job_queue_size() {
    return world_runtime_tuning().max_job_queue_size;
}

std::size_t streaming_backlog_requests_per_frame() {
    return world_runtime_tuning().streaming_backlog_requests_per_frame;
}

std::size_t corridor_requests_per_frame() {
    return world_runtime_tuning().corridor_requests_per_frame;
}

std::size_t corridor_uploads_per_frame() {
    return world_runtime_tuning().corridor_uploads_per_frame;
}

std::size_t corridor_upload_byte_budget() {
    return world_runtime_tuning().corridor_upload_byte_budget;
}

int contiguous_generation_ring_window() {
    return std::max(1, world_runtime_tuning().contiguous_generation_ring_window);
}

int keep_radius_extra_chunks() {
    return std::max(0, world_runtime_tuning().keep_radius_extra_chunks);
}

float completed_result_apply_budget_ms() {
    return world_runtime_tuning().completed_result_apply_budget_ms;
}

int target_chunk_radius() {
    return world_runtime_tuning().target_chunk_radius;
}

float streaming_update_distance_blocks() {
    return world_runtime_tuning().streaming_update_distance_blocks;
}

float forward_priority_weight() {
    return world_runtime_tuning().forward_priority_weight;
}

float side_priority_weight() {
    return world_runtime_tuning().side_priority_weight;
}

float back_priority_penalty() {
    return world_runtime_tuning().back_priority_penalty;
}

int min_forward_buffer_chunks() {
    return world_runtime_tuning().min_forward_buffer_chunks;
}

int max_forward_buffer_chunks() {
    return world_runtime_tuning().max_forward_buffer_chunks;
}

int min_forward_width_chunks() {
    return world_runtime_tuning().min_forward_width_chunks;
}

int max_forward_width_chunks() {
    return world_runtime_tuning().max_forward_width_chunks;
}

float forward_buffer_pipeline_seconds() {
    return world_runtime_tuning().forward_buffer_pipeline_seconds;
}

float forward_buffer_safety_blocks() {
    return world_runtime_tuning().forward_buffer_safety_blocks;
}

float fast_flight_speed_threshold() {
    return world_runtime_tuning().fast_flight_speed_threshold;
}

float very_fast_flight_speed_threshold() {
    return world_runtime_tuning().very_fast_flight_speed_threshold;
}

bool adaptive_corridor_streaming_enabled() {
    return world_runtime_tuning().adaptive_corridor_streaming_enabled;
}

float corridor_speed_threshold_blocks_per_second() {
    return world_runtime_tuning().corridor_speed_threshold_blocks_per_second;
}

int corridor_safe_radius_chunks() {
    return std::max(1, world_runtime_tuning().corridor_safe_radius_chunks);
}

int corridor_rear_keep_chunks() {
    return std::max(0, world_runtime_tuning().corridor_rear_keep_chunks);
}

int corridor_inner_half_width_chunks() {
    return std::max(1, world_runtime_tuning().corridor_inner_half_width_chunks);
}

int corridor_outer_half_width_chunks() {
    return std::max(corridor_inner_half_width_chunks(), world_runtime_tuning().corridor_outer_half_width_chunks);
}

bool corridor_mode_for_speed(float speed_blocks_per_second) {
    return adaptive_corridor_streaming_enabled() &&
        speed_blocks_per_second >= corridor_speed_threshold_blocks_per_second();
}

int adaptive_corridor_forward_chunks(float speed_blocks_per_second, int chunk_radius) {
    const float chunks_per_second = speed_blocks_per_second / static_cast<float>(kChunkWidth);
    const int calculated = static_cast<int>(
        std::ceil(chunks_per_second * world_runtime_tuning().corridor_lookahead_seconds + 2.0f)
    );

    return std::clamp(
        calculated,
        std::max(1, world_runtime_tuning().corridor_min_forward_chunks),
        std::min(chunk_radius, world_runtime_tuning().corridor_max_forward_chunks)
    );
}

int adaptive_forward_buffer_chunks(float speed_blocks_per_second) {
    const float needed_blocks =
        speed_blocks_per_second * forward_buffer_pipeline_seconds() +
        forward_buffer_safety_blocks();

    const int calculated = static_cast<int>(std::ceil(needed_blocks / static_cast<float>(kChunkWidth)));
    return std::clamp(calculated, min_forward_buffer_chunks(), max_forward_buffer_chunks());
}

int adaptive_forward_width_chunks(float speed_blocks_per_second) {
    int width = min_forward_width_chunks();

    if (speed_blocks_per_second >= very_fast_flight_speed_threshold()) {
        width = max_forward_width_chunks();
    } else if (speed_blocks_per_second >= fast_flight_speed_threshold()) {
        width = std::min(max_forward_width_chunks(), min_forward_width_chunks() + 2);
    }

    if ((width % 2) == 0) {
        ++width;
    }

    return std::clamp(width, min_forward_width_chunks(), max_forward_width_chunks());
}

Vec3 normalized_horizontal_direction(Vec3 direction) {
    direction.y = 0.0f;
    if (length(direction) <= 0.00001f) {
        return {0.0f, 0.0f, -1.0f};
    }
    return normalize(direction);
}

float chunk_forward_units(ChunkCoord origin, ChunkCoord coord, Vec3 direction) {
    const float dx = static_cast<float>(coord.x - origin.x);
    const float dz = static_cast<float>(coord.z - origin.z);
    return dx * direction.x + dz * direction.z;
}

float chunk_side_units(ChunkCoord origin, ChunkCoord coord, Vec3 direction) {
    const float dx = static_cast<float>(coord.x - origin.x);
    const float dz = static_cast<float>(coord.z - origin.z);
    return std::abs(dx * -direction.z + dz * direction.x);
}

bool load_area_chunk(ChunkCoord origin, ChunkCoord coord, int chunk_radius) {
    const int dx = coord.x - origin.x;
    const int dz = coord.z - origin.z;
    return std::max(std::abs(dx), std::abs(dz)) <= chunk_radius;
}

bool keep_area_chunk(ChunkCoord origin, ChunkCoord coord, int chunk_radius) {
    const int dx = coord.x - origin.x;
    const int dz = coord.z - origin.z;
    const int keep_radius = std::min(kMaxChunkRadius, chunk_radius + keep_radius_extra_chunks());
    return std::max(std::abs(dx), std::abs(dz)) <= keep_radius;
}

// Compatibility helper. This is NOT a forward-corridor world mask.
// Corridor logic must affect priority only.
bool corridor_candidate_chunk(
    ChunkCoord origin,
    ChunkCoord coord,
    Vec3 direction,
    float speed_blocks_per_second,
    int chunk_radius
) {
    (void)direction;
    (void)speed_blocks_per_second;
    return load_area_chunk(origin, coord, chunk_radius);
}

float corridor_priority_score(
    ChunkCoord origin,
    ChunkCoord coord,
    Vec3 direction,
    float speed_blocks_per_second
) {
    const int dx = coord.x - origin.x;
    const int dz = coord.z - origin.z;
    const int chebyshev = std::max(std::abs(dx), std::abs(dz));
    const int dist_sq = dx * dx + dz * dz;

    if (chebyshev == 0) {
        return 0.0f;
    }

    if (chebyshev <= corridor_safe_radius_chunks()) {
        return 5.0f + static_cast<float>(dist_sq);
    }

    const float forward = chunk_forward_units(origin, coord, direction);
    const float side = chunk_side_units(origin, coord, direction);

    if (!corridor_mode_for_speed(speed_blocks_per_second)) {
        const float behind = std::max(0.0f, -forward) * 8.0f;
        const float look_bonus = std::max(0.0f, forward) * -1.5f;
        return 80.0f + static_cast<float>(dist_sq) * 0.75f + side * 4.0f + behind + look_bonus;
    }

    const int forward_chunks = adaptive_corridor_forward_chunks(speed_blocks_per_second, world_runtime_tuning().target_chunk_radius);
    const float clamped_forward = std::clamp(
        forward,
        0.0f,
        static_cast<float>(forward_chunks)
    );

    const float behind_penalty = forward < -static_cast<float>(corridor_rear_keep_chunks())
        ? 650.0f + std::abs(forward) * 45.0f
        : std::max(0.0f, -forward) * 18.0f;

    return 100.0f +
        static_cast<float>(dist_sq) * 0.45f +
        side * 22.0f +
        behind_penalty -
        clamped_forward * 10.0f;
}

constexpr std::size_t kMaxDirtyChunkSavesPerTick = 1;constexpr std::size_t kMaxDirtyChunkSavesPerTick = 1;


#ifdef __ANDROID__
constexpr std::uint64_t kGrassUpdateIntervalFrames = 30;
constexpr std::size_t kGrassUpdateChunksPerTick = 2;
constexpr int kGrassUpdateColumnAttemptsPerChunk = 18;
#else
constexpr std::uint64_t kGrassUpdateIntervalFrames = 20;
constexpr std::size_t kGrassUpdateChunksPerTick = 4;
constexpr int kGrassUpdateColumnAttemptsPerChunk = 64;
#endif
constexpr int kGrassBlockedCheckHeight = 1;
constexpr std::uint64_t kGrassCoveredDecayDelayFrames = 35;
constexpr std::size_t kMaxPendingGrassUpdatesPerTick = 16;



bool nearly_equal(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= kRaycastTieEpsilon;
}

std::size_t mesh_vertex_count(const ChunkMesh& mesh) {
    return mesh.opaque_mesh.vertices.size() + mesh.cutout_mesh.vertices.size() + mesh.transparent_mesh.vertices.size();
}

std::size_t mesh_index_count(const ChunkMesh& mesh) {
    return mesh.opaque_mesh.indices.size() + mesh.cutout_mesh.indices.size() + mesh.transparent_mesh.indices.size();
}

std::size_t mesh_byte_count(const ChunkMesh& mesh) {
    return mesh_vertex_count(mesh) * sizeof(Vertex) + mesh_index_count(mesh) * sizeof(std::uint32_t);
}

ChunkVisibilityMetadata build_visibility_metadata(const ChunkData& chunk, const BlockRegistry& block_registry) {
    ChunkVisibilityMetadata metadata {};
    std::array<int, static_cast<std::size_t>(kChunkWidth * kChunkDepth)> surface_y {};
    std::array<int, static_cast<std::size_t>(kChunkWidth * kChunkDepth)> roof_y {};
    surface_y.fill(kWorldMinY);
    roof_y.fill(kWorldMaxY + 1);

    const auto column_index = [](int x, int z) {
        return static_cast<std::size_t>(x + z * kChunkWidth);
    };

    for (int z = 0; z < kChunkDepth; ++z) {
        for (int x = 0; x < kChunkWidth; ++x) {
            bool sky_open = true;
            for (int local_y = kChunkHeight - 1; local_y >= 0; --local_y) {
                const BlockId block = chunk.get(x, local_y, z);
                const int world_y = local_y_to_world_y(local_y);
                if (block_registry.is_renderable(block) && surface_y[column_index(x, z)] == kWorldMinY) {
                    surface_y[column_index(x, z)] = world_y;
                }
                if (sky_open && block_registry.is_opaque(block)) {
                    roof_y[column_index(x, z)] = world_y;
                    sky_open = false;
                }
            }
        }
    }

    for (int section_index = 0; section_index < kChunkSectionCount; ++section_index) {
        ChunkSectionVisibility& section = metadata.sections[static_cast<std::size_t>(section_index)];
        section.min_world_y = kWorldMinY + section_index * kChunkSectionHeight;
        section.max_world_y = std::min(kWorldMaxY, section.min_world_y + kChunkSectionHeight - 1);
        section.nearest_surface_y = kWorldMinY;
        int surface_blocks = 0;
        int cave_blocks = 0;

        for (int z = 0; z < kChunkDepth; ++z) {
            for (int x = 0; x < kChunkWidth; ++x) {
                const int column_surface = surface_y[column_index(x, z)];
                section.nearest_surface_y = std::max(section.nearest_surface_y, column_surface);
                const int column_roof = roof_y[column_index(x, z)];
                for (int world_y = section.min_world_y; world_y <= section.max_world_y; ++world_y) {
                    const int local_y = world_y_to_local_y(world_y);
                    const BlockId block = chunk.get(x, local_y, z);
                    if (!block_registry.is_renderable(block)) {
                        continue;
                    }
                    section.has_geometry = true;
                    const bool sky_access = column_roof == kWorldMaxY + 1 || world_y >= column_roof;
                    section.has_sky_access = section.has_sky_access || sky_access;
                    const bool near_surface = world_y >= column_surface - 8;
                    if (sky_access || near_surface || block == BlockId::OakLog || block == BlockId::OakLeaves) {
                        ++surface_blocks;
                    }
                    if (!sky_access && world_y < column_surface - 8) {
                        ++cave_blocks;
                    }
                }
            }
        }
        const int mixed_threshold = 12;
        section.has_surface_geometry = surface_blocks > 0;
        section.has_cave_geometry = cave_blocks > 0;
        if (surface_blocks > 0 && cave_blocks > 0 && std::min(surface_blocks, cave_blocks) < mixed_threshold) {
            if (surface_blocks > cave_blocks) {
                section.has_cave_geometry = false;
            } else {
                section.has_surface_geometry = false;
            }
        }
        section.solid_roof_above = section.nearest_surface_y - section.max_world_y >= 8;
        if (section.has_geometry && !section.has_surface_geometry && !section.has_cave_geometry) {
            section.has_cave_geometry = !section.has_sky_access;
            section.has_surface_geometry = section.has_sky_access;
        }
    }

    return metadata;
}

}

WorldStreamer::WorldStreamer(WorldSeed seed, const BlockRegistry& block_registry, int chunk_radius)
    : WorldStreamer(seed, block_registry, chunk_radius, nullptr) {
}
// тут
WorldStreamer::WorldStreamer(WorldSeed seed, const BlockRegistry& block_registry, int chunk_radius, WorldSave* world_save)
    : seed_(seed)
    , world_save_(world_save)
    , block_registry_(block_registry)
    , generator_(block_registry)
    , chunk_radius_(std::clamp(std::max(chunk_radius, target_chunk_radius()), kMinChunkRadius, kMaxChunkRadius)) {
    const std::size_t worker_count = world_runtime_tuning().worker_count;

    log_message(
        LogLevel::Info,
        std::string("WorldStreamer: worker threads=") + std::to_string(worker_count)
    );

    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }


}

WorldStreamer::~WorldStreamer() {
    flush_all_dirty_chunks();
    {
        std::lock_guard lock(mutex_);
        stop_requested_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void WorldStreamer::update_observer(Vec3 position) {
    update_observer(position, observer_forward_, 0.0f);
}

void WorldStreamer::update_observer(Vec3 position, Vec3 forward) {
    update_observer(position, forward, 0.0f);
}

void WorldStreamer::update_observer(Vec3 position, Vec3 forward, float dt_seconds) {
    const ChunkCoord origin = world_to_chunk(position);
    ++frame_counter_;
    observer_position_ = position;

    Vec3 camera_forward = normalized_horizontal_direction({forward.x, 0.0f, forward.z});
    Vec3 motion_direction = camera_forward;

    if (has_previous_observer_position_ && dt_seconds > 0.0001f) {
        const float dx = position.x - previous_observer_position_.x;
        const float dz = position.z - previous_observer_position_.z;
        const float instant_speed = std::sqrt(dx * dx + dz * dz) / dt_seconds;

        observer_speed_blocks_per_second_ =
            observer_speed_blocks_per_second_ * 0.85f +
            instant_speed * 0.15f;

        if (std::sqrt(dx * dx + dz * dz) > 0.001f) {
            motion_direction = normalized_horizontal_direction({dx, 0.0f, dz});
        }
    }

    const bool corridor_mode = corridor_mode_for_speed(observer_speed_blocks_per_second_);
    Vec3 stream_direction = camera_forward;
    if (corridor_mode) {
        stream_direction = normalized_horizontal_direction({
            motion_direction.x * world_runtime_tuning().corridor_velocity_weight +
                camera_forward.x * world_runtime_tuning().corridor_look_weight,
            0.0f,
            motion_direction.z * world_runtime_tuning().corridor_velocity_weight +
                camera_forward.z * world_runtime_tuning().corridor_look_weight
        });
    }

    observer_forward_ = stream_direction;
    previous_observer_position_ = position;
    has_previous_observer_position_ = true;
    observer_chunk_ = origin;

    const float streaming_distance = streaming_update_distance_blocks();
    const float move_dx = position.x - last_streaming_update_position_.x;
    const float move_dz = position.z - last_streaming_update_position_.z;
    const float move_distance_sq = move_dx * move_dx + move_dz * move_dz;
    const float required_distance_sq = streaming_distance * streaming_distance;

    const bool changed_chunk =
        !has_streaming_update_position_ ||
        streaming_backlog_origin_.x != origin.x ||
        streaming_backlog_origin_.z != origin.z;

    const bool should_rebuild_backlog =
        !has_streaming_update_position_ ||
        move_distance_sq >= required_distance_sq ||
        streaming_backlog_.empty() ||
        changed_chunk;

    if (should_rebuild_backlog) {
        last_streaming_update_position_ = position;
        has_streaming_update_position_ = true;
        streaming_backlog_origin_ = origin;
        streaming_backlog_.clear();
        streaming_backlog_cursor_ = 0;
        streaming_backlog_.reserve(static_cast<std::size_t>((chunk_radius_ * 2 + 1) * (chunk_radius_ * 2 + 1)));

        for (int dz = -chunk_radius_; dz <= chunk_radius_; ++dz) {
            for (int dx = -chunk_radius_; dx <= chunk_radius_; ++dx) {
                const ChunkCoord coord {origin.x + dx, origin.z + dz};

                if (!load_area_chunk(origin, coord, chunk_radius_)) {
                    continue;
                }

                if (auto it = chunks_.find(coord); it != chunks_.end()) {
                    it->second.last_touched_frame = frame_counter_;
                    continue;
                }

                streaming_backlog_.push_back(coord);
            }
        }

        std::sort(
            streaming_backlog_.begin(),
            streaming_backlog_.end(),
            [&](const ChunkCoord& lhs, const ChunkCoord& rhs) {
                const float lhs_score = corridor_priority_score(
                    origin,
                    lhs,
                    stream_direction,
                    observer_speed_blocks_per_second_
                );
                const float rhs_score = corridor_priority_score(
                    origin,
                    rhs,
                    stream_direction,
                    observer_speed_blocks_per_second_
                );

                if (lhs_score != rhs_score) {
                    return lhs_score < rhs_score;
                }

                if (lhs.x != rhs.x) {
                    return lhs.x < rhs.x;
                }

                return lhs.z < rhs.z;
            }
        );
    } else {
        for (auto& [coord, record] : chunks_) {
            if (keep_area_chunk(origin, coord, chunk_radius_)) {
                record.last_touched_frame = frame_counter_;
            }
        }
    }

    const std::size_t request_budget = corridor_mode
        ? std::min(corridor_requests_per_frame(), max_new_chunk_requests_per_frame())
        : std::min(max_new_chunk_requests_per_frame(), streaming_backlog_requests_per_frame());

    std::size_t requested_this_frame = 0;

    while (streaming_backlog_cursor_ < streaming_backlog_.size() &&
           requested_this_frame < request_budget) {
        const ChunkCoord coord = streaming_backlog_[streaming_backlog_cursor_++];

        if (chunks_.find(coord) != chunks_.end()) {
            continue;
        }

        if (!load_area_chunk(origin, coord, chunk_radius_)) {
            continue;
        }

        {
            std::lock_guard lock(mutex_);
            if (job_queue_.size() >= max_job_queue_size()) {
                if (streaming_backlog_cursor_ > 0) {
                    --streaming_backlog_cursor_;
                }
                break;
            }
        }

        ChunkRecord record {};
        record.generation_version = next_chunk_version_++;
        record.mesh_version = 0;
        record.last_touched_frame = frame_counter_;
        const std::uint64_t version = record.generation_version;

        chunks_.emplace(coord, std::move(record));
        queue_generate_job(coord, version);
        ++requested_this_frame;
    }

    if (streaming_backlog_cursor_ >= streaming_backlog_.size()) {
        streaming_backlog_.clear();
        streaming_backlog_cursor_ = 0;
    }

    if (requested_this_frame > 0 || should_rebuild_backlog) {
        std::lock_guard lock(mutex_);
        std::stable_sort(
            job_queue_.begin(),
            job_queue_.end(),
            [&](const ChunkJob& lhs, const ChunkJob& rhs) {
                const float lhs_score = corridor_priority_score(
                    origin,
                    lhs.coord,
                    stream_direction,
                    observer_speed_blocks_per_second_
                );
                const float rhs_score = corridor_priority_score(
                    origin,
                    rhs.coord,
                    stream_direction,
                    observer_speed_blocks_per_second_
                );

                if (lhs_score != rhs_score) {
                    return lhs_score < rhs_score;
                }

                if (lhs.type != rhs.type) {
                    return static_cast<int>(lhs.type) < static_cast<int>(rhs.type);
                }

                if (lhs.coord.x != rhs.coord.x) {
                    return lhs.coord.x < rhs.coord.x;
                }

                return lhs.coord.z < rhs.coord.z;
            }
        );
    }

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (!keep_area_chunk(origin, it->first, chunk_radius_)) {
            const ChunkCoord unloaded_coord = it->first;
            if (world_save_ != nullptr && it->second.dirty_save && it->second.data.has_value()) {
                world_save_->save_chunk(unloaded_coord, *it->second.data);
            }
            {
                std::lock_guard lock(mutex_);
                const std::size_t before = job_queue_.size();
                std::erase_if(job_queue_, [&](const ChunkJob& job) {
                    return job.coord == unloaded_coord;
                });
                queued_light_jobs_.erase(unloaded_coord);
                dropped_jobs_ += before - job_queue_.size();
            }
            pending_unloads_.push_back(unloaded_coord);
            rebuild_states_.erase(unloaded_coord);
            dirty_save_set_.erase(unloaded_coord);
            if (logged_rebuild_lifecycle_count_ < 16) {
                log_message(
                    LogLevel::Info,
                    std::string("WorldStreamer: chunk unloaded coord=(") +
                        std::to_string(unloaded_coord.x) + "," + std::to_string(unloaded_coord.z) + ")"
                );
                ++logged_rebuild_lifecycle_count_;
            }
            it = chunks_.erase(it);
        } else {
            ++it;
        }
    }

    refresh_visible_chunks();
    flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
}
int WorldStreamer::continuous_uploaded_radius(Vec3 position, int max_radius) const {
    const ChunkCoord center = world_to_chunk(position);
    const int clamped_radius = std::clamp(max_radius, 0, chunk_radius_);

    int continuous_radius = -1;
    for (int ring = 0; ring <= clamped_radius; ++ring) {
        bool ring_ready = true;

        for (int dz = -ring; dz <= ring && ring_ready; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue;
                }

                const ChunkCoord coord {center.x + dx, center.z + dz};
                const auto it = chunks_.find(coord);
                if (it == chunks_.end() || !it->second.uploaded_to_gpu) {
                    ring_ready = false;
                    break;
                }
            }
        }

        if (!ring_ready) {
            break;
        }

        continuous_radius = ring;
    }

    return continuous_radius;
}

bool WorldStreamer::all_chunks_uploaded_in_radius(Vec3 position, int radius) const {
    return continuous_uploaded_radius(position, radius) >= radius;
}
void WorldStreamer::request_spawn_preload(Vec3 position, int radius, std::size_t max_requests) {
    if (max_requests == 0) {
        return;
    }

    const ChunkCoord center = world_to_chunk(position);
    ++frame_counter_;
    observer_position_ = position;
    observer_chunk_ = center;
    last_streaming_update_position_ = position;
    has_streaming_update_position_ = true;
    previous_observer_position_ = position;
    has_previous_observer_position_ = true;
    observer_speed_blocks_per_second_ = 0.0f;
    streaming_backlog_.clear();
    streaming_backlog_cursor_ = 0;
    streaming_backlog_origin_ = center;

    const int preload_radius = std::clamp(radius, 0, chunk_radius_);

    int first_incomplete_ring = -1;
    for (int ring = 0; ring <= preload_radius; ++ring) {
        bool ring_ready = true;

        for (int dz = -ring; dz <= ring && ring_ready; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue;
                }

                const ChunkCoord coord {center.x + dx, center.z + dz};
                const auto it = chunks_.find(coord);
                if (it == chunks_.end() || !it->second.uploaded_to_gpu) {
                    ring_ready = false;
                    break;
                }
            }
        }

        if (!ring_ready) {
            first_incomplete_ring = ring;
            break;
        }
    }

    if (first_incomplete_ring < 0) {
        refresh_visible_chunks();
        flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
        return;
    }

    const int max_schedule_ring = std::min(
        preload_radius,
        first_incomplete_ring + contiguous_generation_ring_window() - 1
    );

    std::vector<ChunkCoord> ordered_chunks;
    for (int ring = first_incomplete_ring; ring <= max_schedule_ring; ++ring) {
        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue;
                }

                const ChunkCoord coord {center.x + dx, center.z + dz};
                auto it = chunks_.find(coord);
                if (it != chunks_.end()) {
                    it->second.last_touched_frame = frame_counter_;
                    continue;
                }

                ordered_chunks.push_back(coord);
            }
        }
    }

    std::stable_sort(
        ordered_chunks.begin(),
        ordered_chunks.end(),
        [&](const ChunkCoord& lhs, const ChunkCoord& rhs) {
            const int lhs_dx = lhs.x - center.x;
            const int lhs_dz = lhs.z - center.z;
            const int rhs_dx = rhs.x - center.x;
            const int rhs_dz = rhs.z - center.z;

            const int lhs_ring = std::max(std::abs(lhs_dx), std::abs(lhs_dz));
            const int rhs_ring = std::max(std::abs(rhs_dx), std::abs(rhs_dz));
            if (lhs_ring != rhs_ring) {
                return lhs_ring < rhs_ring;
            }

            const int lhs_dist = lhs_dx * lhs_dx + lhs_dz * lhs_dz;
            const int rhs_dist = rhs_dx * rhs_dx + rhs_dz * rhs_dz;
            if (lhs_dist != rhs_dist) {
                return lhs_dist < rhs_dist;
            }

            if (lhs.x != rhs.x) {
                return lhs.x < rhs.x;
            }

            return lhs.z < rhs.z;
        }
    );

    std::size_t requested = 0;
    for (const ChunkCoord& coord : ordered_chunks) {
        if (requested >= max_requests) {
            break;
        }

        {
            std::lock_guard lock(mutex_);
            if (job_queue_.size() >= max_job_queue_size()) {
                break;
            }
        }

        ChunkRecord record {};
        record.generation_version = next_chunk_version_++;
        record.mesh_version = 0;
        record.last_touched_frame = frame_counter_;
        const std::uint64_t version = record.generation_version;

        chunks_.emplace(coord, std::move(record));
        queue_generate_job(coord, version);
        ++requested;
    }

    if (requested > 0) {
        std::lock_guard lock(mutex_);
        std::stable_sort(
            job_queue_.begin(),
            job_queue_.end(),
            [&](const ChunkJob& lhs, const ChunkJob& rhs) {
                const int lhs_dx = lhs.coord.x - center.x;
                const int lhs_dz = lhs.coord.z - center.z;
                const int rhs_dx = rhs.coord.x - center.x;
                const int rhs_dz = rhs.coord.z - center.z;

                const int lhs_ring = std::max(std::abs(lhs_dx), std::abs(lhs_dz));
                const int rhs_ring = std::max(std::abs(rhs_dx), std::abs(rhs_dz));

                if (lhs_ring != rhs_ring) {
                    return lhs_ring < rhs_ring;
                }

                const int lhs_dist = lhs_dx * lhs_dx + lhs_dz * lhs_dz;
                const int rhs_dist = rhs_dx * rhs_dx + rhs_dz * rhs_dz;

                if (lhs_dist != rhs_dist) {
                    return lhs_dist < rhs_dist;
                }

                if (lhs.type != rhs.type) {
                    return static_cast<int>(lhs.type) < static_cast<int>(rhs.type);
                }

                if (lhs.coord.x != rhs.coord.x) {
                    return lhs.coord.x < rhs.coord.x;
                }

                return lhs.coord.z < rhs.coord.z;
            }
        );
    }

    refresh_visible_chunks();
    flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
}
void WorldStreamer::tick_generation_jobs() {
    std::lock_guard lock(mutex_);
    std::size_t processed = 0;
    const auto apply_start = std::chrono::steady_clock::now();
    while (!completed_.empty() && processed < max_completed_results_per_tick()) {
        if (processed > 0) {
            const float apply_ms = std::chrono::duration<float, std::milli>(
                std::chrono::steady_clock::now() - apply_start
            ).count();

            if (apply_ms >= completed_result_apply_budget_ms()) {
                break;
            }
        }

        JobResult result = std::move(completed_.front());
        completed_.pop();
        ++processed;

        if (result.type == ChunkJobType::CalculateLight) {
            queued_light_jobs_.erase(result.coord);
        }

        auto it = chunks_.find(result.coord);
        if (it == chunks_.end() || !desired_chunk(observer_chunk_, result.coord)) {
            ++stale_results_;
            continue;
        }

        if (result.type == ChunkJobType::CalculateLight) {
            ChunkRecord& record = it->second;
            if (!result.light.has_value()) {
                ++light_stale_results_;
                if (record.dirty_light || record.needs_final_light) {
                    queue_light_job_if_loaded_locked(result.coord);
                }
                continue;
            }
            if (record.generation_version != result.version || record.latest_light_job_token != result.light_job_token) {
                ++light_stale_results_;
                if (record.dirty_light || record.needs_final_light) {
                    queue_light_job_if_loaded_locked(result.coord);
                }
                continue;
            }

            last_light_ms_ = result.light_ms;
            const bool had_ready = record.light.has_value() && record.light->borders_ready;
            const std::uint64_t old_signature = record.border_signature;

            record.state = result.provisional ? ChunkState::WaitingForNeighbors : ChunkState::LightReady;
            record.light = std::move(result.light);
            record.border_signature = result.border_signature;
            record.dirty_light = result.provisional;
            record.needs_final_light = result.provisional;
            record.dirty_mesh = true;

            const bool boundaries_changed =
                (!had_ready && result.borders_ready) ||
                old_signature != result.border_signature;

            queue_rebuild_job_if_loaded_locked(result.coord);
            if (boundaries_changed) {
                queue_rebuild_self_and_neighbors_if_loaded_locked(result.coord, true);
            }
            continue;
        }

        if (result.type == ChunkJobType::BuildMesh && it->second.generation_version != result.version) {
            ++stale_results_;
            if (auto state_it = rebuild_states_.find(result.coord); state_it != rebuild_states_.end()) {
                state_it->second.queued = false;
            }
            queue_rebuild_job_if_loaded_locked(result.coord);
            continue;
        }

        if (it->second.generation_version != result.version) {
            ++stale_results_;
            continue;
        }

        if (result.type == ChunkJobType::BuildMesh) {
            if (result.stale_rebuild) {
                auto state_it = rebuild_states_.find(result.coord);
                if (state_it != rebuild_states_.end()) {
                    state_it->second.queued = false;
                    state_it->second.dirty = false;
                }
                if (logged_rebuild_lifecycle_count_ < 16) {
                    log_message(
                        LogLevel::Info,
                        std::string("WorldStreamer: stale rebuild skipped coord=(") +
                            std::to_string(result.coord.x) + "," + std::to_string(result.coord.z) + ")"
                    );
                    ++logged_rebuild_lifecycle_count_;
                }
                queue_rebuild_job_if_loaded_locked(result.coord);
                continue;
            }
            if (result.rebuild_serial != 0) {
                auto state_it = rebuild_states_.find(result.coord);
                if (state_it != rebuild_states_.end() && state_it->second.serial != result.rebuild_serial) {
                    ++stale_results_;
                    state_it->second.queued = false;
                    queue_rebuild_job_if_loaded_locked(result.coord);
                    continue;
                }
            }
            auto state_it = rebuild_states_.find(result.coord);
            if (state_it != rebuild_states_.end() && state_it->second.dirty) {
                state_it->second.queued = false;
                state_it->second.dirty = false;
                if (logged_rebuild_lifecycle_count_ < 16) {
                    log_message(
                        LogLevel::Info,
                        std::string("WorldStreamer: stale rebuild discarded coord=(") +
                            std::to_string(result.coord.x) + "," + std::to_string(result.coord.z) + ")"
                    );
                    ++logged_rebuild_lifecycle_count_;
                }
                queue_rebuild_job_if_loaded_locked(result.coord);
                continue;
            }
            rebuild_states_.erase(result.coord);
            last_mesh_ms_ = result.mesh_ms;
            it->second.state = ChunkState::UploadQueued;
            it->second.dirty_mesh = false;
            if (it->second.provisional_mesh && !result.provisional) {
                ++edge_fixups_;
            }

            if (result.mesh.empty()) {
                log_message(LogLevel::Warning, "WorldStreamer: rebuilt chunk mesh is empty");
            } else if (logged_ready_chunk_count_ < 8) {
                log_message(
                    LogLevel::Info,
                    std::string("WorldStreamer: chunk ready vertices=") + std::to_string(mesh_vertex_count(result.mesh)) +
                        " indices=" + std::to_string(mesh_index_count(result.mesh)) +
                        " generate_ms=" + std::to_string(result.generate_ms) +
                        " mesh_ms=" + std::to_string(result.mesh_ms)
                );
                ++logged_ready_chunk_count_;
            }
            const ChunkVisibilityMetadata visibility = it->second.data.has_value()
                ? build_visibility_metadata(*it->second.data, block_registry_)
                : ChunkVisibilityMetadata {};
            std::size_t replaced_stale_uploads = 0;
            const auto old_end = std::remove_if(
                pending_uploads_.begin(),
                pending_uploads_.end(),
                [&](const PendingChunkUpload& upload) {
                    if (upload.coord == result.coord && (!upload.provisional || result.provisional)) {
                        ++replaced_stale_uploads;
                    }
                    return upload.coord == result.coord;
                }
            );
            for (std::size_t i = 0; i < replaced_stale_uploads; ++i) {
                record_stale_upload_drop(result.coord);
            }
            pending_uploads_.erase(old_end, pending_uploads_.end());
            it->second.provisional_mesh = result.provisional;
            const std::uint64_t upload_token = next_upload_token_++;
            it->second.latest_upload_token = upload_token;
            it->second.mesh_version = result.version;
            pending_uploads_.push_back({
                result.coord,
                result.version,
                result.rebuild_serial,
                upload_token,
                result.provisional,
                std::move(result.mesh),
                visibility
            });
            continue;
        }

        if (!result.chunk_data.has_value()) {
            ++stale_results_;
            continue;
        }

        if (result.type == ChunkJobType::GenerateTerrain) {
            last_generate_ms_ = result.generate_ms;
            it->second.state = ChunkState::TerrainGenerated;
            it->second.data = *result.chunk_data;
            queue_stage_job_locked(result.coord, result.version, ChunkJobType::Decorate, std::move(result.chunk_data));
            continue;
        }

        if (result.type == ChunkJobType::Decorate) {
            it->second.state = ChunkState::Decorated;
            it->second.data = *result.chunk_data;
            it->second.dirty_light = true;
            it->second.needs_final_light = true;
            queue_light_self_and_neighbors_if_loaded_locked(result.coord, true);
            continue;
        }

    }

    tick_grass_updates_locked();

}

std::span<const ActiveChunk> WorldStreamer::visible_chunks() const {
    return visible_chunks_;
}


void WorldStreamer::refresh_visible_chunks() {
    visible_chunks_.clear();

    const auto center_it = chunks_.find(observer_chunk_);
    if (center_it == chunks_.end() || !center_it->second.uploaded_to_gpu) {
        return;
    }

    const Vec3 stream_direction = normalized_horizontal_direction(observer_forward_);

    std::vector<ChunkCoord> open;
    std::vector<ChunkCoord> visited;
    open.reserve(static_cast<std::size_t>((chunk_radius_ * 2 + 1) * (chunk_radius_ * 2 + 1)));
    visited.reserve(static_cast<std::size_t>((chunk_radius_ * 2 + 1) * (chunk_radius_ * 2 + 1)));

    auto contains_coord = [](const std::vector<ChunkCoord>& list, ChunkCoord coord) {
        return std::find(list.begin(), list.end(), coord) != list.end();
    };

    open.push_back(observer_chunk_);
    visited.push_back(observer_chunk_);

    constexpr std::array<std::array<int, 2>, 4> neighbors {{
        {{ 1,  0}},
        {{-1,  0}},
        {{ 0,  1}},
        {{ 0, -1}}
    }};

    std::size_t cursor = 0;
    while (cursor < open.size()) {
        const ChunkCoord current = open[cursor++];

        const auto record_it = chunks_.find(current);
        if (record_it == chunks_.end() ||
            !record_it->second.uploaded_to_gpu ||
            !load_area_chunk(observer_chunk_, current, chunk_radius_)) {
            continue;
        }

        visible_chunks_.push_back({current});

        for (const auto& offset : neighbors) {
            const ChunkCoord next {current.x + offset[0], current.z + offset[1]};
            if (contains_coord(visited, next)) {
                continue;
            }

            if (!load_area_chunk(observer_chunk_, next, chunk_radius_)) {
                continue;
            }

            const auto next_it = chunks_.find(next);
            if (next_it == chunks_.end() || !next_it->second.uploaded_to_gpu) {
                continue;
            }

            visited.push_back(next);
            open.push_back(next);
        }
    }

    std::sort(
        visible_chunks_.begin(),
        visible_chunks_.end(),
        [&](const ActiveChunk& lhs, const ActiveChunk& rhs) {
            const float lhs_score = corridor_priority_score(
                observer_chunk_,
                lhs.coord,
                stream_direction,
                observer_speed_blocks_per_second_
            );
            const float rhs_score = corridor_priority_score(
                observer_chunk_,
                rhs.coord,
                stream_direction,
                observer_speed_blocks_per_second_
            );

            if (lhs_score != rhs_score) {
                return lhs_score < rhs_score;
            }

            if (lhs.coord.x != rhs.coord.x) {
                return lhs.coord.x < rhs.coord.x;
            }

            return lhs.coord.z < rhs.coord.z;
        }
    );
}
std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads() {
    std::vector<PendingChunkUpload> uploads;
    uploads.reserve(pending_uploads_.size());
    for (PendingChunkUpload& upload : pending_uploads_) {
        const auto it = chunks_.find(upload.coord);
        if (it == chunks_.end() ||
            it->second.generation_version != upload.version ||
            it->second.latest_rebuild_serial != upload.rebuild_serial ||
            it->second.latest_upload_token != upload.upload_token) {
            record_stale_upload_drop(upload.coord);
            continue;
        }
        uploads.push_back(std::move(upload));
    }
    pending_uploads_.clear();
    return uploads;
}

std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads(std::size_t max_count, Vec3 observer_position) {
    return drain_pending_uploads(max_count, observer_position, observer_forward_);
}

std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads(std::size_t max_count, Vec3 observer_position, Vec3 observer_forward) {
    if (max_count == 0 || pending_uploads_.empty()) {
        return {};
    }

    const Vec3 planar_forward = normalize({observer_forward.x, 0.0f, observer_forward.z});

    const std::size_t upload_count = std::min(max_count, pending_uploads_.size());
    const auto upload_end = pending_uploads_.begin() + static_cast<std::ptrdiff_t>(upload_count);
    std::partial_sort(
        pending_uploads_.begin(),
        upload_end,
        pending_uploads_.end(),
        [&](const PendingChunkUpload& lhs, const PendingChunkUpload& rhs) {
            return chunk_priority_score(lhs.coord, observer_position, planar_forward) <
                chunk_priority_score(rhs.coord, observer_position, planar_forward);
        }
    );

    std::vector<PendingChunkUpload> uploads;
    uploads.reserve(upload_count);
    std::size_t consumed_count = 0;
    while (consumed_count < upload_count) {
        PendingChunkUpload& pending = pending_uploads_[consumed_count];
        auto it = chunks_.find(pending.coord);
        if (it == chunks_.end() ||
            it->second.generation_version != pending.version ||
            it->second.latest_rebuild_serial != pending.rebuild_serial ||
            it->second.latest_upload_token != pending.upload_token) {
            record_stale_upload_drop(pending.coord);
            ++consumed_count;
            continue;
        }
        uploads.push_back(std::move(pending));
        ++consumed_count;
    }
    pending_uploads_.erase(pending_uploads_.begin(), pending_uploads_.begin() + static_cast<std::ptrdiff_t>(consumed_count));
    return uploads;
}


std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads_by_budget(
    std::size_t byte_budget,
    std::size_t max_count,
    Vec3 observer_position,
    Vec3 observer_forward
) {
    std::vector<PendingChunkUpload> selected;
    if (max_count == 0 || pending_uploads_.empty()) {
        return selected;
    }

    const ChunkCoord origin = world_to_chunk(observer_position);
    const Vec3 stream_direction = normalized_horizontal_direction(observer_forward);

    const bool corridor_mode = corridor_mode_for_speed(observer_speed_blocks_per_second_);
    const std::size_t effective_max_count = corridor_mode
        ? std::min(max_count, corridor_uploads_per_frame())
        : max_count;

    const std::size_t effective_byte_budget = corridor_mode
        ? std::min(byte_budget, corridor_upload_byte_budget())
        : byte_budget;

    std::stable_sort(
        pending_uploads_.begin(),
        pending_uploads_.end(),
        [&](const PendingChunkUpload& lhs, const PendingChunkUpload& rhs) {
            const float lhs_score = corridor_priority_score(
                origin,
                lhs.coord,
                stream_direction,
                observer_speed_blocks_per_second_
            );
            const float rhs_score = corridor_priority_score(
                origin,
                rhs.coord,
                stream_direction,
                observer_speed_blocks_per_second_
            );

            if (lhs_score != rhs_score) {
                return lhs_score < rhs_score;
            }

            if (lhs.coord.x != rhs.coord.x) {
                return lhs.coord.x < rhs.coord.x;
            }

            return lhs.coord.z < rhs.coord.z;
        }
    );

    std::size_t used_bytes = 0;
    std::size_t take_count = 0;

    while (take_count < pending_uploads_.size() && take_count < effective_max_count) {
        const std::size_t upload_bytes = mesh_byte_count(pending_uploads_[take_count].mesh);
        if (take_count > 0 && used_bytes + upload_bytes > effective_byte_budget) {
            break;
        }

        used_bytes += upload_bytes;
        ++take_count;
    }

    selected.reserve(take_count);
    for (std::size_t index = 0; index < take_count; ++index) {
        selected.push_back(std::move(pending_uploads_[index]));
    }

    if (take_count > 0) {
        pending_uploads_.erase(
            pending_uploads_.begin(),
            pending_uploads_.begin() + static_cast<std::ptrdiff_t>(take_count)
        );
    }

    return selected;
}
bool WorldStreamer::confirm_chunk_uploaded(
    ChunkCoord coord,
    std::uint64_t version,
    std::uint64_t rebuild_serial,
    std::uint64_t upload_token
) {
    auto it = chunks_.find(coord);
    if (it == chunks_.end() ||
        it->second.generation_version != version ||
        it->second.latest_rebuild_serial != rebuild_serial ||
        it->second.latest_upload_token != upload_token) {
        record_stale_upload_drop(coord);
        return false;
    }

    it->second.state = ChunkState::UploadedToGPU;
    it->second.uploaded_to_gpu = true;
    return true;
}

std::vector<ChunkCoord> WorldStreamer::drain_pending_unloads() {
    std::vector<ChunkCoord> unloads;
    unloads.swap(pending_unloads_);
    return unloads;
}

WorldStreamer::StreamingStats WorldStreamer::stats() const {
    std::lock_guard lock(mutex_);
    std::size_t queued_rebuilds = 0;
    for (const auto& [coord, state] : rebuild_states_) {
        (void)coord;
        if (state.queued) {
            ++queued_rebuilds;
        }
    }
    std::size_t queued_generates = 0;
    std::size_t queued_decorates = 0;
    std::size_t queued_lights = 0;
    std::size_t queued_meshes = 0;
    std::size_t queued_fast_meshes = 0;
    std::size_t queued_final_meshes = 0;
    for (const ChunkJob& job : job_queue_) {
        if (job.type == ChunkJobType::GenerateTerrain) {
            ++queued_generates;
        } else if (job.type == ChunkJobType::Decorate) {
            ++queued_decorates;
        } else if (job.type == ChunkJobType::CalculateLight) {
            ++queued_lights;
        } else if (job.type == ChunkJobType::BuildMesh) {
            ++queued_meshes;
            if (job.snapshot != nullptr && job.snapshot->provisional) {
                ++queued_fast_meshes;
            } else {
                ++queued_final_meshes;
            }
        }
    }
    std::size_t pending_upload_bytes = 0;
    std::size_t provisional_uploads = 0;
    for (const PendingChunkUpload& upload : pending_uploads_) {
        pending_upload_bytes += mesh_byte_count(upload.mesh);
        if (upload.provisional) {
            ++provisional_uploads;
        }
    }
    bool observer_light_borders_ready = false;
    int observer_light_border_status = 0;
    if (const auto observer_it = chunks_.find(observer_chunk_);
        observer_it != chunks_.end() && observer_it->second.light.has_value()) {
        observer_light_borders_ready = observer_it->second.light->borders_ready;
        observer_light_border_status = observer_light_borders_ready ? 1 : 0;
        const std::array<ChunkCoord, 4> cardinal_neighbors {
            ChunkCoord {observer_chunk_.x - 1, observer_chunk_.z},
            ChunkCoord {observer_chunk_.x + 1, observer_chunk_.z},
            ChunkCoord {observer_chunk_.x, observer_chunk_.z - 1},
            ChunkCoord {observer_chunk_.x, observer_chunk_.z + 1}
        };
        const std::array<ChunkCoord, 4> diagonal_neighbors {
            ChunkCoord {observer_chunk_.x - 1, observer_chunk_.z - 1},
            ChunkCoord {observer_chunk_.x + 1, observer_chunk_.z - 1},
            ChunkCoord {observer_chunk_.x - 1, observer_chunk_.z + 1},
            ChunkCoord {observer_chunk_.x + 1, observer_chunk_.z + 1}
        };
        const auto has_final_light = [this](ChunkCoord coord) {
            const auto it = chunks_.find(coord);
            return it != chunks_.end() &&
                it->second.light.has_value() &&
                it->second.light->borders_ready;
        };
        const bool cardinal_ready = std::all_of(cardinal_neighbors.begin(), cardinal_neighbors.end(), has_final_light);
        const bool diagonal_ready = std::all_of(diagonal_neighbors.begin(), diagonal_neighbors.end(), has_final_light);
        if (cardinal_ready && diagonal_ready) {
            observer_light_border_status = 2;
        } else if (cardinal_ready) {
            observer_light_border_status = 1;
        }
    }

    return {
        visible_chunks_.size(),
        pending_uploads_.size(),
        queued_rebuilds,
        queued_generates,
        queued_decorates,
        queued_lights,
        queued_meshes,
        queued_fast_meshes,
        queued_final_meshes,
        pending_upload_bytes,
        stale_results_,
        stale_uploads_dropped_,
        provisional_uploads,
        light_stale_results_,
        edge_fixups_,
        dropped_jobs_,
        dirty_save_set_.size(),
        observer_light_borders_ready,
        observer_light_border_status,
        last_generate_ms_,
        last_light_ms_,
        last_mesh_ms_
    };
}

BlockQueryResult WorldStreamer::query_block_at_world(int x, int y, int z) const {
    if (!contains_world_y(y)) {
        return {BlockQueryStatus::OutOfBounds, BlockId::Air};
    }

    const auto floor_div = [](int value, int divisor) -> int {
        if (value >= 0) {
            return value / divisor;
        }
        return -(((-value) + divisor - 1) / divisor);
    };

    const auto positive_mod = [](int value, int divisor) -> int {
        const int result = value % divisor;
        return result < 0 ? result + divisor : result;
    };

    const ChunkCoord chunk_coord {
        floor_div(x, kChunkWidth),
        floor_div(z, kChunkDepth)
    };

    const auto chunk_it = chunks_.find(chunk_coord);
    if (chunk_it == chunks_.end() || !chunk_it->second.data.has_value()) {
        return {BlockQueryStatus::Unloaded, BlockId::Air};
    }

    return {
        BlockQueryStatus::Loaded,
        chunk_it->second.data->get(positive_mod(x, kChunkWidth), world_y_to_local_y(y), positive_mod(z, kChunkDepth))
    };
}

BlockId WorldStreamer::block_at_world(int x, int y, int z) const {
    return query_block_at_world(x, y, z).block;
}

bool WorldStreamer::is_solid_at_world(int x, int y, int z) const {
    return block_registry_.is_solid(block_at_world(x, y, z));
}

std::optional<BlockHit> WorldStreamer::raycast(const Vec3& origin, const Vec3& direction, float max_distance) const {
    const Vec3 dir = normalize(direction);
    if (length(dir) <= 0.00001f) {
        return std::nullopt;
    }

    const Vec3 adjusted_origin = origin + dir * kRaycastEpsilon;

    const auto floor_to_int = [](float value) -> int {
        return static_cast<int>(std::floor(value));
    };

    const auto select_t = [](float value, int step) -> float {
        if (step > 0) {
            return 1.0f - value;
        }
        if (step < 0) {
            return value;
        }
        return std::numeric_limits<float>::infinity();
    };

    Int3 cell {
        floor_to_int(adjusted_origin.x),
        floor_to_int(adjusted_origin.y),
        floor_to_int(adjusted_origin.z)
    };

    if (block_registry_.is_solid(block_at_world(cell.x, cell.y, cell.z))) {
        return BlockHit {true, cell, {}, cell, 0.0f};
    }

    const int step_x = dir.x > 0.0f ? 1 : (dir.x < 0.0f ? -1 : 0);
    const int step_y = dir.y > 0.0f ? 1 : (dir.y < 0.0f ? -1 : 0);
    const int step_z = dir.z > 0.0f ? 1 : (dir.z < 0.0f ? -1 : 0);

    const float local_x = adjusted_origin.x - std::floor(adjusted_origin.x);
    const float local_y = adjusted_origin.y - std::floor(adjusted_origin.y);
    const float local_z = adjusted_origin.z - std::floor(adjusted_origin.z);

    float t_max_x = step_x == 0 ? std::numeric_limits<float>::infinity() : select_t(local_x, step_x) / std::abs(dir.x);
    float t_max_y = step_y == 0 ? std::numeric_limits<float>::infinity() : select_t(local_y, step_y) / std::abs(dir.y);
    float t_max_z = step_z == 0 ? std::numeric_limits<float>::infinity() : select_t(local_z, step_z) / std::abs(dir.z);

    const float t_delta_x = step_x == 0 ? std::numeric_limits<float>::infinity() : 1.0f / std::abs(dir.x);
    const float t_delta_y = step_y == 0 ? std::numeric_limits<float>::infinity() : 1.0f / std::abs(dir.y);
    const float t_delta_z = step_z == 0 ? std::numeric_limits<float>::infinity() : 1.0f / std::abs(dir.z);

    float distance = 0.0f;
    while (distance <= max_distance) {
        const float next_t = std::min(t_max_x, std::min(t_max_y, t_max_z));
        const bool step_x_axis = nearly_equal(t_max_x, next_t);
        const bool step_y_axis = nearly_equal(t_max_y, next_t);
        const bool step_z_axis = nearly_equal(t_max_z, next_t);
        distance = next_t;

        if (distance > max_distance) {
            break;
        }

        if (step_y_axis && step_y != 0) {
            const Int3 candidate {cell.x, cell.y + step_y, cell.z};
            if (block_registry_.is_solid(block_at_world(candidate.x, candidate.y, candidate.z))) {
                return BlockHit {true, candidate, {0, -step_y, 0}, cell, distance};
            }
        }
        if (step_x_axis && step_x != 0) {
            const Int3 candidate {cell.x + step_x, cell.y, cell.z};
            if (block_registry_.is_solid(block_at_world(candidate.x, candidate.y, candidate.z))) {
                return BlockHit {true, candidate, {-step_x, 0, 0}, cell, distance};
            }
        }
        if (step_z_axis && step_z != 0) {
            const Int3 candidate {cell.x, cell.y, cell.z + step_z};
            if (block_registry_.is_solid(block_at_world(candidate.x, candidate.y, candidate.z))) {
                return BlockHit {true, candidate, {0, 0, -step_z}, cell, distance};
            }
        }

        if (step_x_axis) {
            cell.x += step_x;
            t_max_x += t_delta_x;
        }
        if (step_y_axis) {
            cell.y += step_y;
            t_max_y += t_delta_y;
        }
        if (step_z_axis) {
            cell.z += step_z;
            t_max_z += t_delta_z;
        }
    }

    return std::nullopt;
}

SetBlockResult WorldStreamer::set_block_at_world(int x, int y, int z, BlockId block) {
    if (!contains_world_y(y)) {
        return SetBlockResult::OutOfBounds;
    }

    const auto positive_mod = [](int value, int divisor) -> int {
        const int result = value % divisor;
        return result < 0 ? result + divisor : result;
    };

    const ChunkCoord chunk_coord = world_to_chunk(x, z);
    auto chunk_it = chunks_.find(chunk_coord);
    if (chunk_it == chunks_.end() || !chunk_it->second.data.has_value()) {
        return SetBlockResult::ChunkUnloaded;
    }

    ChunkData& chunk = *chunk_it->second.data;
    const int local_x = positive_mod(x, kChunkWidth);
    const int local_y = world_y_to_local_y(y);
    const int local_z = positive_mod(z, kChunkDepth);
    const BlockId existing = chunk.get(local_x, local_y, local_z);
    if (existing == block) {
        return SetBlockResult::NoChange;
    }
    if (block != BlockId::Air && !block_registry_.is_replaceable(existing)) {
        return SetBlockResult::Occupied;
    }

    chunk.set(local_x, local_y, local_z, block);

    if (block != BlockId::Air && contains_world_y(y - 1)) {
        const int below_local_y = world_y_to_local_y(y - 1);
        if (below_local_y >= 0 && below_local_y < kChunkHeight &&
            chunk.get(local_x, below_local_y, local_z) == BlockId::Grass) {
            queue_delayed_grass_update_locked({x, y - 1, z}, kGrassCoveredDecayDelayFrames);
        }
    }

    const std::uint64_t new_version = next_chunk_version_++;
    chunk_it->second.generation_version = new_version;
    chunk_it->second.mesh_version = 0;
    const bool light_affecting_change =
        block_registry_.light_dampening(existing) != block_registry_.light_dampening(block) ||
        block_registry_.light_emission(existing) != block_registry_.light_emission(block);
    if (light_affecting_change) {
        chunk_it->second.dirty_light = true;
        chunk_it->second.needs_final_light = true;
        chunk_it->second.state = ChunkState::LightPropagating;
    }
    chunk_it->second.dirty_mesh = true;
    mark_chunk_dirty_for_save(chunk_coord);
    const bool touches_west = local_x < kLightBorder;
    const bool touches_east = local_x >= kChunkWidth - kLightBorder;
    const bool touches_north = local_z < kLightBorder;
    const bool touches_south = local_z >= kChunkDepth - kLightBorder;

    const auto queue_affected = [&](auto&& queue_fn) {
        queue_fn(chunk_coord);
        if (touches_west) {
            queue_fn({chunk_coord.x - 1, chunk_coord.z});
        }
        if (touches_east) {
            queue_fn({chunk_coord.x + 1, chunk_coord.z});
        }
        if (touches_north) {
            queue_fn({chunk_coord.x, chunk_coord.z - 1});
        }
        if (touches_south) {
            queue_fn({chunk_coord.x, chunk_coord.z + 1});
        }
        if (touches_west && touches_north) {
            queue_fn({chunk_coord.x - 1, chunk_coord.z - 1});
        }
        if (touches_east && touches_north) {
            queue_fn({chunk_coord.x + 1, chunk_coord.z - 1});
        }
        if (touches_west && touches_south) {
            queue_fn({chunk_coord.x - 1, chunk_coord.z + 1});
        }
        if (touches_east && touches_south) {
            queue_fn({chunk_coord.x + 1, chunk_coord.z + 1});
        }
    };

    if (light_affecting_change) {
        queue_affected([&](ChunkCoord affected) {
            queue_light_job_if_loaded(affected);
        });
    } else {
        queue_affected([&](ChunkCoord affected) {
            queue_rebuild_job_if_loaded(affected);
        });
    }
    return SetBlockResult::Success;
}

void WorldStreamer::queue_delayed_grass_update_locked(Int3 block, std::uint64_t delay_frames) {
    PendingGrassBlockUpdate update {};
    update.block = block;
    update.due_frame = frame_counter_ + delay_frames;
    pending_grass_updates_.push_back(update);
}

bool WorldStreamer::apply_grass_lifecycle_at_locked(int x, int y, int z) {
    if (!contains_world_y(y) || !contains_world_y(y + 1)) {
        return false;
    }

    const auto positive_mod = [](int value, int divisor) -> int {
        const int result = value % divisor;
        return result < 0 ? result + divisor : result;
    };

    const ChunkCoord coord = world_to_chunk(x, z);
    auto chunk_it = chunks_.find(coord);
    if (chunk_it == chunks_.end() || !chunk_it->second.data.has_value()) {
        return false;
    }

    ChunkRecord& record = chunk_it->second;
    if (!record.uploaded_to_gpu || record.dirty_mesh || rebuild_states_.find(coord) != rebuild_states_.end()) {
        return false;
    }

    const bool chunk_waiting_for_upload = std::any_of(
        pending_uploads_.begin(),
        pending_uploads_.end(),
        [&](const PendingChunkUpload& upload) {
            return upload.coord == coord;
        }
    );
    if (chunk_waiting_for_upload) {
        return false;
    }

    ChunkData& chunk = *record.data;
    const int local_x = positive_mod(x, kChunkWidth);
    const int local_y = world_y_to_local_y(y);
    const int local_z = positive_mod(z, kChunkDepth);

    if (local_y < 0 || local_y >= kChunkHeight - 1) {
        return false;
    }

    const BlockId current = chunk.get(local_x, local_y, local_z);
    const BlockId above = chunk.get(local_x, local_y + kGrassBlockedCheckHeight, local_z);
    const bool above_is_air = above == BlockId::Air;

    BlockId next = current;
    if (current == BlockId::Grass && !above_is_air) {
        next = BlockId::Dirt;
    } else if (current == BlockId::Dirt && above_is_air) {
        next = BlockId::Grass;
    } else {
        return false;
    }

    chunk.set(local_x, local_y, local_z, next);

    const std::uint64_t new_version = next_chunk_version_++;
    record.generation_version = new_version;
    record.mesh_version = 0;
    record.dirty_mesh = true;
    mark_chunk_dirty_for_save(coord);
    queue_rebuild_self_and_neighbors_if_loaded_locked(coord, false);
    return true;
}

void WorldStreamer::tick_grass_updates_locked() {
    std::size_t processed_pending = 0;
    while (!pending_grass_updates_.empty() && processed_pending < kMaxPendingGrassUpdatesPerTick) {
        PendingGrassBlockUpdate update = pending_grass_updates_.front();
        pending_grass_updates_.pop_front();
        ++processed_pending;

        if (frame_counter_ < update.due_frame) {
            pending_grass_updates_.push_back(update);
            break;
        }

        const bool changed = apply_grass_lifecycle_at_locked(update.block.x, update.block.y, update.block.z);
        if (!changed) {
            // If the chunk is still streaming, retry shortly instead of dropping the update.
            if (frame_counter_ < update.due_frame + kGrassUpdateIntervalFrames * 8) {
                update.due_frame = frame_counter_ + kGrassUpdateIntervalFrames;
                pending_grass_updates_.push_back(update);
            }
        }
    }

    if (frame_counter_ < next_grass_update_frame_) {
        return;
    }
    next_grass_update_frame_ = frame_counter_ + kGrassUpdateIntervalFrames;

    // Do not run random grass ticks before the first terrain is visible.
    // Unlike the previous fix, this no longer waits for the whole streaming queue
    // to become empty, because on Android that made grass updates almost never run.
    if (visible_chunks_.size() < 4) {
        return;
    }

    if (chunks_.empty()) {
        grass_update_chunk_cursor_ = 0;
        return;
    }

    std::vector<ChunkCoord> loaded_coords;
    loaded_coords.reserve(chunks_.size());
    for (const auto& [coord, record] : chunks_) {
        if (record.data.has_value() && record.uploaded_to_gpu && desired_chunk(observer_chunk_, coord)) {
            loaded_coords.push_back(coord);
        }
    }

    if (loaded_coords.empty()) {
        grass_update_chunk_cursor_ = 0;
        return;
    }

    std::sort(loaded_coords.begin(), loaded_coords.end(), [](const ChunkCoord& lhs, const ChunkCoord& rhs) {
        if (lhs.x != rhs.x) {
            return lhs.x < rhs.x;
        }
        return lhs.z < rhs.z;
    });

    if (grass_update_chunk_cursor_ >= loaded_coords.size()) {
        grass_update_chunk_cursor_ = 0;
    }

    const std::size_t chunks_to_check = std::min(kGrassUpdateChunksPerTick, loaded_coords.size());
    for (std::size_t chunk_offset = 0; chunk_offset < chunks_to_check; ++chunk_offset) {
        const ChunkCoord coord = loaded_coords[(grass_update_chunk_cursor_ + chunk_offset) % loaded_coords.size()];
        auto chunk_it = chunks_.find(coord);
        if (chunk_it == chunks_.end() || !chunk_it->second.data.has_value()) {
            continue;
        }

        ChunkRecord& record = chunk_it->second;
        if (!record.uploaded_to_gpu || record.dirty_mesh || rebuild_states_.find(coord) != rebuild_states_.end()) {
            continue;
        }

        const bool chunk_waiting_for_upload = std::any_of(
            pending_uploads_.begin(),
            pending_uploads_.end(),
            [&](const PendingChunkUpload& upload) {
                return upload.coord == coord;
            }
        );
        if (chunk_waiting_for_upload) {
            continue;
        }

        ChunkData& chunk = *record.data;
        bool changed = false;

        const int column_count = kChunkWidth * kChunkDepth;
        const int base_column = static_cast<int>(
            (frame_counter_ * 37u + static_cast<std::uint64_t>((coord.x * 31) ^ (coord.z * 17))) %
            static_cast<std::uint64_t>(column_count)
        );

        for (int attempt = 0; attempt < kGrassUpdateColumnAttemptsPerChunk && !changed; ++attempt) {
            const int column = (base_column + attempt * 73) % column_count;
            const int local_x = column % kChunkWidth;
            const int local_z = column / kChunkWidth;

            for (int local_y = kChunkHeight - 2; local_y >= 0; --local_y) {
                const BlockId current = chunk.get(local_x, local_y, local_z);
                const BlockId above = chunk.get(local_x, local_y + kGrassBlockedCheckHeight, local_z);
                const bool above_is_air = above == BlockId::Air;

                if (current == BlockId::Grass && !above_is_air) {
                    chunk.set(local_x, local_y, local_z, BlockId::Dirt);
                    changed = true;
                    break;
                }

                if (current == BlockId::Dirt && above_is_air) {
                    chunk.set(local_x, local_y, local_z, BlockId::Grass);
                    changed = true;
                    break;
                }

                if (block_registry_.is_opaque(current) && current != BlockId::Grass && current != BlockId::Dirt) {
                    break;
                }
            }
        }

        if (!changed) {
            continue;
        }

        const std::uint64_t new_version = next_chunk_version_++;
        record.generation_version = new_version;
        record.mesh_version = 0;
        record.dirty_mesh = true;
        mark_chunk_dirty_for_save(coord);
        queue_rebuild_self_and_neighbors_if_loaded_locked(coord, false);
    }

    grass_update_chunk_cursor_ = (grass_update_chunk_cursor_ + chunks_to_check) % loaded_coords.size();
}


void WorldStreamer::set_leaves_render_mode(LeavesRenderMode mode) {
    std::lock_guard lock(mutex_);
    if (leaves_render_mode_ == mode) {
        return;
    }

    leaves_render_mode_ = mode;
    for (const auto& [coord, record] : chunks_) {
        if (record.data.has_value()) {
            queue_rebuild_job_if_loaded_locked(coord);
        }
    }
}

LeavesRenderMode WorldStreamer::leaves_render_mode() const {
    std::lock_guard lock(mutex_);
    return leaves_render_mode_;
}

int WorldStreamer::chunk_radius() const {
    return chunk_radius_;
}

void WorldStreamer::set_chunk_radius(int radius) {
    const int clamped_radius = std::clamp(radius, kMinChunkRadius, kMaxChunkRadius);
    if (clamped_radius == chunk_radius_) {
        return;
    }

    chunk_radius_ = clamped_radius;
    observer_chunk_ = {std::numeric_limits<int>::max(), std::numeric_limits<int>::max()};
    log_message(LogLevel::Info, std::string("WorldStreamer: render distance set to ") + std::to_string(chunk_radius_));
    update_observer(observer_position_, observer_forward_);
}

void WorldStreamer::flush_dirty_chunks(std::size_t max_chunks) {
    if (world_save_ == nullptr || max_chunks == 0) {
        return;
    }

    std::size_t saved = 0;
    while (!dirty_save_queue_.empty() && saved < max_chunks) {
        const ChunkCoord coord = dirty_save_queue_.front();
        dirty_save_queue_.pop_front();
        dirty_save_set_.erase(coord);

        auto it = chunks_.find(coord);
        if (it == chunks_.end() || !it->second.dirty_save || !it->second.data.has_value()) {
            continue;
        }
        if (world_save_->save_chunk(coord, *it->second.data)) {
            it->second.dirty_save = false;
            ++saved;
        }
    }
}

void WorldStreamer::flush_all_dirty_chunks() {
    while (!dirty_save_queue_.empty()) {
        const std::size_t before = dirty_save_queue_.size();
        flush_dirty_chunks(before);
        if (dirty_save_queue_.size() == before) {
            break;
        }
    }
}

void WorldStreamer::mark_chunk_dirty_for_save(ChunkCoord coord) {
    auto it = chunks_.find(coord);
    if (it == chunks_.end()) {
        return;
    }
    it->second.dirty_save = true;
    enqueue_dirty_save(coord);
}

void WorldStreamer::enqueue_dirty_save(ChunkCoord coord) {
    if (world_save_ == nullptr || dirty_save_set_.contains(coord)) {
        return;
    }
    dirty_save_set_.insert(coord);
    dirty_save_queue_.push_back(coord);
}

void WorldStreamer::worker_loop() {
    while (true) {
        ChunkJob job {};

        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() { return stop_requested_ || !job_queue_.empty(); });
            if (stop_requested_) {
                return;
            }

            job = std::move(job_queue_.front());
            job_queue_.pop_front();
        }

        JobResult result {};
        result.coord = job.coord;
        result.version = job.version;
        result.light_job_token = job.light_job_token;
        result.rebuild_serial = job.rebuild_serial;
        result.type = job.type;

        try {
            if (job.type == ChunkJobType::GenerateTerrain) {
                const auto start = std::chrono::steady_clock::now();
                if (world_save_ != nullptr) {
                    result.chunk_data = world_save_->load_chunk(job.coord);
                }
                if (!result.chunk_data.has_value()) {
                    result.chunk_data = generator_.generate_chunk(job.coord, seed_);
                }
                const auto end = std::chrono::steady_clock::now();
                result.generate_ms = std::chrono::duration<float, std::milli>(end - start).count();
            } else if (job.type == ChunkJobType::Decorate) {
                result.chunk_data = std::move(job.chunk_data);
            } else if (job.type == ChunkJobType::CalculateLight) {
                const auto start = std::chrono::steady_clock::now();
                if (job.light_snapshot != nullptr) {
                    ChunkLightResult light_result = calculate_chunk_light(*job.light_snapshot, block_registry_);
                    result.provisional = light_result.provisional;
                    result.borders_ready = light_result.borders_ready;
                    result.border_signature = light_result.border_signature;
                    result.light = std::move(light_result.light);
                }
                const auto end = std::chrono::steady_clock::now();
                result.light_ms = std::chrono::duration<float, std::milli>(end - start).count();
            } else if (job.snapshot != nullptr) {
                const ChunkMeshSnapshot& snapshot = *job.snapshot;
                const auto start = std::chrono::steady_clock::now();
                result.mesh = build_chunk_mesh(snapshot.chunk, job.coord, block_registry_, neighbors_from_snapshot(snapshot), light_from_snapshot(snapshot), snapshot.leaves_mode);
                result.provisional = snapshot.provisional;
                const auto end = std::chrono::steady_clock::now();
                result.mesh_ms = std::chrono::duration<float, std::milli>(end - start).count();
            }
        } catch (const std::exception& error) {
            log_message(
                LogLevel::Error,
                std::string("WorldStreamer: worker job failed coord=(") +
                    std::to_string(job.coord.x) + "," + std::to_string(job.coord.z) +
                    ") type=" + std::to_string(static_cast<int>(job.type)) +
                    " error=" + error.what()
            );
            continue;
        } catch (...) {
            log_message(
                LogLevel::Error,
                std::string("WorldStreamer: worker job failed with unknown exception coord=(") +
                    std::to_string(job.coord.x) + "," + std::to_string(job.coord.z) +
                    ") type=" + std::to_string(static_cast<int>(job.type))
            );
            continue;
        }

        {
            std::lock_guard lock(mutex_);
            completed_.push(std::move(result));
        }
    }
}

ChunkCoord WorldStreamer::world_to_chunk(Vec3 position) const {
    return world_to_chunk(static_cast<int>(std::floor(position.x)), static_cast<int>(std::floor(position.z)));
}

ChunkCoord WorldStreamer::world_to_chunk(int world_x, int world_z) const {
    const auto floor_div = [](int value, int divisor) -> int {
        if (value >= 0) {
            return value / divisor;
        }
        return -(((-value) + divisor - 1) / divisor);
    };

    return {
        floor_div(world_x, kChunkWidth),
        floor_div(world_z, kChunkDepth)
    };
}

bool WorldStreamer::desired_chunk(const ChunkCoord& origin, const ChunkCoord& candidate) const {
    return std::abs(candidate.x - origin.x) <= chunk_radius_
        && std::abs(candidate.z - origin.z) <= chunk_radius_;
}

float WorldStreamer::chunk_priority_score(ChunkCoord coord, Vec3 observer_position, Vec3 observer_forward) const {
    const ChunkCoord origin = world_to_chunk(observer_position);
    const float dx = static_cast<float>(coord.x - origin.x);
    const float dz = static_cast<float>(coord.z - origin.z);

    Vec3 forward = normalize({observer_forward.x, 0.0f, observer_forward.z});
    if (length(forward) <= 0.00001f) {
        forward = {0.0f, 0.0f, -1.0f};
    }

    const float right_x = -forward.z;
    const float right_z = forward.x;

    const float forward_axis = dx * forward.x + dz * forward.z;
    const float side_axis = dx * right_x + dz * right_z;
    const float side_abs = std::abs(side_axis);
    const float dist_sq = dx * dx + dz * dz;

    const int forward_depth = adaptive_forward_buffer_chunks(observer_speed_blocks_per_second_);
    const int forward_width = adaptive_forward_width_chunks(observer_speed_blocks_per_second_);
    const float half_width = static_cast<float>(forward_width) * 0.5f;

    const bool hot_zone = dist_sq <= 4.0f;
    const bool in_forward_corridor =
        forward_axis > 0.25f &&
        forward_axis <= static_cast<float>(forward_depth) + 0.5f &&
        side_abs <= half_width;

    if (hot_zone) {
        return -30000.0f + dist_sq + side_abs * 0.5f;
    }

    if (in_forward_corridor) {
        const float front_edge_distance = std::abs(static_cast<float>(forward_depth) - forward_axis);
        return
            -20000.0f +
            front_edge_distance * 12.0f +
            side_abs * 5.0f -
            forward_axis * 2.0f +
            dist_sq * 0.01f;
    }

    float score =
        dist_sq * 0.35f -
        forward_axis * forward_priority_weight() +
        side_abs * side_priority_weight();

    if (forward_axis < -0.25f) {
        score += back_priority_penalty() + std::abs(forward_axis) * forward_priority_weight();
    }

    return score;
}
float WorldStreamer::job_priority_score_locked(const ChunkJob& job) const {
    float score = chunk_priority_score(job.coord, observer_position_, observer_forward_);

    switch (job.type) {
    case ChunkJobType::GenerateTerrain:
        score -= 20.0f;
        break;
    case ChunkJobType::Decorate:
        score -= 12.0f;
        break;
    case ChunkJobType::CalculateLight:
        score -= 8.0f;
        break;
    case ChunkJobType::BuildMesh:
        if (job.snapshot != nullptr && job.snapshot->provisional) {
            score -= 10.0f;
        }
        break;
    }

    return score;
}

void WorldStreamer::push_job_locked(ChunkJob&& job) {
    constexpr std::size_t kMaxQueuedChunkJobs = 4096;

    if (job_queue_.size() >= kMaxQueuedChunkJobs) {
        const auto worst_it = std::max_element(
            job_queue_.begin(),
            job_queue_.end(),
            [this](const ChunkJob& lhs, const ChunkJob& rhs) {
                return job_priority_score_locked(lhs) < job_priority_score_locked(rhs);
            }
        );

        if (worst_it != job_queue_.end() && job_priority_score_locked(*worst_it) > job_priority_score_locked(job)) {
            *worst_it = std::move(job);
        } else {
            ++dropped_jobs_;
            return;
        }
    } else {
        job_queue_.push_back(std::move(job));
    }

    std::stable_sort(
        job_queue_.begin(),
        job_queue_.end(),
        [this](const ChunkJob& lhs, const ChunkJob& rhs) {
            return job_priority_score_locked(lhs) < job_priority_score_locked(rhs);
        }
    );

    cv_.notify_one();
}

void WorldStreamer::queue_generate_job(ChunkCoord coord, std::uint64_t version) {
    {
        std::lock_guard lock(mutex_);
        push_job_locked({coord, version, 0, 0, ChunkJobType::GenerateTerrain, std::nullopt, nullptr, nullptr});
    }
    cv_.notify_one();
}

void WorldStreamer::queue_stage_job_locked(ChunkCoord coord, std::uint64_t version, ChunkJobType type, std::optional<ChunkData>&& chunk_data) {
    if (type == ChunkJobType::CalculateLight) {
        auto chunk_it = chunks_.find(coord);
        if (chunk_it == chunks_.end() || !chunk_it->second.data.has_value()) {
            return;
        }
        if (queued_light_jobs_.contains(coord)) {
            chunk_it->second.dirty_light = true;
            chunk_it->second.needs_final_light = true;
            chunk_it->second.latest_light_job_token = next_light_job_token_++;
            return;
        }
        std::optional<LightBuildSnapshot> light_snapshot = make_light_build_snapshot(coord, *chunk_it->second.data);
        if (light_snapshot.has_value()) {
            const std::uint64_t light_job_token = next_light_job_token_++;
            chunk_it->second.latest_light_job_token = light_job_token;
            queued_light_jobs_.insert(coord);
            push_job_locked({
                coord,
                version,
                light_job_token,
                0,
                type,
                std::nullopt,
                nullptr,
                std::make_shared<LightBuildSnapshot>(std::move(*light_snapshot))
            });
            cv_.notify_one();
            return;
        }
    }
    push_job_locked({coord, version, 0, 0, type, std::move(chunk_data), nullptr, nullptr});
    cv_.notify_one();
}

void WorldStreamer::queue_light_job_if_loaded(ChunkCoord coord) {
    std::lock_guard lock(mutex_);
    queue_light_job_if_loaded_locked(coord);
}

void WorldStreamer::queue_light_job_if_loaded_locked(ChunkCoord coord) {
    const auto it = chunks_.find(coord);
    if (it == chunks_.end() || !it->second.data.has_value()) {
        return;
    }
    if (queued_light_jobs_.contains(coord)) {
        it->second.dirty_light = true;
        it->second.needs_final_light = true;
        it->second.latest_light_job_token = next_light_job_token_++;
        return;
    }
    it->second.dirty_light = true;
    it->second.needs_final_light = true;
    it->second.state = ChunkState::LightPropagating;
    queue_stage_job_locked(coord, it->second.generation_version, ChunkJobType::CalculateLight, std::nullopt);
}

void WorldStreamer::queue_light_self_and_neighbors_if_loaded_locked(ChunkCoord coord, bool include_diagonals) {
    queue_light_job_if_loaded_locked(coord);
    queue_light_job_if_loaded_locked({coord.x - 1, coord.z});
    queue_light_job_if_loaded_locked({coord.x + 1, coord.z});
    queue_light_job_if_loaded_locked({coord.x, coord.z - 1});
    queue_light_job_if_loaded_locked({coord.x, coord.z + 1});
    if (!include_diagonals) {
        return;
    }
    queue_light_job_if_loaded_locked({coord.x - 1, coord.z - 1});
    queue_light_job_if_loaded_locked({coord.x + 1, coord.z - 1});
    queue_light_job_if_loaded_locked({coord.x - 1, coord.z + 1});
    queue_light_job_if_loaded_locked({coord.x + 1, coord.z + 1});
}

void WorldStreamer::queue_rebuild_job_if_loaded(ChunkCoord coord) {
    std::lock_guard lock(mutex_);
    queue_rebuild_job_if_loaded_locked(coord);
}

void WorldStreamer::queue_rebuild_job_if_loaded_locked(ChunkCoord coord) {
    auto state_it = rebuild_states_.find(coord);
    if (state_it != rebuild_states_.end() && state_it->second.queued) {
        state_it->second.dirty = true;
        state_it->second.serial = next_rebuild_serial_++;
        if (auto chunk_it = chunks_.find(coord); chunk_it != chunks_.end()) {
            chunk_it->second.latest_rebuild_serial = state_it->second.serial;
        }
        if (logged_rebuild_lifecycle_count_ < 16) {
            log_message(
                LogLevel::Info,
                std::string("WorldStreamer: rebuild dirty coord=(") +
                    std::to_string(coord.x) + "," + std::to_string(coord.z) + ")"
            );
            ++logged_rebuild_lifecycle_count_;
        }
        return;
    }

    std::optional<ChunkMeshSnapshot> snapshot = make_rebuild_snapshot(coord);
    if (!snapshot.has_value()) {
        return;
    }

    RebuildState& state = rebuild_states_[coord];
    state.queued = true;
    state.dirty = false;
    state.serial = next_rebuild_serial_++;
    if (auto chunk_it = chunks_.find(coord); chunk_it != chunks_.end()) {
        chunk_it->second.latest_rebuild_serial = state.serial;
        if (!chunk_it->second.uploaded_to_gpu) {
            chunk_it->second.state = ChunkState::MeshQueued;
        }
    }
    if (logged_rebuild_lifecycle_count_ < 16) {
        log_message(
            LogLevel::Info,
            std::string("WorldStreamer: rebuild queued coord=(") +
                std::to_string(coord.x) + "," + std::to_string(coord.z) + ")"
        );
            ++logged_rebuild_lifecycle_count_;
    }
    push_job_locked({
        coord,
        snapshot->version,
        0,
        state.serial,
        ChunkJobType::BuildMesh,
        std::nullopt,
        std::make_shared<ChunkMeshSnapshot>(std::move(*snapshot)),
        nullptr
    });
    cv_.notify_one();
}

void WorldStreamer::queue_rebuild_self_and_neighbors_if_loaded_locked(ChunkCoord coord, bool include_diagonals) {
    queue_rebuild_job_if_loaded_locked(coord);
    queue_rebuild_job_if_loaded_locked({coord.x - 1, coord.z});
    queue_rebuild_job_if_loaded_locked({coord.x + 1, coord.z});
    queue_rebuild_job_if_loaded_locked({coord.x, coord.z - 1});
    queue_rebuild_job_if_loaded_locked({coord.x, coord.z + 1});
    if (!include_diagonals) {
        return;
    }
    queue_rebuild_job_if_loaded_locked({coord.x - 1, coord.z - 1});
    queue_rebuild_job_if_loaded_locked({coord.x + 1, coord.z - 1});
    queue_rebuild_job_if_loaded_locked({coord.x - 1, coord.z + 1});
    queue_rebuild_job_if_loaded_locked({coord.x + 1, coord.z + 1});
}

void WorldStreamer::record_stale_upload_drop(ChunkCoord coord) {
    ++stale_uploads_dropped_;
    if (logged_stale_upload_count_ >= 16) {
        return;
    }
    log_message(
        LogLevel::Warning,
        std::string("WorldStreamer: stale upload dropped coord=(") +
            std::to_string(coord.x) + "," + std::to_string(coord.z) + ")"
    );
    ++logged_stale_upload_count_;
}

std::optional<LightBuildSnapshot> WorldStreamer::make_light_build_snapshot(ChunkCoord coord, const ChunkData& chunk) const {
    const auto find_chunk_data = [this](ChunkCoord neighbor_coord) -> const ChunkData* {
        const auto it = chunks_.find(neighbor_coord);
        if (it == chunks_.end() || !it->second.data.has_value()) {
            return nullptr;
        }
        return &*it->second.data;
    };

    const auto side_x = [&](ChunkCoord neighbor_coord, int source_x_start) -> std::optional<LightBlockSideBorderX> {
        const ChunkData* neighbor = find_chunk_data(neighbor_coord);
        if (neighbor == nullptr) {
            return std::nullopt;
        }

        LightBlockSideBorderX border {};
        for (int strip_x = 0; strip_x < kLightBorder; ++strip_x) {
            const int source_x = source_x_start + strip_x;
            for (int y = 0; y < kChunkHeight; ++y) {
                for (int z = 0; z < kChunkDepth; ++z) {
                    border.blocks[static_cast<std::size_t>(strip_x + kLightBorder * (z + y * kChunkDepth))] =
                        neighbor->get(source_x, y, z);
                }
            }
        }
        return border;
    };

    const auto side_z = [&](ChunkCoord neighbor_coord, int source_z_start) -> std::optional<LightBlockSideBorderZ> {
        const ChunkData* neighbor = find_chunk_data(neighbor_coord);
        if (neighbor == nullptr) {
            return std::nullopt;
        }

        LightBlockSideBorderZ border {};
        for (int strip_z = 0; strip_z < kLightBorder; ++strip_z) {
            const int source_z = source_z_start + strip_z;
            for (int y = 0; y < kChunkHeight; ++y) {
                for (int x = 0; x < kChunkWidth; ++x) {
                    border.blocks[static_cast<std::size_t>(x + kChunkWidth * (strip_z + y * kLightBorder))] =
                        neighbor->get(x, y, source_z);
                }
            }
        }
        return border;
    };

    const auto corner = [&](ChunkCoord neighbor_coord, int source_x_start, int source_z_start) -> std::optional<LightBlockCornerBorder> {
        const ChunkData* neighbor = find_chunk_data(neighbor_coord);
        if (neighbor == nullptr) {
            return std::nullopt;
        }

        LightBlockCornerBorder border {};
        for (int strip_z = 0; strip_z < kLightBorder; ++strip_z) {
            const int source_z = source_z_start + strip_z;
            for (int y = 0; y < kChunkHeight; ++y) {
                for (int strip_x = 0; strip_x < kLightBorder; ++strip_x) {
                    const int source_x = source_x_start + strip_x;
                    border.blocks[static_cast<std::size_t>(strip_x + kLightBorder * (strip_z + y * kLightBorder))] =
                        neighbor->get(source_x, y, source_z);
                }
            }
        }
        return border;
    };

    auto west = side_x({coord.x - 1, coord.z}, kChunkWidth - kLightBorder);
    auto east = side_x({coord.x + 1, coord.z}, 0);
    auto north = side_z({coord.x, coord.z - 1}, kChunkDepth - kLightBorder);
    auto south = side_z({coord.x, coord.z + 1}, 0);
    auto northwest = corner({coord.x - 1, coord.z - 1}, kChunkWidth - kLightBorder, kChunkDepth - kLightBorder);
    auto northeast = corner({coord.x + 1, coord.z - 1}, 0, kChunkDepth - kLightBorder);
    auto southwest = corner({coord.x - 1, coord.z + 1}, kChunkWidth - kLightBorder, 0);
    auto southeast = corner({coord.x + 1, coord.z + 1}, 0, 0);

    const bool complete_cardinal_borders =
        west.has_value() && east.has_value() && north.has_value() && south.has_value();
    const bool complete_borders =
        complete_cardinal_borders &&
        northwest.has_value() && northeast.has_value() && southwest.has_value() && southeast.has_value();

    return LightBuildSnapshot {
        chunk,
        std::move(west),
        std::move(east),
        std::move(north),
        std::move(south),
        std::move(northwest),
        std::move(northeast),
        std::move(southwest),
        std::move(southeast),
        complete_cardinal_borders,
        complete_borders
    };
}

std::optional<WorldStreamer::ChunkMeshSnapshot> WorldStreamer::make_rebuild_snapshot(ChunkCoord coord) const {
    const auto find_chunk_data = [this](ChunkCoord neighbor_coord) -> const ChunkData* {
        const auto it = chunks_.find(neighbor_coord);
        if (it == chunks_.end() || !it->second.data.has_value()) {
            return nullptr;
        }
        return &*it->second.data;
    };
    const auto find_chunk_light = [this](ChunkCoord neighbor_coord) -> const ChunkLight* {
        const auto it = chunks_.find(neighbor_coord);
        if (it == chunks_.end() || !it->second.light.has_value()) {
            return nullptr;
        }
        return &*it->second.light;
    };

    const auto side_x = [&](ChunkCoord neighbor_coord, int source_x_start) -> std::optional<ChunkSideBorderX> {
        const ChunkData* chunk = find_chunk_data(neighbor_coord);
        if (chunk == nullptr) {
            return std::nullopt;
        }

        ChunkSideBorderX border {};
        for (int strip_x = 0; strip_x < kLightBorder; ++strip_x) {
            const int source_x = source_x_start + strip_x;
            for (int y = 0; y < kChunkHeight; ++y) {
                for (int z = 0; z < kChunkDepth; ++z) {
                    border.blocks[static_cast<std::size_t>(strip_x + kLightBorder * (z + y * kChunkDepth))] =
                        chunk->get(source_x, y, z);
                }
            }
        }
        return border;
    };

    const auto side_z = [&](ChunkCoord neighbor_coord, int source_z_start) -> std::optional<ChunkSideBorderZ> {
        const ChunkData* chunk = find_chunk_data(neighbor_coord);
        if (chunk == nullptr) {
            return std::nullopt;
        }

        ChunkSideBorderZ border {};
        for (int strip_z = 0; strip_z < kLightBorder; ++strip_z) {
            const int source_z = source_z_start + strip_z;
            for (int y = 0; y < kChunkHeight; ++y) {
                for (int x = 0; x < kChunkWidth; ++x) {
                    border.blocks[static_cast<std::size_t>(x + kChunkWidth * (strip_z + y * kLightBorder))] =
                        chunk->get(x, y, source_z);
                }
            }
        }
        return border;
    };

    const auto corner = [&](ChunkCoord neighbor_coord, int source_x_start, int source_z_start) -> std::optional<ChunkCornerBorder> {
        const ChunkData* chunk = find_chunk_data(neighbor_coord);
        if (chunk == nullptr) {
            return std::nullopt;
        }

        ChunkCornerBorder border {};
        for (int strip_z = 0; strip_z < kLightBorder; ++strip_z) {
            const int source_z = source_z_start + strip_z;
            for (int y = 0; y < kChunkHeight; ++y) {
                for (int strip_x = 0; strip_x < kLightBorder; ++strip_x) {
                    const int source_x = source_x_start + strip_x;
                    border.blocks[static_cast<std::size_t>(strip_x + kLightBorder * (strip_z + y * kLightBorder))] =
                        chunk->get(source_x, y, source_z);
                }
            }
        }
        return border;
    };
    const auto light_side_x = [&](ChunkCoord neighbor_coord, int source_x_start) -> std::optional<ChunkLightSideBorderX> {
        const ChunkLight* light = find_chunk_light(neighbor_coord);
        if (light == nullptr) {
            return std::nullopt;
        }

        ChunkLightSideBorderX border {};
        for (int strip_x = 0; strip_x < kLightBorder; ++strip_x) {
            const int source_x = source_x_start + strip_x;
            for (int y = 0; y < kChunkHeight; ++y) {
                for (int z = 0; z < kChunkDepth; ++z) {
                    border.sky[static_cast<std::size_t>(strip_x + kLightBorder * (z + y * kChunkDepth))] =
                        light->sky(source_x, y, z);
                }
            }
        }
        return border;
    };

    const auto light_side_z = [&](ChunkCoord neighbor_coord, int source_z_start) -> std::optional<ChunkLightSideBorderZ> {
        const ChunkLight* light = find_chunk_light(neighbor_coord);
        if (light == nullptr) {
            return std::nullopt;
        }

        ChunkLightSideBorderZ border {};
        for (int strip_z = 0; strip_z < kLightBorder; ++strip_z) {
            const int source_z = source_z_start + strip_z;
            for (int y = 0; y < kChunkHeight; ++y) {
                for (int x = 0; x < kChunkWidth; ++x) {
                    border.sky[static_cast<std::size_t>(x + kChunkWidth * (strip_z + y * kLightBorder))] =
                        light->sky(x, y, source_z);
                }
            }
        }
        return border;
    };

    const auto light_corner = [&](ChunkCoord neighbor_coord, int source_x_start, int source_z_start) -> std::optional<ChunkLightCornerBorder> {
        const ChunkLight* light = find_chunk_light(neighbor_coord);
        if (light == nullptr) {
            return std::nullopt;
        }

        ChunkLightCornerBorder border {};
        for (int strip_z = 0; strip_z < kLightBorder; ++strip_z) {
            const int source_z = source_z_start + strip_z;
            for (int y = 0; y < kChunkHeight; ++y) {
                for (int strip_x = 0; strip_x < kLightBorder; ++strip_x) {
                    const int source_x = source_x_start + strip_x;
                    border.sky[static_cast<std::size_t>(strip_x + kLightBorder * (strip_z + y * kLightBorder))] =
                        light->sky(source_x, y, source_z);
                }
            }
        }
        return border;
    };

    const auto chunk_it = chunks_.find(coord);
    if (chunk_it == chunks_.end() || !chunk_it->second.data.has_value() || !chunk_it->second.light.has_value()) {
        return std::nullopt;
    }

    auto west = side_x({coord.x - 1, coord.z}, kChunkWidth - kLightBorder);
    auto east = side_x({coord.x + 1, coord.z}, 0);
    auto north = side_z({coord.x, coord.z - 1}, kChunkDepth - kLightBorder);
    auto south = side_z({coord.x, coord.z + 1}, 0);
    auto northwest = corner({coord.x - 1, coord.z - 1}, kChunkWidth - kLightBorder, kChunkDepth - kLightBorder);
    auto northeast = corner({coord.x + 1, coord.z - 1}, 0, kChunkDepth - kLightBorder);
    auto southwest = corner({coord.x - 1, coord.z + 1}, kChunkWidth - kLightBorder, 0);
    auto southeast = corner({coord.x + 1, coord.z + 1}, 0, 0);

    auto light_west = light_side_x({coord.x - 1, coord.z}, kChunkWidth - kLightBorder);
    auto light_east = light_side_x({coord.x + 1, coord.z}, 0);
    auto light_north = light_side_z({coord.x, coord.z - 1}, kChunkDepth - kLightBorder);
    auto light_south = light_side_z({coord.x, coord.z + 1}, 0);
    auto light_northwest = light_corner({coord.x - 1, coord.z - 1}, kChunkWidth - kLightBorder, kChunkDepth - kLightBorder);
    auto light_northeast = light_corner({coord.x + 1, coord.z - 1}, 0, kChunkDepth - kLightBorder);
    auto light_southwest = light_corner({coord.x - 1, coord.z + 1}, kChunkWidth - kLightBorder, 0);
    auto light_southeast = light_corner({coord.x + 1, coord.z + 1}, 0, 0);
    const bool provisional =
        !chunk_it->second.light->borders_ready ||
        !west.has_value() || !east.has_value() || !north.has_value() || !south.has_value() ||
        !northwest.has_value() || !northeast.has_value() || !southwest.has_value() || !southeast.has_value() ||
        !light_west.has_value() || !light_east.has_value() || !light_north.has_value() || !light_south.has_value() ||
        !light_northwest.has_value() || !light_northeast.has_value() || !light_southwest.has_value() || !light_southeast.has_value();

    return ChunkMeshSnapshot {
        chunk_it->second.generation_version,
        leaves_render_mode_,
        *chunk_it->second.data,
        std::move(west),
        std::move(east),
        std::move(north),
        std::move(south),
        std::move(northwest),
        std::move(northeast),
        std::move(southwest),
        std::move(southeast),
        *chunk_it->second.light,
        std::move(light_west),
        std::move(light_east),
        std::move(light_north),
        std::move(light_south),
        std::move(light_northwest),
        std::move(light_northeast),
        std::move(light_southwest),
        std::move(light_southeast),
        provisional
    };
}

ChunkMeshNeighbors WorldStreamer::neighbors_from_snapshot(const ChunkMeshSnapshot& snapshot) {
    return {
        snapshot.west.has_value() ? &*snapshot.west : nullptr,
        snapshot.east.has_value() ? &*snapshot.east : nullptr,
        snapshot.north.has_value() ? &*snapshot.north : nullptr,
        snapshot.south.has_value() ? &*snapshot.south : nullptr,
        snapshot.northwest.has_value() ? &*snapshot.northwest : nullptr,
        snapshot.northeast.has_value() ? &*snapshot.northeast : nullptr,
        snapshot.southwest.has_value() ? &*snapshot.southwest : nullptr,
        snapshot.southeast.has_value() ? &*snapshot.southeast : nullptr
    };
}

LightMeshSnapshot WorldStreamer::light_from_snapshot(const ChunkMeshSnapshot& snapshot) {
    return {
        &snapshot.light,
        snapshot.light_west.has_value() ? &*snapshot.light_west : nullptr,
        snapshot.light_east.has_value() ? &*snapshot.light_east : nullptr,
        snapshot.light_north.has_value() ? &*snapshot.light_north : nullptr,
        snapshot.light_south.has_value() ? &*snapshot.light_south : nullptr,
        snapshot.light_northwest.has_value() ? &*snapshot.light_northwest : nullptr,
        snapshot.light_northeast.has_value() ? &*snapshot.light_northeast : nullptr,
        snapshot.light_southwest.has_value() ? &*snapshot.light_southwest : nullptr,
        snapshot.light_southeast.has_value() ? &*snapshot.light_southeast : nullptr,
        snapshot.provisional
    };
}

}

