#pragma once

#include "common/math.hpp"
#include "game/block.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ml {

using WorldSeed = std::uint64_t;

constexpr int kChunkWidth = 16;
constexpr int kChunkDepth = 16;
constexpr int kLightBorder = 4;
constexpr int kWorldMinY = -64;
constexpr int kWorldMaxY = 319;
constexpr int kChunkHeight = kWorldMaxY - kWorldMinY + 1;
constexpr int kChunkSectionHeight = 16;
constexpr int kChunkSectionCount = kChunkHeight / kChunkSectionHeight;
constexpr int kSeaLevel = 62;

constexpr bool contains_world_y(int world_y) {
    return world_y >= kWorldMinY && world_y <= kWorldMaxY;
}

constexpr int world_y_to_local_y(int world_y) {
    return world_y - kWorldMinY;
}

constexpr int local_y_to_world_y(int local_y) {
    return local_y + kWorldMinY;
}

struct ChunkCoord {
    int x {0};
    int z {0};

    bool operator==(const ChunkCoord&) const = default;
};

struct ChunkCoordHasher {
    std::size_t operator()(const ChunkCoord& coord) const {
        std::size_t seed = 0;
        seed = hash_combine(seed, coord.x);
        seed = hash_combine(seed, coord.z);
        return seed;
    }
};

struct Vertex {
    Vec3 position {};
    Vec3 color {};
    Vec2 uv {};
    std::uint32_t texture_index {};
};

struct Aabb {
    Vec3 min {};
    Vec3 max {};
};

struct Int3 {
    int x {0};
    int y {0};
    int z {0};

    bool operator==(const Int3&) const = default;
};

struct MeshSection {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;

    bool empty() const {
        return vertices.empty() || indices.empty();
    }
};

struct ChunkMesh {
    MeshSection opaque_mesh;
    MeshSection cutout_mesh;
    MeshSection transparent_mesh;

    bool empty() const {
        return opaque_mesh.empty() && cutout_mesh.empty() && transparent_mesh.empty();
    }
};

struct ChunkSectionVisibility {
    int min_world_y {kWorldMinY};
    int max_world_y {kWorldMinY + kChunkSectionHeight - 1};
    int nearest_surface_y {kSeaLevel};
    bool has_geometry {false};
    bool has_surface_geometry {false};
    bool has_cave_geometry {false};
    bool has_sky_access {false};
    bool solid_roof_above {false};

    // Renderer-side visibility hints. These fields are intentionally advisory:
    // they must not be used to hard-delete visible geometry. The renderer may
    // use them for ordering, diagnostics, draw budgeting, and future occluder
    // selection. Real visibility still belongs to frustum/occlusion checks.
    int render_priority_bias {0};
    std::uint32_t visible_opaque_faces {0};
    std::uint32_t visible_cutout_faces {0};
    std::uint32_t visible_transparent_faces {0};
    bool likely_occluder {false};
    bool near_surface_band {false};
};

struct SectionOcclusionMetadata {
    bool known {false};
    std::uint8_t open_sides {0};
    std::array<std::uint8_t, 6> side_connections {};
};

struct ChunkVisibilityMetadata {
    std::array<ChunkSectionVisibility, kChunkSectionCount> sections {};
    std::array<SectionOcclusionMetadata, kChunkSectionCount> occlusion {};
};

struct ChunkLight {
    std::vector<std::uint8_t> sky_light;
    std::vector<std::uint8_t> block_light;
    bool dirty {true};
    bool borders_ready {false};
    std::uint64_t border_signature {0};

    ChunkLight()
        : sky_light(static_cast<std::size_t>(kChunkWidth * kChunkDepth * kChunkHeight), 0)
        , block_light(static_cast<std::size_t>(kChunkWidth * kChunkDepth * kChunkHeight), 0) {
    }

    static std::size_t index(int x, int y, int z) {
        return static_cast<std::size_t>(x + z * kChunkWidth + y * kChunkWidth * kChunkDepth);
    }

    std::uint8_t sky(int x, int y, int z) const {
        return sky_light[index(x, y, z)];
    }

    void set_sky(int x, int y, int z, std::uint8_t value) {
        sky_light[index(x, y, z)] = value;
    }

    std::uint8_t block(int x, int y, int z) const {
        return block_light[index(x, y, z)];
    }

    void set_block(int x, int y, int z, std::uint8_t value) {
        block_light[index(x, y, z)] = value;
    }
};

struct ChunkData {
    std::vector<BlockId> blocks;

    ChunkData()
        : blocks(static_cast<std::size_t>(kChunkWidth * kChunkDepth * kChunkHeight), BlockId::Air) {
    }

    static std::size_t index(int x, int y, int z) {
        return static_cast<std::size_t>(x + z * kChunkWidth + y * kChunkWidth * kChunkDepth);
    }

    BlockId get(int x, int y, int z) const {
        return blocks[index(x, y, z)];
    }

    void set(int x, int y, int z, BlockId value) {
        blocks[index(x, y, z)] = value;
    }
};

struct ActiveChunk {
    ChunkCoord coord {};
};

struct PendingChunkUpload {
    ChunkCoord coord {};
    std::uint64_t version {0};
    std::uint64_t rebuild_serial {0};
    std::uint64_t upload_token {0};
    bool provisional {false};
    ChunkMesh mesh {};
    ChunkVisibilityMetadata visibility {};
};

struct CameraFrameData {
    Mat4 projection {};
    Mat4 view_rotation {};
    Mat4 view_proj {};
    Vec3 camera_position {};
    Vec3 camera_forward {};
    Vec3 camera_right {};
    Vec3 camera_up {};
};

struct VisibleChunk {
    ChunkCoord coord {};
};

struct BlockHit {
    bool hit {false};
    Int3 block {};
    Int3 normal {};
    Int3 placement_block {};
    float distance {0.0f};
};

enum class BlockQueryStatus : std::uint8_t {
    Loaded,
    Unloaded,
    OutOfBounds
};

struct BlockQueryResult {
    BlockQueryStatus status {BlockQueryStatus::Unloaded};
    BlockId block {BlockId::Air};
};

enum class SetBlockResult : std::uint8_t {
    Success,
    ChunkUnloaded,
    OutOfBounds,
    Occupied,
    NoChange,
    IntersectsPlayer,
    InvalidPlacementHit
};

}
