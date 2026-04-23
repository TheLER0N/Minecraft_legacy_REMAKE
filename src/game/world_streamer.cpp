#include "game/world_streamer.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

namespace ml {

namespace {

constexpr float kRaycastEpsilon = 0.0001f;
constexpr float kRaycastTieEpsilon = 0.00001f;

bool nearly_equal(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= kRaycastTieEpsilon;
}

}

WorldStreamer::WorldStreamer(WorldSeed seed, const BlockRegistry& block_registry, int chunk_radius)
    : seed_(seed)
    , block_registry_(block_registry)
    , generator_(block_registry)
    , chunk_radius_(chunk_radius) {
    const std::size_t worker_count = std::max<std::size_t>(2, std::thread::hardware_concurrency() / 2);
    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
        workers_.emplace_back([this]() { worker_loop(); });
    }
}

WorldStreamer::~WorldStreamer() {
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
    const ChunkCoord origin = world_to_chunk(position);
    if (origin == observer_chunk_ && !chunks_.empty()) {
        return;
    }

    observer_chunk_ = origin;

    std::vector<ChunkCoord> wanted;
    wanted.reserve(static_cast<std::size_t>((chunk_radius_ * 2 + 1) * (chunk_radius_ * 2 + 1)));
    for (int dz = -chunk_radius_; dz <= chunk_radius_; ++dz) {
        for (int dx = -chunk_radius_; dx <= chunk_radius_; ++dx) {
            wanted.push_back({origin.x + dx, origin.z + dz});
        }
    }

    for (const ChunkCoord& coord : wanted) {
        if (!chunks_.contains(coord)) {
            ChunkRecord record {};
            record.version = next_chunk_version_++;
            const std::uint64_t version = record.version;
            chunks_.emplace(coord, std::move(record));
            queue_generate_job(coord, version);
        }
    }

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (!desired_chunk(origin, it->first)) {
            pending_unloads_.push_back(it->first);
            rebuild_states_.erase(it->first);
            if (logged_rebuild_lifecycle_count_ < 16) {
                log_message(
                    LogLevel::Info,
                    std::string("WorldStreamer: chunk unloaded coord=(") +
                        std::to_string(it->first.x) + "," + std::to_string(it->first.z) + ")"
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
        if (record.state == ChunkState::Visible) {
            visible_chunks_.push_back({coord});
        }
    }
}

void WorldStreamer::tick_generation_jobs() {
    std::lock_guard lock(mutex_);
    constexpr std::size_t max_results_per_tick = 8;
    std::size_t processed = 0;
    while (!completed_.empty() && processed < max_results_per_tick) {
        JobResult result = std::move(completed_.front());
        completed_.pop();
        ++processed;

        if (result.type == ChunkJobType::RebuildMesh) {
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
        }

        auto it = chunks_.find(result.coord);
        if (it == chunks_.end()) {
            continue;
        }
        if (it->second.version != result.version) {
            continue;
        }

        if (result.type == ChunkJobType::GenerateChunk) {
            if (!result.chunk_data.has_value()) {
                continue;
            }
            it->second.state = ChunkState::Visible;
            it->second.data = std::move(result.chunk_data);
            visible_chunks_.push_back({result.coord});

            queue_rebuild_job_if_loaded_locked(result.coord);
            queue_rebuild_job_if_loaded_locked({result.coord.x - 1, result.coord.z});
            queue_rebuild_job_if_loaded_locked({result.coord.x + 1, result.coord.z});
            queue_rebuild_job_if_loaded_locked({result.coord.x, result.coord.z - 1});
            queue_rebuild_job_if_loaded_locked({result.coord.x, result.coord.z + 1});
            continue;
        }

        if (result.mesh.vertices.empty() || result.mesh.indices.empty()) {
            log_message(LogLevel::Warning, "WorldStreamer: rebuilt chunk mesh is empty");
        } else if (logged_ready_chunk_count_ < 8) {
            log_message(
                LogLevel::Info,
                std::string("WorldStreamer: chunk ready vertices=") + std::to_string(result.mesh.vertices.size()) +
                    " indices=" + std::to_string(result.mesh.indices.size())
            );
            ++logged_ready_chunk_count_;
        }
        pending_uploads_.push_back({result.coord, std::move(result.mesh)});
    }
}

std::span<const ActiveChunk> WorldStreamer::visible_chunks() const {
    return visible_chunks_;
}

std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads() {
    std::vector<PendingChunkUpload> uploads;
    uploads.swap(pending_uploads_);
    return uploads;
}

std::vector<PendingChunkUpload> WorldStreamer::drain_pending_uploads(std::size_t max_count, Vec3 observer_position) {
    if (max_count == 0 || pending_uploads_.empty()) {
        return {};
    }

    const auto distance_to_observer = [observer_position](const PendingChunkUpload& upload) {
        const float center_x = static_cast<float>(upload.coord.x * kChunkWidth) + static_cast<float>(kChunkWidth) * 0.5f;
        const float center_z = static_cast<float>(upload.coord.z * kChunkDepth) + static_cast<float>(kChunkDepth) * 0.5f;
        const float dx = center_x - observer_position.x;
        const float dz = center_z - observer_position.z;
        return dx * dx + dz * dz;
    };

    std::sort(
        pending_uploads_.begin(),
        pending_uploads_.end(),
        [&](const PendingChunkUpload& lhs, const PendingChunkUpload& rhs) {
            return distance_to_observer(lhs) < distance_to_observer(rhs);
        }
    );

    const std::size_t upload_count = std::min(max_count, pending_uploads_.size());
    std::vector<PendingChunkUpload> uploads;
    uploads.reserve(upload_count);
    for (std::size_t i = 0; i < upload_count; ++i) {
        uploads.push_back(std::move(pending_uploads_[i]));
    }
    pending_uploads_.erase(pending_uploads_.begin(), pending_uploads_.begin() + static_cast<std::ptrdiff_t>(upload_count));
    return uploads;
}

std::vector<ChunkCoord> WorldStreamer::drain_pending_unloads() {
    std::vector<ChunkCoord> unloads;
    unloads.swap(pending_unloads_);
    return unloads;
}

WorldStreamer::StreamingStats WorldStreamer::stats() const {
    std::size_t queued_rebuilds = 0;
    for (const auto& [coord, state] : rebuild_states_) {
        (void)coord;
        if (state.queued) {
            ++queued_rebuilds;
        }
    }

    return {
        visible_chunks_.size(),
        pending_uploads_.size(),
        queued_rebuilds
    };
}

BlockQueryResult WorldStreamer::query_block_at_world(int x, int y, int z) const {
    if (y < 0 || y >= kChunkHeight) {
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
        chunk_it->second.data->get(positive_mod(x, kChunkWidth), y, positive_mod(z, kChunkDepth))
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
    if (y < 0 || y >= kChunkHeight) {
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
    const int local_z = positive_mod(z, kChunkDepth);
    const BlockId existing = chunk.get(local_x, y, local_z);
    if (existing == block) {
        return SetBlockResult::NoChange;
    }
    if (block != BlockId::Air && !block_registry_.is_replaceable(existing)) {
        return SetBlockResult::Occupied;
    }

    chunk.set(local_x, y, local_z, block);
    queue_rebuild_job_if_loaded(chunk_coord);
    if (local_x == 0) {
        queue_rebuild_job_if_loaded({chunk_coord.x - 1, chunk_coord.z});
    }
    if (local_x == kChunkWidth - 1) {
        queue_rebuild_job_if_loaded({chunk_coord.x + 1, chunk_coord.z});
    }
    if (local_z == 0) {
        queue_rebuild_job_if_loaded({chunk_coord.x, chunk_coord.z - 1});
    }
    if (local_z == kChunkDepth - 1) {
        queue_rebuild_job_if_loaded({chunk_coord.x, chunk_coord.z + 1});
    }

    return SetBlockResult::Success;
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

            job = job_queue_.front();
            job_queue_.pop();
        }

        JobResult result {};
        result.coord = job.coord;
        result.version = job.version;
        result.type = job.type;

        if (job.type == ChunkJobType::GenerateChunk) {
            result.chunk_data = generator_.generate_chunk(job.coord, seed_);
        } else if (job.snapshot.has_value()) {
            const ChunkMeshSnapshot& snapshot = *job.snapshot;
            result.mesh = build_chunk_mesh(snapshot.chunk, job.coord, block_registry_, neighbors_from_snapshot(snapshot));
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

void WorldStreamer::queue_generate_job(ChunkCoord coord, std::uint64_t version) {
    {
        std::lock_guard lock(mutex_);
        job_queue_.push({coord, version, ChunkJobType::GenerateChunk, std::nullopt});
    }
    cv_.notify_one();
}

void WorldStreamer::queue_rebuild_job_if_loaded(ChunkCoord coord) {
    std::lock_guard lock(mutex_);
    queue_rebuild_job_if_loaded_locked(coord);
}

void WorldStreamer::queue_rebuild_job_if_loaded_locked(ChunkCoord coord) {
    auto state_it = rebuild_states_.find(coord);
    if (state_it != rebuild_states_.end() && state_it->second.queued) {
        state_it->second.dirty = true;
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
    if (logged_rebuild_lifecycle_count_ < 16) {
        log_message(
            LogLevel::Info,
            std::string("WorldStreamer: rebuild queued coord=(") +
                std::to_string(coord.x) + "," + std::to_string(coord.z) + ")"
        );
        ++logged_rebuild_lifecycle_count_;
    }
    job_queue_.push({coord, snapshot->version, ChunkJobType::RebuildMesh, std::move(snapshot)});
    cv_.notify_one();
}

std::optional<WorldStreamer::ChunkMeshSnapshot> WorldStreamer::make_rebuild_snapshot(ChunkCoord coord) const {
    const auto chunk_data = [this](ChunkCoord neighbor_coord) -> std::optional<ChunkData> {
        const auto it = chunks_.find(neighbor_coord);
        if (it == chunks_.end() || !it->second.data.has_value()) {
            return std::nullopt;
        }
        return *it->second.data;
    };

    const auto chunk_it = chunks_.find(coord);
    if (chunk_it == chunks_.end() || !chunk_it->second.data.has_value()) {
        return std::nullopt;
    }

    return ChunkMeshSnapshot {
        chunk_it->second.version,
        *chunk_it->second.data,
        chunk_data({coord.x - 1, coord.z}),
        chunk_data({coord.x + 1, coord.z}),
        chunk_data({coord.x, coord.z - 1}),
        chunk_data({coord.x, coord.z + 1})
    };
}

ChunkMeshNeighbors WorldStreamer::neighbors_from_snapshot(const ChunkMeshSnapshot& snapshot) {
    return {
        snapshot.west.has_value() ? &*snapshot.west : nullptr,
        snapshot.east.has_value() ? &*snapshot.east : nullptr,
        snapshot.north.has_value() ? &*snapshot.north : nullptr,
        snapshot.south.has_value() ? &*snapshot.south : nullptr
    };
}

}
