#pragma once

#include "game/world_types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace ml {

struct ChunkLightSideBorderX {
    std::array<std::uint8_t, static_cast<std::size_t>(kLightBorder * kChunkDepth * kChunkHeight)> sky {};

    std::uint8_t get(int strip_x, int y, int z) const {
        return sky[static_cast<std::size_t>(strip_x + kLightBorder * (z + y * kChunkDepth))];
    }
};

struct ChunkLightSideBorderZ {
    std::array<std::uint8_t, static_cast<std::size_t>(kChunkWidth * kLightBorder * kChunkHeight)> sky {};

    std::uint8_t get(int x, int y, int strip_z) const {
        return sky[static_cast<std::size_t>(x + kChunkWidth * (strip_z + y * kLightBorder))];
    }
};

struct ChunkLightCornerBorder {
    std::array<std::uint8_t, static_cast<std::size_t>(kLightBorder * kLightBorder * kChunkHeight)> sky {};

    std::uint8_t get(int strip_x, int y, int strip_z) const {
        return sky[static_cast<std::size_t>(strip_x + kLightBorder * (strip_z + y * kLightBorder))];
    }
};

struct LightBlockSideBorderX {
    std::array<BlockId, static_cast<std::size_t>(kLightBorder * kChunkDepth * kChunkHeight)> blocks {};

    BlockId get(int strip_x, int y, int z) const {
        return blocks[static_cast<std::size_t>(strip_x + kLightBorder * (z + y * kChunkDepth))];
    }
};

struct LightBlockSideBorderZ {
    std::array<BlockId, static_cast<std::size_t>(kChunkWidth * kLightBorder * kChunkHeight)> blocks {};

    BlockId get(int x, int y, int strip_z) const {
        return blocks[static_cast<std::size_t>(x + kChunkWidth * (strip_z + y * kLightBorder))];
    }
};

struct LightBlockCornerBorder {
    std::array<BlockId, static_cast<std::size_t>(kLightBorder * kLightBorder * kChunkHeight)> blocks {};

    BlockId get(int strip_x, int y, int strip_z) const {
        return blocks[static_cast<std::size_t>(strip_x + kLightBorder * (strip_z + y * kLightBorder))];
    }
};

struct LightBuildSnapshot {
    ChunkData chunk {};
    std::optional<LightBlockSideBorderX> west {};
    std::optional<LightBlockSideBorderX> east {};
    std::optional<LightBlockSideBorderZ> north {};
    std::optional<LightBlockSideBorderZ> south {};
    std::optional<LightBlockCornerBorder> northwest {};
    std::optional<LightBlockCornerBorder> northeast {};
    std::optional<LightBlockCornerBorder> southwest {};
    std::optional<LightBlockCornerBorder> southeast {};
    bool complete_cardinal_borders {false};
    bool complete_borders {false};
};

struct ChunkLightResult {
    ChunkLight light {};
    bool provisional {true};
};

struct LightMeshSnapshot {
    const ChunkLight* center {nullptr};
    const ChunkLightSideBorderX* west {nullptr};
    const ChunkLightSideBorderX* east {nullptr};
    const ChunkLightSideBorderZ* north {nullptr};
    const ChunkLightSideBorderZ* south {nullptr};
    const ChunkLightCornerBorder* northwest {nullptr};
    const ChunkLightCornerBorder* northeast {nullptr};
    const ChunkLightCornerBorder* southwest {nullptr};
    const ChunkLightCornerBorder* southeast {nullptr};
    bool provisional {true};
};

ChunkLightResult calculate_chunk_light(const LightBuildSnapshot& snapshot, const BlockRegistry& block_registry);

std::uint8_t sample_sky_light(const LightMeshSnapshot& snapshot, int x, int y, int z);

}
