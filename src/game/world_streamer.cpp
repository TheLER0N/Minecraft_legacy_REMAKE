#include "game/world_streamer.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace ml {

namespace {

constexpr float kRaycastEpsilon = 0.0001f;
constexpr float kRaycastTieEpsilon = 0.00001f;
constexpr int kMinChunkRadius = 2;
constexpr int kMaxChunkRadius = 100;
constexpr std::size_t kMaxNewChunkRequestsPerFrame = 2;
constexpr std::size_t kMaxResultsPerTick = 16;
constexpr std::size_t kMaxDirtyChunkSavesPerTick = 1;

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

WorldStreamer::WorldStreamer(WorldSeed seed, const BlockRegistry& block_registry, int chunk_radius, WorldSave* world_save)
    : seed_(seed)
    , world_save_(world_save)
    , block_registry_(block_registry)
    , generator_(block_registry)
    , chunk_radius_(std::clamp(chunk_radius, kMinChunkRadius, kMaxChunkRadius)) {
    const std::size_t hardware_threads = std::max<std::size_t>(2, std::thread::hardware_concurrency());
    const std::size_t worker_count = std::clamp<std::size_t>(hardware_threads - 1, 2, 12);
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
    update_observer(position, observer_forward_);
}

void WorldStreamer::update_observer(Vec3 position, Vec3 forward) {
    const ChunkCoord origin = world_to_chunk(position);
    ++frame_counter_;
    observer_position_ = position;
    observer_forward_ = normalize({forward.x, 0.0f, forward.z});
    if (length(observer_forward_) <= 0.00001f) {
        observer_forward_ = {0.0f, 0.0f, -1.0f};
    }

    observer_chunk_ = origin;

    std::vector<ChunkCoord> wanted;
    wanted.reserve(static_cast<std::size_t>((chunk_radius_ * 2 + 1) * (chunk_radius_ * 2 + 1)));
    for (int dz = -chunk_radius_; dz <= chunk_radius_; ++dz) {
        for (int dx = -chunk_radius_; dx <= chunk_radius_; ++dx) {
            wanted.push_back({origin.x + dx, origin.z + dz});
        }
    }
    std::sort(
        wanted.begin(),
        wanted.end(),
        [&](const ChunkCoord& lhs, const ChunkCoord& rhs) {
            return chunk_priority_score(lhs, observer_position_, observer_forward_) <
                chunk_priority_score(rhs, observer_position_, observer_forward_);
        }
    );

    std::size_t requested_this_frame = 0;
    for (const ChunkCoord& coord : wanted) {
        if (requested_this_frame >= kMaxNewChunkRequestsPerFrame) {
            break;
        }
        if (!chunks_.contains(coord)) {
            ChunkRecord record {};
            record.generation_version = next_chunk_version_++;
            record.mesh_version = record.generation_version;
            record.last_touched_frame = frame_counter_;
            const std::uint64_t version = record.generation_version;
            chunks_.emplace(coord, std::move(record));
            queue_generate_job(coord, version);
            ++requested_this_frame;
        } else {
            chunks_[coord].last_touched_frame = frame_counter_;
        }
    }

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (!desired_chunk(origin, it->first)) {
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

    visible_chunks_.clear();
    for (const auto& [coord, record] : chunks_) {
        if (record.uploaded_to_gpu) {
            visible_chunks_.push_back({coord});
        }
    }

    flush_dirty_chunks(kMaxDirtyChunkSavesPerTick);
}

void WorldStreamer::tick_generation_jobs() {
    std::lock_guard lock(mutex_);
    std::size_t processed = 0;
    while (!completed_.empty() && processed < kMaxResultsPerTick) {
        JobResult result = std::move(completed_.front());
        completed_.pop();
        ++processed;

        if (result.type == ChunkJobType::CalculateLight) {
            queued_light_jobs_.erase(result.coord);
        }

        auto it = chunks_.find(result.coord);
        if (it == chunks_.end() || it->second.generation_version != result.version || !desired_chunk(observer_chunk_, result.coord)) {
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
            pending_uploads_.push_back({result.coord, result.rebuild_serial, result.provisional, std::move(result.mesh), visibility});
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
            queue_stage_job_locked(result.coord, result.version, ChunkJobType::CalculateLight, std::move(result.chunk_data));
            const std::array<ChunkCoord, 4> cardinal_neighbors {
                ChunkCoord {result.coord.x - 1, result.coord.z},
                ChunkCoord {result.coord.x + 1, result.coord.z},
                ChunkCoord {result.coord.x, result.coord.z - 1},
                ChunkCoord {result.coord.x, result.coord.z + 1}
            };
            for (const ChunkCoord neighbor : cardinal_neighbors) {
                const auto neighbor_it = chunks_.find(neighbor);
                if (neighbor_it != chunks_.end() &&
                    neighbor_it->second.data.has_value() &&
                    (!neighbor_it->second.light.has_value() || !neighbor_it->second.light->borders_ready)) {
                    queue_light_job_if_loaded_locked(neighbor);
                }
            }
            continue;
        }

        if (result.type == ChunkJobType::CalculateLight) {
            if (!result.light.has_value()) {
                ++light_stale_results_;
                continue;
            }
            last_light_ms_ = result.light_ms;
            it->second.state = result.provisional ? ChunkState::WaitingForNeighbors : ChunkState::LightReady;
            it->second.data = std::move(result.chunk_data);
            it->second.light = std::move(result.light);
            it->second.dirty_light = false;
            it->second.dirty_mesh = true;
            const auto visible_it = std::find_if(
                visible_chunks_.begin(),
                visible_chunks_.end(),
                [&](const ActiveChunk& active) {
                    return active.coord == result.coord;
                }
            );
            if (visible_it == visible_chunks_.end()) {
                visible_chunks_.push_back({result.coord});
            }

            queue_rebuild_job_if_loaded_locked(result.coord);
            continue;
        }

    }
}

std::span<const ActiveChunk> WorldStreamer::visible_chunks() const {
    return visible_chunks_;
}

std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads() {
    std::vector<PendingChunkUpload> uploads;
    uploads.reserve(pending_uploads_.size());
    for (PendingChunkUpload& upload : pending_uploads_) {
        const auto it = chunks_.find(upload.coord);
        if (it == chunks_.end() || it->second.latest_rebuild_serial != upload.rebuild_serial) {
            record_stale_upload_drop(upload.coord);
            continue;
        }
        it->second.state = ChunkState::UploadedToGPU;
        it->second.uploaded_to_gpu = true;
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
        if (it == chunks_.end() || it->second.latest_rebuild_serial != pending.rebuild_serial) {
            record_stale_upload_drop(pending.coord);
            ++consumed_count;
            continue;
        }
        it->second.state = ChunkState::UploadedToGPU;
        it->second.uploaded_to_gpu = true;
        uploads.push_back(std::move(pending));
        ++consumed_count;
    }
    pending_uploads_.erase(pending_uploads_.begin(), pending_uploads_.begin() + static_cast<std::ptrdiff_t>(consumed_count));
    return uploads;
}

std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads_by_budget(std::size_t byte_budget, Vec3 observer_position, Vec3 observer_forward) {
    if (byte_budget == 0 || pending_uploads_.empty()) {
        return {};
    }

    const Vec3 planar_forward = normalize({observer_forward.x, 0.0f, observer_forward.z});
    std::sort(
        pending_uploads_.begin(),
        pending_uploads_.end(),
        [&](const PendingChunkUpload& lhs, const PendingChunkUpload& rhs) {
            return chunk_priority_score(lhs.coord, observer_position, planar_forward) <
                chunk_priority_score(rhs.coord, observer_position, planar_forward);
        }
    );

    std::vector<PendingChunkUpload> uploads;
    std::size_t selected_bytes = 0;
    std::size_t selected_count = 0;
    for (; selected_count < pending_uploads_.size(); ++selected_count) {
        PendingChunkUpload& pending = pending_uploads_[selected_count];
        auto chunk_it = chunks_.find(pending.coord);
        if (chunk_it == chunks_.end() || chunk_it->second.latest_rebuild_serial != pending.rebuild_serial) {
            record_stale_upload_drop(pending.coord);
            continue;
        }
        const std::size_t upload_bytes = mesh_byte_count(pending.mesh);
        if (!uploads.empty() && selected_bytes + upload_bytes > byte_budget) {
            break;
        }
        selected_bytes += upload_bytes;
        chunk_it->second.state = ChunkState::UploadedToGPU;
        chunk_it->second.uploaded_to_gpu = true;
        uploads.push_back(std::move(pending));
    }
    pending_uploads_.erase(pending_uploads_.begin(), pending_uploads_.begin() + static_cast<std::ptrdiff_t>(selected_count));
    return uploads;
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
        const auto has_light = [this](ChunkCoord coord) {
            const auto it = chunks_.find(coord);
            return it != chunks_.end() && it->second.light.has_value();
        };
        const bool cardinal_ready = std::all_of(cardinal_neighbors.begin(), cardinal_neighbors.end(), has_light);
        const bool diagonal_ready = std::all_of(diagonal_neighbors.begin(), diagonal_neighbors.end(), has_light);
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
    const bool light_affecting_change =
        block_registry_.light_dampening(existing) != block_registry_.light_dampening(block) ||
        block_registry_.light_emission(existing) != block_registry_.light_emission(block);
    if (light_affecting_change) {
        chunk_it->second.dirty_light = true;
        chunk_it->second.state = ChunkState::LightPropagating;
    }
    chunk_it->second.dirty_mesh = true;
    chunk_it->second.mesh_version = next_chunk_version_++;
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
        result.rebuild_serial = job.rebuild_serial;
        result.type = job.type;

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
                result.light = std::move(light_result.light);
                result.chunk_data = std::move(job.light_snapshot->chunk);
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
    const float center_x = static_cast<float>(coord.x * kChunkWidth) + static_cast<float>(kChunkWidth) * 0.5f;
    const float center_z = static_cast<float>(coord.z * kChunkDepth) + static_cast<float>(kChunkDepth) * 0.5f;
    const Vec3 to_chunk {
        center_x - observer_position.x,
        0.0f,
        center_z - observer_position.z
    };
    const float distance_sq = dot(to_chunk, to_chunk);
    const Vec3 planar_forward = normalize({observer_forward.x, 0.0f, observer_forward.z});
    if (length(planar_forward) <= 0.00001f || distance_sq <= 0.00001f) {
        return distance_sq;
    }

    const float facing = dot(normalize(to_chunk), planar_forward);
    const float bucket = facing > 0.25f ? 0.0f : (facing > -0.25f ? 1.0f : 2.0f);
    return bucket * 1000000.0f + distance_sq;
}

float WorldStreamer::job_priority_score_locked(const ChunkJob& job) const {
    float score = chunk_priority_score(job.coord, observer_position_, observer_forward_);
    if (job.type == ChunkJobType::Decorate) {
        score += 50000.0f;
    } else if (job.type == ChunkJobType::CalculateLight) {
        score += 100000.0f;
    } else if (job.type == ChunkJobType::BuildMesh) {
        score += 150000.0f;
    }
    return score;
}

void WorldStreamer::push_job_locked(ChunkJob&& job) {
    const float score = job_priority_score_locked(job);
    const auto insert_at = std::find_if(
        job_queue_.begin(),
        job_queue_.end(),
        [&](const ChunkJob& existing) {
            return score < job_priority_score_locked(existing);
        }
    );
    job_queue_.insert(insert_at, std::move(job));
}

void WorldStreamer::queue_generate_job(ChunkCoord coord, std::uint64_t version) {
    {
        std::lock_guard lock(mutex_);
        push_job_locked({coord, version, 0, ChunkJobType::GenerateTerrain, std::nullopt, nullptr, nullptr});
    }
    cv_.notify_one();
}

void WorldStreamer::queue_stage_job_locked(ChunkCoord coord, std::uint64_t version, ChunkJobType type, std::optional<ChunkData>&& chunk_data) {
    if (type == ChunkJobType::CalculateLight && chunk_data.has_value()) {
        if (queued_light_jobs_.contains(coord)) {
            return;
        }
        std::optional<LightBuildSnapshot> light_snapshot = make_light_build_snapshot(coord, *chunk_data);
        if (light_snapshot.has_value()) {
            queued_light_jobs_.insert(coord);
            push_job_locked({
                coord,
                version,
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
    push_job_locked({coord, version, 0, type, std::move(chunk_data), nullptr, nullptr});
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
        return;
    }
    it->second.dirty_light = true;
    it->second.state = ChunkState::LightPropagating;
    std::optional<ChunkData> chunk_copy {*it->second.data};
    queue_stage_job_locked(coord, it->second.generation_version, ChunkJobType::CalculateLight, std::move(chunk_copy));
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
        !west.has_value() || !east.has_value() || !north.has_value() || !south.has_value() ||
        !light_west.has_value() || !light_east.has_value() || !light_north.has_value() || !light_south.has_value();

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
