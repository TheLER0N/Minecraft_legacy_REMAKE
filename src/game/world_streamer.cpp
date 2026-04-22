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
            chunks_.emplace(coord, ChunkRecord {});
            queue_job(coord);
        }
    }

    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (!desired_chunk(origin, it->first)) {
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
    while (!completed_.empty()) {
        JobResult result = std::move(completed_.front());
        completed_.pop();

        auto it = chunks_.find(result.coord);
        if (it == chunks_.end()) {
            continue;
        }

        it->second.state = ChunkState::Visible;
        it->second.data = std::move(result.chunk_data);
        visible_chunks_.push_back({result.coord});
        if (result.mesh.vertices.empty() || result.mesh.indices.empty()) {
            log_message(LogLevel::Warning, "WorldStreamer: completed chunk mesh is empty");
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
    rebuild_chunk_if_loaded(chunk_coord);
    if (local_x == 0) {
        rebuild_chunk_if_loaded({chunk_coord.x - 1, chunk_coord.z});
    }
    if (local_x == kChunkWidth - 1) {
        rebuild_chunk_if_loaded({chunk_coord.x + 1, chunk_coord.z});
    }
    if (local_z == 0) {
        rebuild_chunk_if_loaded({chunk_coord.x, chunk_coord.z - 1});
    }
    if (local_z == kChunkDepth - 1) {
        rebuild_chunk_if_loaded({chunk_coord.x, chunk_coord.z + 1});
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

        ChunkData chunk = generator_.generate_chunk(job.coord, seed_);
        ChunkMesh mesh = build_chunk_mesh(chunk, job.coord, block_registry_);

        {
            std::lock_guard lock(mutex_);
            completed_.push({job.coord, std::move(chunk), std::move(mesh)});
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

void WorldStreamer::queue_job(ChunkCoord coord) {
    {
        std::lock_guard lock(mutex_);
        job_queue_.push({coord});
    }
    cv_.notify_one();
}

void WorldStreamer::rebuild_chunk_if_loaded(ChunkCoord coord) {
    auto chunk_it = chunks_.find(coord);
    if (chunk_it == chunks_.end() || !chunk_it->second.data.has_value()) {
        return;
    }

    ChunkMesh mesh = build_chunk_mesh(*chunk_it->second.data, coord, block_registry_);
    pending_uploads_.push_back({coord, std::move(mesh)});
}

}
