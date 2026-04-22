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

    const BlockRegistry& block_registry_;
};

ChunkMesh build_chunk_mesh(const ChunkData& chunk_data, ChunkCoord coord, const BlockRegistry& block_registry);

}

