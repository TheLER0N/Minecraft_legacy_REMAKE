#include "game/world_generator.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <limits>

namespace ml {

namespace {

constexpr int kWaterLevel = 40;
constexpr int kShoreGravelRadiusMin = 2;
constexpr int kShoreGravelRadiusMax = 5;
constexpr int kShoreCandidateMinYOffset = -4;
constexpr int kShoreCandidateMaxYOffset = 1;
constexpr int kUnderwaterDepthMin = 3;
constexpr int kUndergroundGravelBlobAttempts = 14;
constexpr int kOakTreeMargin = 4;

struct FaceTemplate {
    const char* name;
    int neighbor_x;
    int neighbor_y;
    int neighbor_z;
    std::array<Vec3, 4> vertices;
};

constexpr std::array<FaceTemplate, 6> kFaceTemplates {{
    {
        "top",
        0, 1, 0,
        {{
            {0.0f, 1.0f, 0.0f},
            {0.0f, 1.0f, 1.0f},
            {1.0f, 1.0f, 1.0f},
            {1.0f, 1.0f, 0.0f}
        }}
    },
    {
        "bottom",
        0, -1, 0,
        {{
            {0.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 1.0f},
            {0.0f, 0.0f, 1.0f}
        }}
    },
    {
        "east",
        1, 0, 0,
        {{
            {1.0f, 0.0f, 0.0f},
            {1.0f, 1.0f, 0.0f},
            {1.0f, 1.0f, 1.0f},
            {1.0f, 0.0f, 1.0f}
        }}
    },
    {
        "west",
        -1, 0, 0,
        {{
            {0.0f, 0.0f, 1.0f},
            {0.0f, 1.0f, 1.0f},
            {0.0f, 1.0f, 0.0f},
            {0.0f, 0.0f, 0.0f}
        }}
    },
    {
        "south",
        0, 0, 1,
        {{
            {1.0f, 0.0f, 1.0f},
            {1.0f, 1.0f, 1.0f},
            {0.0f, 1.0f, 1.0f},
            {0.0f, 0.0f, 1.0f}
        }}
    },
    {
        "north",
        0, 0, -1,
        {{
            {0.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f},
            {1.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 0.0f}
        }}
    }
}};

float hash_noise(int x, int z, WorldSeed seed) {
    std::uint64_t value = static_cast<std::uint64_t>(x) * 0x9E3779B185EBCA87ull;
    value ^= static_cast<std::uint64_t>(z) * 0xC2B2AE3D27D4EB4Full;
    value ^= seed + 0x165667B19E3779F9ull;
    value ^= (value >> 33u);
    value *= 0xff51afd7ed558ccdull;
    value ^= (value >> 33u);
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= (value >> 33u);
    return static_cast<float>(value & 0xFFFFFFull) / static_cast<float>(0xFFFFFFull);
}

int floor_div(int value, int divisor) {
    int quotient = value / divisor;
    const int remainder = value % divisor;
    if (remainder != 0 && ((remainder < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

int random_range(int min_value, int max_value, float noise) {
    const int span = max_value - min_value + 1;
    const int offset = std::clamp(static_cast<int>(noise * static_cast<float>(span)), 0, span - 1);
    return min_value + offset;
}

bool can_replace_surface_with_gravel(BlockId block) {
    return block == BlockId::Sand || block == BlockId::Dirt || block == BlockId::Grass;
}

bool should_emit_face(BlockId block, BlockId neighbor, const BlockRegistry& block_registry) {
    if (!block_registry.is_renderable(neighbor)) {
        return true;
    }

    if (block == BlockId::OakLeaves) {
        return !block_registry.is_opaque(neighbor);
    }

    if (neighbor == block) {
        return false;
    }

    return block_registry.is_opaque(neighbor) != block_registry.is_opaque(block);
}

float smooth_noise(float x, float z, WorldSeed seed) {
    int x0 = static_cast<int>(std::floor(x));
    int z0 = static_cast<int>(std::floor(z));
    int x1 = x0 + 1;
    int z1 = z0 + 1;

    float tx = x - static_cast<float>(x0);
    float tz = z - static_cast<float>(z0);

    tx = tx * tx * (3.0f - 2.0f * tx);
    tz = tz * tz * (3.0f - 2.0f * tz);

    float n00 = hash_noise(x0, z0, seed);
    float n10 = hash_noise(x1, z0, seed);
    float n01 = hash_noise(x0, z1, seed);
    float n11 = hash_noise(x1, z1, seed);

    float nx0 = n00 + tx * (n10 - n00);
    float nx1 = n01 + tx * (n11 - n01);

    return nx0 + tz * (nx1 - nx0);
}

void append_face(ChunkMesh& mesh, const Vec3& color, const std::array<Vec3, 4>& vertices, const std::array<Vec2, 4>& uvs, std::uint32_t tex_index) {
    const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        mesh.vertices.push_back({vertices[i], color, uvs[i], tex_index});
    }

    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 3);
    mesh.indices.push_back(base + 0);
}

template <typename SampleFn>
ChunkMesh build_mesh_from_sampler(SampleFn&& sample, ChunkCoord coord, const BlockRegistry& block_registry, std::size_t* face_count_out = nullptr) {
    ChunkMesh mesh {};
    std::size_t face_count = 0;

    for (int y = 0; y < kChunkHeight; ++y) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int x = 0; x < kChunkWidth; ++x) {
                const BlockId block = sample(x, y, z);
                if (!block_registry.is_renderable(block)) {
                    continue;
                }

                const float world_x = static_cast<float>(coord.x * kChunkWidth + x);
                const float world_y = static_cast<float>(y);
                const float world_z = static_cast<float>(coord.z * kChunkDepth + z);

                for (const FaceTemplate& face : kFaceTemplates) {
                    const BlockId neighbor = sample(x + face.neighbor_x, y + face.neighbor_y, z + face.neighbor_z);
                    if (!should_emit_face(block, neighbor, block_registry)) {
                        continue;
                    }

                    std::array<Vec3, 4> vertices {};
                    for (std::size_t i = 0; i < face.vertices.size(); ++i) {
                        vertices[i] = {
                            world_x + face.vertices[i].x,
                            world_y + face.vertices[i].y,
                            world_z + face.vertices[i].z
                        };
                    }

                    std::array<Vec2, 4> uvs {{
                        {0.0f, 1.0f},
                        {0.0f, 0.0f},
                        {1.0f, 0.0f},
                        {1.0f, 1.0f}
                    }};
                    if (block == BlockId::OakLeaves) {
                        uvs = {{
                            {1.0f, 0.0f},
                            {1.0f, 1.0f},
                            {0.0f, 1.0f},
                            {0.0f, 0.0f}
                        }};
                    }

                    std::uint32_t tex_index = 0;
                    Vec3 face_color {1.0f, 1.0f, 1.0f};

                    if (face.neighbor_y == 1) {
                        tex_index = block_registry.get(block).tex_top;
                        if (block == BlockId::Grass) {
                            face_color = block_registry.get(block).debug_color;
                        }
                    } else if (face.neighbor_y == -1) {
                        tex_index = block_registry.get(block).tex_bottom;
                    } else {
                        tex_index = block_registry.get(block).tex_side;
                    }

                    append_face(mesh, face_color, vertices, uvs, tex_index);
                    ++face_count;
                }
            }
        }
    }

