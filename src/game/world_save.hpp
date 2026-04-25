#pragma once

#include "game/world_types.hpp"

#include <filesystem>
#include <optional>

namespace ml {

struct WorldMetadata {
    int version {1};
    WorldSeed world_seed {0};
};

class WorldSave {
public:
    explicit WorldSave(std::filesystem::path root);

    const std::filesystem::path& root() const;
    const std::filesystem::path& chunks_directory() const;

    WorldMetadata load_or_create_metadata();
    std::optional<ChunkData> load_chunk(ChunkCoord coord) const;
    bool save_chunk(ChunkCoord coord, const ChunkData& chunk) const;

private:
    std::filesystem::path chunk_path(ChunkCoord coord) const;

    std::filesystem::path root_;
    std::filesystem::path chunks_directory_;
};

WorldSeed random_world_seed();
WorldSeed splitmix64(WorldSeed value);
WorldSeed get_chunk_seed(WorldSeed world_seed, int chunk_x, int chunk_z);

}
