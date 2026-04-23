#pragma once

#include "game/block.hpp"
#include "game/world_types.hpp"

namespace ml {

class WorldGenerator {
public:
    explicit WorldGenerator(const BlockRegistry& block_registry);
    ChunkData generate_chunk(ChunkCoord coord, WorldSeed seed) const;

private:
    float sample_height(int world_x, int world_z, WorldSeed seed) const;
    bool is_water_adjacent_or_submerged_surface(int world_x, int world_z, int surface_y, int water_level, WorldSeed seed) const;
    void apply_shore_gravel_disks(ChunkData& chunk, ChunkCoord coord, int water_level, WorldSeed seed) const;
    void apply_underwater_gravel_bottom(ChunkData& chunk, ChunkCoord coord, int water_level, WorldSeed seed) const;
    void apply_underground_gravel_blobs(ChunkData& chunk, ChunkCoord coord, WorldSeed seed) const;
    void apply_oak_trees(ChunkData& chunk, ChunkCoord coord, WorldSeed seed) const;
    bool can_place_oak_tree(int base_world_x, int base_y, int base_world_z, WorldSeed seed) const;
    void try_place_oak_tree(ChunkData& chunk, ChunkCoord coord, int base_world_x, int base_y, int base_world_z, int trunk_height, WorldSeed seed) const;

    const BlockRegistry& block_registry_;
};

struct ChunkMeshNeighbors {
    const ChunkData* west {nullptr};
    const ChunkData* east {nullptr};
    const ChunkData* north {nullptr};
    const ChunkData* south {nullptr};
};

ChunkMesh build_chunk_mesh(const ChunkData& chunk_data, ChunkCoord coord, const BlockRegistry& block_registry);
ChunkMesh build_chunk_mesh(
    const ChunkData& chunk_data,
    ChunkCoord coord,
    const BlockRegistry& block_registry,
    const ChunkMeshNeighbors& neighbors
);

}
