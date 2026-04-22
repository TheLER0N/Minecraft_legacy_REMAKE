#pragma once

#include "common/math.hpp"
#include "game/block.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ml {

using WorldSeed = std::uint64_t;

constexpr int kChunkWidth = 16;
constexpr int kChunkDepth = 16;
constexpr int kChunkHeight = 128;

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

struct ChunkMesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
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
    ChunkMesh mesh {};
};

struct CameraFrameData {
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