    if (face_count_out != nullptr) {
        *face_count_out = face_count;
    }

    return mesh;
}

void run_mesh_builder_self_check(const BlockRegistry& block_registry) {
    static bool checked = false;
    if (checked) {
        return;
    }
    checked = true;

    ChunkData single_block {};
    single_block.set(0, 0, 0, BlockId::Stone);
    const auto single_sample = [&](int x, int y, int z) -> BlockId {
        if (x < 0 || x >= kChunkWidth || y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return single_block.get(x, y, z);
    };
    std::size_t single_faces = 0;
    const ChunkMesh single_mesh = build_mesh_from_sampler(single_sample, {}, block_registry, &single_faces);
    assert(single_faces == 6);
    assert(single_mesh.vertices.size() == 24);
    assert(single_mesh.indices.size() == 36);

    ChunkData two_blocks {};
    two_blocks.set(0, 0, 0, BlockId::Stone);
    two_blocks.set(1, 0, 0, BlockId::Stone);
    const auto double_sample = [&](int x, int y, int z) -> BlockId {
        if (x < 0 || x >= kChunkWidth || y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return two_blocks.get(x, y, z);
    };
    std::size_t double_faces = 0;
    const ChunkMesh double_mesh = build_mesh_from_sampler(double_sample, {}, block_registry, &double_faces);
    assert(double_faces == 10);
    assert(double_mesh.vertices.size() == 40);
    assert(double_mesh.indices.size() == 60);

    ChunkData seam_left {};
    seam_left.set(kChunkWidth - 1, 0, 0, BlockId::Stone);
    const auto seam_left_sample = [&](int x, int y, int z) -> BlockId {
        if (x < 0 || x >= kChunkWidth || y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return seam_left.get(x, y, z);
    };
    const ChunkMesh seam_left_mesh = build_mesh_from_sampler(seam_left_sample, {0, 0}, block_registry);

    ChunkData seam_right {};
    seam_right.set(0, 0, 0, BlockId::Stone);
    const auto seam_right_sample = [&](int x, int y, int z) -> BlockId {
        if (x < 0 || x >= kChunkWidth || y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return seam_right.get(x, y, z);
    };
    const ChunkMesh seam_right_mesh = build_mesh_from_sampler(seam_right_sample, {1, 0}, block_registry);

    const auto x_extent = [](const ChunkMesh& mesh) {
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        for (const Vertex& vertex : mesh.vertices) {
            min_x = std::min(min_x, vertex.position.x);
            max_x = std::max(max_x, vertex.position.x);
        }
        return std::array<float, 2> {min_x, max_x};
    };

    const auto left_extent = x_extent(seam_left_mesh);
    const auto right_extent = x_extent(seam_right_mesh);
    assert(left_extent[0] == static_cast<float>(kChunkWidth - 1));
    assert(left_extent[1] == static_cast<float>(kChunkWidth));
    assert(right_extent[0] == static_cast<float>(kChunkWidth));
    assert(right_extent[1] == static_cast<float>(kChunkWidth + 1));

    log_message(LogLevel::Info, "WorldGenerator: seam self-check passed, chunk boundary blocks remain 1x1x1");
}

}

WorldGenerator::WorldGenerator(const BlockRegistry& block_registry)
    : block_registry_(block_registry) {
}

ChunkData WorldGenerator::generate_chunk(ChunkCoord coord, WorldSeed seed) const {
    ChunkData chunk {};

    for (int z = 0; z < kChunkDepth; ++z) {
        for (int x = 0; x < kChunkWidth; ++x) {
            const int world_x = coord.x * kChunkWidth + x;
            const int world_z = coord.z * kChunkDepth + z;
            const int height = static_cast<int>(sample_height(world_x, world_z, seed));

            for (int y = 0; y < kChunkHeight; ++y) {
                if (y > height) {
                    if (y <= kWaterLevel) {
                        chunk.set(x, y, z, BlockId::Water);
                    }
                    continue;
                }

                if (y == height) {
                    if (y <= kWaterLevel + 1) {
                        chunk.set(x, y, z, BlockId::Sand);
                    } else {
                        chunk.set(x, y, z, BlockId::Grass);
                    }
                } else if (y > height - 4) {
                    if (height <= kWaterLevel + 1) {
                        chunk.set(x, y, z, BlockId::Sand);
                    } else {
                        chunk.set(x, y, z, BlockId::Dirt);
                    }
                } else {
                    chunk.set(x, y, z, BlockId::Stone);
                }
            }
        }
    }

    apply_underwater_gravel_bottom(chunk, coord, kWaterLevel, seed);
    apply_shore_gravel_disks(chunk, coord, kWaterLevel, seed);
    apply_underground_gravel_blobs(chunk, coord, seed);
    apply_oak_trees(chunk, coord, seed);

    return chunk;
}

bool WorldGenerator::is_water_adjacent_or_submerged_surface(
    int world_x,
    int world_z,
    int surface_y,
    int water_level,
    WorldSeed seed) const {
    if (surface_y <= water_level - kUnderwaterDepthMin) {
        return true;
    }

    constexpr std::array<std::array<int, 2>, 8> offsets {{
        {{1, 0}},
        {{-1, 0}},
        {{0, 1}},
        {{0, -1}},
        {{2, 0}},
        {{-2, 0}},
        {{0, 2}},
        {{0, -2}}
    }};

    for (const auto& offset : offsets) {
        const int neighbor_height = static_cast<int>(sample_height(world_x + offset[0], world_z + offset[1], seed));
        if (neighbor_height < water_level && surface_y <= water_level + 1) {
            return true;
        }
    }

    return false;
}

void WorldGenerator::apply_underwater_gravel_bottom(ChunkData& chunk, ChunkCoord coord, int water_level, WorldSeed seed) const {
    for (int z = 0; z < kChunkDepth; ++z) {
        for (int x = 0; x < kChunkWidth; ++x) {
            const int world_x = coord.x * kChunkWidth + x;
            const int world_z = coord.z * kChunkDepth + z;
            const int surface_y = static_cast<int>(sample_height(world_x, world_z, seed));
            if (surface_y < 0 || surface_y >= kChunkHeight || surface_y > water_level - kUnderwaterDepthMin) {
                continue;
            }

            const float clump = smooth_noise(static_cast<float>(world_x) * 0.13f, static_cast<float>(world_z) * 0.13f, seed ^ 0xA0C7B157BEEFull);
            const float edge = smooth_noise(static_cast<float>(world_x) * 0.31f, static_cast<float>(world_z) * 0.31f, seed ^ 0x6C8E9CF570932BD5ull);
            if (clump + edge * 0.35f < 0.78f) {
                continue;
            }

            if (can_replace_surface_with_gravel(chunk.get(x, surface_y, z)) || chunk.get(x, surface_y, z) == BlockId::Stone) {
                chunk.set(x, surface_y, z, BlockId::Gravel);
            }
            if (surface_y > 0 && chunk.get(x, surface_y - 1, z) == BlockId::Sand && hash_noise(world_x, world_z, seed ^ 0xB0770B077ull) > 0.45f) {
                chunk.set(x, surface_y - 1, z, BlockId::Gravel);
            }
        }
    }
}

void WorldGenerator::apply_shore_gravel_disks(ChunkData& chunk, ChunkCoord coord, int water_level, WorldSeed seed) const {
    constexpr int cell_size = 8;
    constexpr int margin = kShoreGravelRadiusMax;

    const int min_world_x = coord.x * kChunkWidth;
    const int min_world_z = coord.z * kChunkDepth;
    const int max_world_x = min_world_x + kChunkWidth - 1;
    const int max_world_z = min_world_z + kChunkDepth - 1;

    const int min_cell_x = floor_div(min_world_x - margin, cell_size);
    const int max_cell_x = floor_div(max_world_x + margin, cell_size);
    const int min_cell_z = floor_div(min_world_z - margin, cell_size);
    const int max_cell_z = floor_div(max_world_z + margin, cell_size);

    for (int cell_z = min_cell_z; cell_z <= max_cell_z; ++cell_z) {
        for (int cell_x = min_cell_x; cell_x <= max_cell_x; ++cell_x) {
            const float chance = hash_noise(cell_x, cell_z, seed ^ 0x51A0C0A57E5D1234ull);
            if (chance > 0.34f) {
                continue;
            }

            const int center_x = cell_x * cell_size + random_range(1, cell_size - 2, hash_noise(cell_x * 17, cell_z * 31, seed ^ 0x2143658709ABCDEFull));
            const int center_z = cell_z * cell_size + random_range(1, cell_size - 2, hash_noise(cell_x * 37, cell_z * 19, seed ^ 0x13579BDF2468ACE0ull));
            const int center_y = static_cast<int>(sample_height(center_x, center_z, seed));
            if (center_y < water_level + kShoreCandidateMinYOffset || center_y > water_level + kShoreCandidateMaxYOffset) {
                continue;
            }
            if (!is_water_adjacent_or_submerged_surface(center_x, center_z, center_y, water_level, seed)) {
                continue;
            }

            const int radius = random_range(kShoreGravelRadiusMin, kShoreGravelRadiusMax, hash_noise(cell_x * 53, cell_z * 47, seed ^ 0xC0FFEE1234567890ull));
            const float radius_sq = static_cast<float>(radius * radius);

            for (int z = 0; z < kChunkDepth; ++z) {
                for (int x = 0; x < kChunkWidth; ++x) {
                    const int world_x = min_world_x + x;
                    const int world_z = min_world_z + z;
                    const int dx = world_x - center_x;
                    const int dz = world_z - center_z;
                    const float distance_sq = static_cast<float>(dx * dx + dz * dz);
                    const float boundary_jitter = hash_noise(world_x * 11 + center_x, world_z * 13 + center_z, seed ^ 0xD15C5A7Eull) * 1.25f;
                    if (distance_sq > radius_sq + boundary_jitter) {
                        continue;
                    }

                    const int surface_y = static_cast<int>(sample_height(world_x, world_z, seed));
                    if (surface_y < 0 || surface_y >= kChunkHeight) {
                        continue;
                    }
                    if (surface_y < water_level + kShoreCandidateMinYOffset || surface_y > water_level + kShoreCandidateMaxYOffset) {
                        continue;
                    }
                    if (!is_water_adjacent_or_submerged_surface(world_x, world_z, surface_y, water_level, seed)) {
                        continue;
                    }
                    if (can_replace_surface_with_gravel(chunk.get(x, surface_y, z))) {
                        chunk.set(x, surface_y, z, BlockId::Gravel);
                    }
                    if (surface_y > 0 && chunk.get(x, surface_y - 1, z) == BlockId::Sand && hash_noise(world_x * 3, world_z * 5, seed ^ 0x5A7D5A7Dull) > 0.62f) {
                        chunk.set(x, surface_y - 1, z, BlockId::Gravel);
                    }
                }
            }
        }
    }
}

void WorldGenerator::apply_underground_gravel_blobs(ChunkData& chunk, ChunkCoord coord, WorldSeed seed) const {
    constexpr int max_radius = 3;

    for (int source_chunk_z = coord.z - 1; source_chunk_z <= coord.z + 1; ++source_chunk_z) {
        for (int source_chunk_x = coord.x - 1; source_chunk_x <= coord.x + 1; ++source_chunk_x) {
            for (int attempt = 0; attempt < kUndergroundGravelBlobAttempts; ++attempt) {
                const int seed_x = source_chunk_x * 8191 + attempt * 73;
                const int seed_z = source_chunk_z * 131071 - attempt * 97;
                const int center_x = source_chunk_x * kChunkWidth + random_range(0, kChunkWidth - 1, hash_noise(seed_x, seed_z, seed ^ 0x9A504EA9D90E4A11ull));
                const int center_z = source_chunk_z * kChunkDepth + random_range(0, kChunkDepth - 1, hash_noise(seed_x, seed_z, seed ^ 0x4B1D0B5B00B51357ull));
                const int center_y = random_range(5, 72, hash_noise(seed_x, seed_z, seed ^ 0x7F4A7C159E3779B9ull));
                const float radius = 1.5f + hash_noise(seed_x, seed_z, seed ^ 0x0DDC0FFEEC0FFEE0ull) * 1.7f;
                const float radius_sq = radius * radius;

                const int min_x = std::max(0, center_x - max_radius - coord.x * kChunkWidth);
                const int max_x = std::min(kChunkWidth - 1, center_x + max_radius - coord.x * kChunkWidth);
                const int min_z = std::max(0, center_z - max_radius - coord.z * kChunkDepth);
                const int max_z = std::min(kChunkDepth - 1, center_z + max_radius - coord.z * kChunkDepth);
                const int min_y = std::max(0, center_y - max_radius);
                const int max_y = std::min(kChunkHeight - 1, center_y + max_radius);

                for (int y = min_y; y <= max_y; ++y) {
                    for (int z = min_z; z <= max_z; ++z) {
                        for (int x = min_x; x <= max_x; ++x) {
                            const int world_x = coord.x * kChunkWidth + x;
                            const int world_z = coord.z * kChunkDepth + z;
                            const float dx = static_cast<float>(world_x - center_x);
                            const float dy = static_cast<float>(y - center_y);
                            const float dz = static_cast<float>(world_z - center_z);
                            const float noise = hash_noise(world_x * 29 + y, world_z * 31 - y, seed ^ 0xA11ECA7Eull) * 0.75f;
                            if (dx * dx + dy * dy + dz * dz > radius_sq + noise) {
                                continue;
                            }
                            if (chunk.get(x, y, z) == BlockId::Stone) {
                                chunk.set(x, y, z, BlockId::Gravel);
                            }
                        }
                    }
                }
            }
        }
    }
}

void WorldGenerator::apply_oak_trees(ChunkData& chunk, ChunkCoord coord, WorldSeed seed) const {
    constexpr int cell_size = 10;
    const int min_world_x = coord.x * kChunkWidth;
    const int min_world_z = coord.z * kChunkDepth;
    const int max_world_x = min_world_x + kChunkWidth - 1;
    const int max_world_z = min_world_z + kChunkDepth - 1;

    const int min_cell_x = floor_div(min_world_x - kOakTreeMargin, cell_size);
    const int max_cell_x = floor_div(max_world_x + kOakTreeMargin, cell_size);
    const int min_cell_z = floor_div(min_world_z - kOakTreeMargin, cell_size);
    const int max_cell_z = floor_div(max_world_z + kOakTreeMargin, cell_size);

    for (int cell_z = min_cell_z; cell_z <= max_cell_z; ++cell_z) {
        for (int cell_x = min_cell_x; cell_x <= max_cell_x; ++cell_x) {
            const float chance = hash_noise(cell_x, cell_z, seed ^ 0x0A0C7AEE0A0C7AEEull);
            if (chance > 0.22f) {
                continue;
            }

            const int base_world_x = cell_x * cell_size + random_range(2, cell_size - 3, hash_noise(cell_x * 41, cell_z * 67, seed ^ 0x710EE5B16B16B16Bull));
            const int base_world_z = cell_z * cell_size + random_range(2, cell_size - 3, hash_noise(cell_x * 89, cell_z * 43, seed ^ 0x5A9160A9160A9160ull));
            const int ground_y = static_cast<int>(sample_height(base_world_x, base_world_z, seed));
            const int trunk_height = random_range(4, 6, hash_noise(cell_x * 97, cell_z * 101, seed ^ 0x1080A401080A401ull));

            if (!can_place_oak_tree(base_world_x, ground_y, base_world_z, seed)) {
                continue;
            }

            try_place_oak_tree(chunk, coord, base_world_x, ground_y + 1, base_world_z, trunk_height, seed);
        }
    }
}

bool WorldGenerator::can_place_oak_tree(int base_world_x, int ground_y, int base_world_z, WorldSeed seed) const {
    if (ground_y < 1 || ground_y + 7 >= kChunkHeight || ground_y <= kWaterLevel + 1) {
        return false;
    }

    const int local_surface = static_cast<int>(sample_height(base_world_x, base_world_z, seed));
    if (local_surface != ground_y) {
        return false;
    }

    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int neighbor_height = static_cast<int>(sample_height(base_world_x + dx, base_world_z + dz, seed));
            if (neighbor_height <= kWaterLevel + 1) {
                return false;
            }
            if (std::abs(neighbor_height - ground_y) > 2) {
                return false;
            }
        }
    }

    return true;
}

void WorldGenerator::try_place_oak_tree(
    ChunkData& chunk,
    ChunkCoord coord,
    int base_world_x,
    int base_y,
    int base_world_z,
    int trunk_height,
    WorldSeed seed) const {
    const int chunk_min_x = coord.x * kChunkWidth;
    const int chunk_min_z = coord.z * kChunkDepth;

    const auto set_if_local = [&](int world_x, int y, int world_z, BlockId block) {
        const int local_x = world_x - chunk_min_x;
        const int local_z = world_z - chunk_min_z;
        if (local_x < 0 || local_x >= kChunkWidth || local_z < 0 || local_z >= kChunkDepth || y < 0 || y >= kChunkHeight) {
            return;
        }

        const BlockId existing = chunk.get(local_x, y, local_z);
        if (block == BlockId::OakLog) {
            if (existing == BlockId::Air || existing == BlockId::OakLeaves) {
                chunk.set(local_x, y, local_z, block);
            }
            return;
        }

        if (existing == BlockId::Air) {
            chunk.set(local_x, y, local_z, block);
        }
    };

    for (int y = 0; y < trunk_height; ++y) {
        set_if_local(base_world_x, base_y + y, base_world_z, BlockId::OakLog);
    }

    const int leaf_base_y = base_y + trunk_height - 2;
    for (int dy = 0; dy <= 3; ++dy) {
        const int layer_y = leaf_base_y + dy;
        const int radius = dy >= 2 ? 1 : 2;
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                const int manhattan = std::abs(dx) + std::abs(dz);
                const bool corner = std::abs(dx) == radius && std::abs(dz) == radius;
                if (corner && hash_noise(base_world_x + dx * 13 + layer_y, base_world_z + dz * 17 - layer_y, seed ^ 0x1EA5E51EA5E5ull) < 0.45f) {
                    continue;
                }
                if (dy == 3 && manhattan > 1) {
                    continue;
                }
                set_if_local(base_world_x + dx, layer_y, base_world_z + dz, BlockId::OakLeaves);
            }
        }
    }
}

float WorldGenerator::sample_height(int world_x, int world_z, WorldSeed seed) const {
    const float x = static_cast<float>(world_x);
    const float z = static_cast<float>(world_z);

    float elevation = smooth_noise(x * 0.01f, z * 0.01f, seed) * 32.0f;
    float roughness = smooth_noise(x * 0.05f, z * 0.05f, seed ^ 0x12345678ull) * 16.0f;
    float detail = smooth_noise(x * 0.1f, z * 0.1f, seed ^ 0x87654321ull) * 8.0f;

    return 30.0f + elevation + roughness + detail;
}

ChunkMesh build_chunk_mesh(const ChunkData& chunk_data, ChunkCoord coord, const BlockRegistry& block_registry) {
    run_mesh_builder_self_check(block_registry);

    const auto sample = [&](int x, int y, int z) -> BlockId {
        if (x < 0 || x >= kChunkWidth || y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return chunk_data.get(x, y, z);
    };

    return build_mesh_from_sampler(sample, coord, block_registry);
}

}
