#pragma once

#include "game/block.hpp"
#include "game/world_generator.hpp"
#include "game/world_save.hpp"
#include "game/world_types.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <span>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ml {

class WorldStreamer {
public:
    struct StreamingStats {
        std::size_t visible_chunks {0};
        std::size_t pending_uploads {0};
        std::size_t queued_rebuilds {0};
        std::size_t queued_generates {0};
        std::size_t queued_decorates {0};
        std::size_t queued_lights {0};
        std::size_t queued_meshes {0};
        std::size_t queued_fast_meshes {0};
        std::size_t queued_final_meshes {0};
        std::size_t pending_upload_bytes {0};
        std::size_t stale_results {0};
        std::size_t stale_uploads {0};
        std::size_t provisional_uploads {0};
        std::size_t light_stale_results {0};
        std::size_t edge_fixups {0};
        std::size_t dropped_jobs {0};
        std::size_t dirty_save_chunks {0};
        bool observer_light_borders_ready {false};
        int observer_light_border_status {0};
        float last_generate_ms {0.0f};
        float last_light_ms {0.0f};
        float last_mesh_ms {0.0f};
    };

    WorldStreamer(WorldSeed seed, const BlockRegistry& block_registry, int chunk_radius = 6);
    WorldStreamer(WorldSeed seed, const BlockRegistry& block_registry, int chunk_radius, WorldSave* world_save);
    ~WorldStreamer();

    void update_observer(Vec3 position);
    void update_observer(Vec3 position, Vec3 forward);
    void tick_generation_jobs();
    std::span<const ActiveChunk> visible_chunks() const;
    std::vector<PendingChunkUpload> drain_pending_uploads();
    std::vector<PendingChunkUpload> drain_pending_uploads(std::size_t max_count, Vec3 observer_position);
    std::vector<PendingChunkUpload> drain_pending_uploads(std::size_t max_count, Vec3 observer_position, Vec3 observer_forward);
    std::vector<PendingChunkUpload> drain_pending_uploads_by_budget(
        std::size_t byte_budget,
        std::size_t max_count,
        Vec3 observer_position,
        Vec3 observer_forward
    );    std::vector<ChunkCoord> drain_pending_unloads();
    StreamingStats stats() const;
    BlockQueryResult query_block_at_world(int x, int y, int z) const;
    BlockId block_at_world(int x, int y, int z) const;
    bool is_solid_at_world(int x, int y, int z) const;
    std::optional<BlockHit> raycast(const Vec3& origin, const Vec3& direction, float max_distance) const;
    SetBlockResult set_block_at_world(int x, int y, int z, BlockId block);
    void set_leaves_render_mode(LeavesRenderMode mode);
    LeavesRenderMode leaves_render_mode() const;
    int chunk_radius() const;
    void set_chunk_radius(int radius);
    void flush_dirty_chunks(std::size_t max_chunks);
    void flush_all_dirty_chunks();

private:
    enum class ChunkJobType {
        GenerateTerrain,
        Decorate,
        CalculateLight,
        BuildMesh
    };

    struct ChunkMeshSnapshot {
        std::uint64_t version {0};
        LeavesRenderMode leaves_mode {LeavesRenderMode::Fancy};
        ChunkData chunk {};
        std::optional<ChunkSideBorderX> west {};
        std::optional<ChunkSideBorderX> east {};
        std::optional<ChunkSideBorderZ> north {};
        std::optional<ChunkSideBorderZ> south {};
        std::optional<ChunkCornerBorder> northwest {};
        std::optional<ChunkCornerBorder> northeast {};
        std::optional<ChunkCornerBorder> southwest {};
        std::optional<ChunkCornerBorder> southeast {};
        ChunkLight light {};
        std::optional<ChunkLightSideBorderX> light_west {};
        std::optional<ChunkLightSideBorderX> light_east {};
        std::optional<ChunkLightSideBorderZ> light_north {};
        std::optional<ChunkLightSideBorderZ> light_south {};
        std::optional<ChunkLightCornerBorder> light_northwest {};
        std::optional<ChunkLightCornerBorder> light_northeast {};
        std::optional<ChunkLightCornerBorder> light_southwest {};
        std::optional<ChunkLightCornerBorder> light_southeast {};
        bool provisional {true};
    };

    struct ChunkJob {
        ChunkCoord coord {};
        std::uint64_t version {0};
        std::uint64_t light_job_token {0};
        std::uint64_t rebuild_serial {0};
        ChunkJobType type {ChunkJobType::GenerateTerrain};
        std::optional<ChunkData> chunk_data {};
        std::shared_ptr<ChunkMeshSnapshot> snapshot {};
        std::shared_ptr<LightBuildSnapshot> light_snapshot {};
    };

    struct JobResult {
        ChunkCoord coord {};
        std::uint64_t version {0};
        std::uint64_t light_job_token {0};
        std::uint64_t rebuild_serial {0};
        ChunkJobType type {ChunkJobType::GenerateTerrain};
        bool stale_rebuild {false};
        float generate_ms {0.0f};
        float light_ms {0.0f};
        float mesh_ms {0.0f};
        std::optional<ChunkData> chunk_data {};
        std::optional<ChunkLight> light {};
        ChunkMesh mesh {};
        std::uint64_t border_signature {0};
        bool borders_ready {false};
        bool provisional {false};
    };

