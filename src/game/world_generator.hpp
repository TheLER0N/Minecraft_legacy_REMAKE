#pragma once

#include "game/block.hpp"
#include "game/light.hpp"
#include "game/world_types.hpp"

#include <array>
#include <cstddef>

namespace ml {

enum class LeavesRenderMode : std::uint8_t {
    Fast,
    Fancy
};

class WorldGenerator {
public:
    explicit WorldGenerator(const BlockRegistry& block_registry);
    ChunkData generate_chunk(ChunkCoord coord, WorldSeed seed) const;
    int surface_height_at(int world_x, int world_z, WorldSeed seed) const;

private:
    float sample_height(int world_x, int world_z, WorldSeed seed) const;
    float sample_continentalness(int world_x, int world_z, WorldSeed seed) const;
    float sample_cave_density(int world_x, int world_y, int world_z, WorldSeed seed) const;
    int sample_aquifer_level(int world_x, int world_y, int world_z, WorldSeed seed) const;
    bool should_carve_cave(int world_x, int world_y, int world_z, int surface_y, WorldSeed seed) const;
    void apply_caves_and_aquifers(ChunkData& chunk, ChunkCoord coord, WorldSeed seed) const;
    bool is_water_adjacent_or_submerged_surface(int world_x, int world_z, int surface_y, int water_level, WorldSeed seed) const;
    void apply_shore_gravel_disks(ChunkData& chunk, ChunkCoord coord, int water_level, WorldSeed seed) const;
    void apply_underwater_gravel_bottom(ChunkData& chunk, ChunkCoord coord, int water_level, WorldSeed seed) const;
    void apply_underground_gravel_blobs(ChunkData& chunk, ChunkCoord coord, WorldSeed seed) const;
    void apply_oak_trees(ChunkData& chunk, ChunkCoord coord, WorldSeed seed) const;
    bool can_place_oak_tree(int base_world_x, int base_y, int base_world_z, WorldSeed seed) const;
    void try_place_oak_tree(ChunkData& chunk, ChunkCoord coord, int base_world_x, int base_y, int base_world_z, int trunk_height, WorldSeed seed) const;

    const BlockRegistry& block_registry_;
};

struct ChunkSideBorderX {
    std::array<BlockId, static_cast<std::size_t>(kLightBorder * kChunkDepth * kChunkHeight)> blocks {};

    BlockId get(int strip_x, int y, int z) const {
        return blocks[static_cast<std::size_t>(strip_x + kLightBorder * (z + y * kChunkDepth))];
    }
};

struct ChunkSideBorderZ {
    std::array<BlockId, static_cast<std::size_t>(kChunkWidth * kLightBorder * kChunkHeight)> blocks {};

    BlockId get(int x, int y, int strip_z) const {
        return blocks[static_cast<std::size_t>(x + kChunkWidth * (strip_z + y * kLightBorder))];
    }
};

struct ChunkCornerBorder {
    std::array<BlockId, static_cast<std::size_t>(kLightBorder * kLightBorder * kChunkHeight)> blocks {};

    BlockId get(int strip_x, int y, int strip_z) const {
        return blocks[static_cast<std::size_t>(strip_x + kLightBorder * (strip_z + y * kLightBorder))];
    }
};

struct ChunkMeshNeighbors {
    const ChunkSideBorderX* west {nullptr};
    const ChunkSideBorderX* east {nullptr};
    const ChunkSideBorderZ* north {nullptr};
    const ChunkSideBorderZ* south {nullptr};
    const ChunkCornerBorder* northwest {nullptr};
    const ChunkCornerBorder* northeast {nullptr};
    const ChunkCornerBorder* southwest {nullptr};
    const ChunkCornerBorder* southeast {nullptr};
};

ChunkMesh build_chunk_mesh(
    const ChunkData& chunk_data,
    ChunkCoord coord,
    const BlockRegistry& block_registry,
    LeavesRenderMode leaves_mode = LeavesRenderMode::Fancy
);
ChunkMesh build_chunk_mesh(
    const ChunkData& chunk_data,
    ChunkCoord coord,
    const BlockRegistry& block_registry,
    const ChunkMeshNeighbors& neighbors,
    LeavesRenderMode leaves_mode = LeavesRenderMode::Fancy
);
ChunkMesh build_chunk_mesh(
    const ChunkData& chunk_data,
    ChunkCoord coord,
    const BlockRegistry& block_registry,
    const ChunkMeshNeighbors& neighbors,
    const LightMeshSnapshot& light,
    LeavesRenderMode leaves_mode = LeavesRenderMode::Fancy
);
ChunkMeshPayload build_chunk_section_meshes(
    const ChunkData& chunk_data,
    ChunkCoord coord,
    const BlockRegistry& block_registry,
    const ChunkMeshNeighbors& neighbors,
    const LightMeshSnapshot& light,
    LeavesRenderMode leaves_mode = LeavesRenderMode::Fancy
);

}
