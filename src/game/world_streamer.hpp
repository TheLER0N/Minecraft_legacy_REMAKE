#pragma once

#include "game/block.hpp"
#include "game/world_generator.hpp"
#include "game/world_types.hpp"

#include <condition_variable>
#include <cstdint>
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
    struct StreamingStats {
        std::size_t visible_chunks {0};
        std::size_t pending_uploads {0};
        std::size_t queued_rebuilds {0};
    };

    WorldStreamer(WorldSeed seed, const BlockRegistry& block_registry, int chunk_radius = 6);
    ~WorldStreamer();

    void update_observer(Vec3 position);
    void tick_generation_jobs();
    std::span<const ActiveChunk> visible_chunks() const;
    std::vector<PendingChunkUpload> drain_pending_uploads();
    std::vector<PendingChunkUpload> drain_pending_uploads(std::size_t max_count, Vec3 observer_position);
    std::vector<ChunkCoord> drain_pending_unloads();
    StreamingStats stats() const;
    BlockQueryResult query_block_at_world(int x, int y, int z) const;
    BlockId block_at_world(int x, int y, int z) const;
    bool is_solid_at_world(int x, int y, int z) const;
    std::optional<BlockHit> raycast(const Vec3& origin, const Vec3& direction, float max_distance) const;
    SetBlockResult set_block_at_world(int x, int y, int z, BlockId block);

private:
    enum class ChunkJobType {
        GenerateChunk,
        RebuildMesh
    };

    struct ChunkMeshSnapshot {
        std::uint64_t version {0};
        ChunkData chunk {};
        std::optional<ChunkData> west {};
        std::optional<ChunkData> east {};
        std::optional<ChunkData> north {};
        std::optional<ChunkData> south {};
    };

    struct ChunkJob {
        ChunkCoord coord {};
        std::uint64_t version {0};
        ChunkJobType type {ChunkJobType::GenerateChunk};
        std::optional<ChunkMeshSnapshot> snapshot {};
    };

    struct JobResult {
        ChunkCoord coord {};
        std::uint64_t version {0};
        ChunkJobType type {ChunkJobType::GenerateChunk};
        std::optional<ChunkData> chunk_data {};
        ChunkMesh mesh {};
    };

    enum class ChunkState {
        Requested,
        Visible
    };

    struct ChunkRecord {
        ChunkState state {ChunkState::Requested};
        std::uint64_t version {0};
        std::optional<ChunkData> data {};
    };

    struct RebuildState {
        bool queued {false};
        bool dirty {false};
    };

    void worker_loop();
    ChunkCoord world_to_chunk(Vec3 position) const;
    ChunkCoord world_to_chunk(int world_x, int world_z) const;
    bool desired_chunk(const ChunkCoord& origin, const ChunkCoord& candidate) const;
    void queue_generate_job(ChunkCoord coord, std::uint64_t version);
    void queue_rebuild_job_if_loaded(ChunkCoord coord);
    void queue_rebuild_job_if_loaded_locked(ChunkCoord coord);
    std::optional<ChunkMeshSnapshot> make_rebuild_snapshot(ChunkCoord coord) const;
    static ChunkMeshNeighbors neighbors_from_snapshot(const ChunkMeshSnapshot& snapshot);

    WorldSeed seed_ {0};
    const BlockRegistry& block_registry_;
    WorldGenerator generator_;
    int chunk_radius_ {6};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<ChunkJob> job_queue_;
    std::queue<JobResult> completed_;
    std::unordered_map<ChunkCoord, RebuildState, ChunkCoordHasher> rebuild_states_;
    bool stop_requested_ {false};
    std::vector<std::thread> workers_;

    std::unordered_map<ChunkCoord, ChunkRecord, ChunkCoordHasher> chunks_;
    std::vector<ActiveChunk> visible_chunks_;
    std::vector<PendingChunkUpload> pending_uploads_;
    std::vector<ChunkCoord> pending_unloads_;
    ChunkCoord observer_chunk_ {};
    std::uint64_t next_chunk_version_ {1};
    std::size_t logged_ready_chunk_count_ {0};
    std::size_t logged_rebuild_lifecycle_count_ {0};
};

}