    enum class ChunkState {
        Unloaded,
        Requested,
        TerrainGenerated,
        Decorated,
        WaitingForNeighbors,
        VerticalSkyReady,
        LightPropagating,
        EdgeCheckPending,
        LightReady,
        LightCalculated,
        MeshQueued,
        MeshBuilt,
        UploadQueued,
        UploadedToGPU,
        Visible
    };

    struct ChunkRecord {
        ChunkState state {ChunkState::Requested};
        std::uint64_t generation_version {0};
        std::uint64_t mesh_version {0};
        std::uint64_t latest_light_job_token {0};
        std::uint64_t latest_rebuild_serial {0};
        std::uint64_t latest_upload_token {0};
        std::uint64_t border_signature {0};
        std::uint64_t last_touched_frame {0};
        bool uploaded_to_gpu {false};
        bool dirty_mesh {false};
        bool dirty_light {false};
        bool needs_final_light {false};
        bool provisional_mesh {false};
        bool dirty_save {false};
        std::optional<ChunkData> data {};
        std::optional<ChunkLight> light {};
    };

    struct RebuildState {
        bool queued {false};
        bool dirty {false};
        std::uint64_t serial {0};
    };

    void worker_loop();
    ChunkCoord world_to_chunk(Vec3 position) const;
    ChunkCoord world_to_chunk(int world_x, int world_z) const;
    bool desired_chunk(const ChunkCoord& origin, const ChunkCoord& candidate) const;
    float chunk_priority_score(ChunkCoord coord, Vec3 observer_position, Vec3 observer_forward) const;
    float job_priority_score_locked(const ChunkJob& job) const;
    void push_job_locked(ChunkJob&& job);
    void queue_generate_job(ChunkCoord coord, std::uint64_t version);
    void queue_stage_job_locked(ChunkCoord coord, std::uint64_t version, ChunkJobType type, std::optional<ChunkData>&& chunk_data);
    void queue_light_job_if_loaded(ChunkCoord coord);
    void queue_light_job_if_loaded_locked(ChunkCoord coord);
    void queue_light_self_and_neighbors_if_loaded_locked(ChunkCoord coord, bool include_diagonals);
    void queue_rebuild_job_if_loaded(ChunkCoord coord);
    void queue_rebuild_job_if_loaded_locked(ChunkCoord coord);
    void queue_rebuild_self_and_neighbors_if_loaded_locked(ChunkCoord coord, bool include_diagonals);
    void record_stale_upload_drop(ChunkCoord coord);
    void mark_chunk_dirty_for_save(ChunkCoord coord);
    void enqueue_dirty_save(ChunkCoord coord);
    std::optional<LightBuildSnapshot> make_light_build_snapshot(ChunkCoord coord, const ChunkData& chunk) const;
    std::optional<ChunkMeshSnapshot> make_rebuild_snapshot(ChunkCoord coord) const;
    static ChunkMeshNeighbors neighbors_from_snapshot(const ChunkMeshSnapshot& snapshot);
    static LightMeshSnapshot light_from_snapshot(const ChunkMeshSnapshot& snapshot);

    WorldSeed seed_ {0};
    WorldSave* world_save_ {nullptr};
    const BlockRegistry& block_registry_;
    WorldGenerator generator_;
    int chunk_radius_ {6};
    LeavesRenderMode leaves_render_mode_ {LeavesRenderMode::Fancy};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ChunkJob> job_queue_;
    std::unordered_set<ChunkCoord, ChunkCoordHasher> queued_light_jobs_;
    std::queue<JobResult> completed_;
    std::unordered_map<ChunkCoord, RebuildState, ChunkCoordHasher> rebuild_states_;
    bool stop_requested_ {false};
    std::vector<std::thread> workers_;

    std::unordered_map<ChunkCoord, ChunkRecord, ChunkCoordHasher> chunks_;
    std::vector<ActiveChunk> visible_chunks_;
    std::vector<PendingChunkUpload> pending_uploads_;
    std::vector<ChunkCoord> pending_unloads_;
    ChunkCoord observer_chunk_ {};
    Vec3 observer_position_ {};
    Vec3 observer_forward_ {0.0f, 0.0f, -1.0f};
    std::uint64_t next_chunk_version_ {1};
    std::uint64_t next_light_job_token_ {1};
    std::uint64_t next_rebuild_serial_ {1};
    std::uint64_t next_upload_token_ {1};
    std::uint64_t frame_counter_ {0};
    std::size_t stale_results_ {0};
    std::size_t stale_uploads_dropped_ {0};
    std::size_t light_stale_results_ {0};
    std::size_t edge_fixups_ {0};
    std::size_t dropped_jobs_ {0};
    std::size_t logged_ready_chunk_count_ {0};
    std::size_t logged_rebuild_lifecycle_count_ {0};
    std::size_t logged_stale_upload_count_ {0};
    float last_generate_ms_ {0.0f};
    float last_light_ms_ {0.0f};
    float last_mesh_ms_ {0.0f};
    std::deque<ChunkCoord> dirty_save_queue_;
    std::unordered_set<ChunkCoord, ChunkCoordHasher> dirty_save_set_;
};

}
