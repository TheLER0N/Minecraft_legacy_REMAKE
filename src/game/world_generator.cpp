#include "game/world_generator.hpp"

#include "common/log.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <limits>

namespace ml {

namespace {

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
                    if (neighbor == block) {
                        continue;
                    }
                    if (block_registry.is_renderable(neighbor) && block_registry.is_opaque(neighbor) == block_registry.is_opaque(block)) {
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
    constexpr int water_level = 40;

    for (int z = 0; z < kChunkDepth; ++z) {
        for (int x = 0; x < kChunkWidth; ++x) {
            const int world_x = coord.x * kChunkWidth + x;
            const int world_z = coord.z * kChunkDepth + z;
            const int height = static_cast<int>(sample_height(world_x, world_z, seed));

            for (int y = 0; y < kChunkHeight; ++y) {
                if (y > height) {
                    if (y <= water_level) {
                        chunk.set(x, y, z, BlockId::Water);
                    }
                    continue;
                }

                if (y == height) {
                    chunk.set(x, y, z, BlockId::Grass);
                } else if (y > height - 4) {
                    chunk.set(x, y, z, BlockId::Dirt);
                } else {
                    chunk.set(x, y, z, BlockId::Stone);
                }
            }
        }
    }

    return chunk;
}

float WorldGenerator::sample_height(int world_x, int world_z, WorldSeed seed) const {
    const float low = hash_noise(world_x / 8, world_z / 8, seed) * 18.0f;
    const float high = hash_noise(world_x / 3, world_z / 3, seed ^ 0xA5A5A5A5ull) * 8.0f;
    return 28.0f + low + high;
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
