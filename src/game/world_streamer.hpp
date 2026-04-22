#pragma once

#include "game/block.hpp"
#include "game/world_generator.hpp"
#include "game/world_types.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ml {

class WorldStreamer {
public:
    WorldStreamer(WorldSeed seed, const BlockRegistry& block_registry, int chunk_radius = 6);
    ~WorldStreamer();

    void update_observer(Vec3 position);
    void tick_generation_jobs();
    std::span<const ActiveChunk> visible_chunks() const;
    std::vector<PendingChunkUpload> drain_pending_uploads();
    BlockQueryResult query_block_at_world(int x, int y, int z) const;
    BlockId block_at_world(int x, int y, int z) const;
    bool is_solid_at_world(int x, int y, int z) const;
    std::optional<BlockHit> raycast(const Vec3& origin, const Vec3& direction, float max_distance) const;
    SetBlockResult set_block_at_world(int x, int y, int z, BlockId block);

private:
    struct ChunkJob {
        ChunkCoord coord {};
    };

    struct JobResult {
        ChunkCoord coord {};
        ChunkData chunk_data {};
        ChunkMesh mesh {};
    };

    enum class ChunkState {
        Requested,
        Visible
    };

    struct ChunkRecord {
        ChunkState state {ChunkState::Requested};
        std::optional<ChunkData> data {};
    };

    void worker_loop();
    ChunkCoord world_to_chunk(Vec3 position) const;
    ChunkCoord world_to_chunk(int world_x, int world_z) const;
    bool desired_chunk(const ChunkCoord& origin, const ChunkCoord& candidate) const;
    void queue_job(ChunkCoord coord);
    void rebuild_chunk_if_loaded(ChunkCoord coord);

    WorldSeed seed_ {0};
    const BlockRegistry& block_registry_;
    WorldGenerator generator_;
    int chunk_radius_ {6};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ChunkJob> job_queue_;
    std::queue<JobResult> completed_;
    bool stop_requested_ {false};
    std::vector<std::thread> workers_;

    std::unordered_map<ChunkCoord, ChunkRecord, ChunkCoordHasher> chunks_;
    std::vector<ActiveChunk> visible_chunks_;
    std::vector<PendingChunkUpload> pending_uploads_;
    ChunkCoord observer_chunk_ {};
    std::size_t logged_ready_chunk_count_ {0};
};

}
