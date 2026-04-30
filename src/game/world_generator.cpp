#include "game/world_generator.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace ml {

namespace {

constexpr int kShoreGravelRadiusMin = 2;
constexpr int kShoreGravelRadiusMax = 5;
constexpr int kShoreCandidateMinYOffset = -4;
constexpr int kShoreCandidateMaxYOffset = 1;
constexpr int kUnderwaterDepthMin = 3;
constexpr int kUndergroundGravelBlobAttempts = 14;
constexpr int kOakTreeMargin = 4;
constexpr int kSurfaceCaveProtectionDepth = 6;
constexpr int kCaveMinY = kWorldMinY + 4;
constexpr int kCaveMaxY = 220;
constexpr int kAquiferMinY = kWorldMinY + 8;
constexpr int kAquiferMaxY = kSeaLevel - 4;
constexpr int kMeshVerticalPadding = 2;

struct ColumnProfile {
    int surface_y {kSeaLevel};
    int cave_min_y {kCaveMinY};
    int cave_max_y {kCaveMinY - 1};
    float continentalness {0.0f};
    bool deep_ocean {false};
    bool cave_candidate {false};
};

struct VerticalRange {
    int min_y {0};
    int max_y {-1};

    bool empty() const {
        return max_y < min_y;
    }
};

std::size_t column_index(int x, int z) {
    return static_cast<std::size_t>(x + z * kChunkWidth);
}

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

float hash_noise3d(int x, int y, int z, WorldSeed seed) {
    std::uint64_t value = static_cast<std::uint64_t>(x) * 0x9E3779B185EBCA87ull;
    value ^= static_cast<std::uint64_t>(y) * 0xD6E8FEB86659FD93ull;
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

bool is_greedy_opaque_block(BlockId block, const BlockRegistry& block_registry) {
    return block_registry.render_type(block) == BlockRenderType::Opaque && block_registry.is_solid(block);
}

bool is_greedy_cutout_block(BlockId block, const BlockRegistry& block_registry) {
    return block_registry.render_type(block) == BlockRenderType::Cutout;
}

bool preserves_internal_cutout_faces(BlockId block, LeavesRenderMode leaves_mode) {
    return block == BlockId::OakLeaves && leaves_mode == LeavesRenderMode::Fancy;
}

bool uses_greedy_cutout_meshing(BlockId block, const BlockRegistry& block_registry, LeavesRenderMode leaves_mode) {
    if (!is_greedy_cutout_block(block, block_registry)) {
        return false;
    }
    return !preserves_internal_cutout_faces(block, leaves_mode);
}

bool is_water_block(BlockId block, const BlockRegistry& block_registry) {
    return block_registry.render_type(block) == BlockRenderType::Transparent;
}

bool should_emit_opaque_face(BlockId block, BlockId neighbor, const BlockRegistry& block_registry) {
    if (!block_registry.is_renderable(neighbor)) {
        return true;
    }
    if (neighbor == block) {
        return false;
    }
    return block_registry.render_type(neighbor) != BlockRenderType::Opaque;
}

bool should_emit_cutout_face(BlockId block, BlockId neighbor, const BlockRegistry& block_registry, LeavesRenderMode leaves_mode) {
    if (!block_registry.is_renderable(neighbor)) {
        return true;
    }
    if (neighbor == block) {
        return preserves_internal_cutout_faces(block, leaves_mode);
    }
    return !block_registry.is_opaque(neighbor);
}

bool should_emit_water_face(BlockId block, BlockId neighbor, const BlockRegistry& block_registry, int face_index) {
    if (face_index == 1) {
        return false;
    }
    if (!block_registry.is_renderable(neighbor)) {
        return true;
    }
    if (neighbor == block || block_registry.render_type(neighbor) == BlockRenderType::Transparent) {
        return false;
    }
    return true;
}

bool should_emit_face(BlockId block, BlockId neighbor, const BlockRegistry& block_registry, int face_index, LeavesRenderMode leaves_mode) {
    switch (block_registry.render_type(block)) {
    case BlockRenderType::Opaque:
        return should_emit_opaque_face(block, neighbor, block_registry);
    case BlockRenderType::Cutout:
        return should_emit_cutout_face(block, neighbor, block_registry, leaves_mode);
    case BlockRenderType::Transparent:
        return should_emit_water_face(block, neighbor, block_registry, face_index);
    default:
        return false;
    }
}

struct FaceKey {
    BlockId block {BlockId::Air};
    std::uint32_t texture_index {0};
    Vec3 color {1.0f, 1.0f, 1.0f};
};

struct FaceLighting {
    std::array<Vec3, 4> colors {};
};

struct FaceLightKey {
    std::array<std::uint8_t, 4> sky {};
    std::array<std::uint8_t, 4> ao {};

    bool operator==(const FaceLightKey&) const = default;
};

struct LightNode {
    int x {0};
    int y {0};
    int z {0};
};

struct ChunkLightData {
    static constexpr int kBorder = 1;
    static constexpr int kWidth = kChunkWidth + kBorder * 2;
    static constexpr int kDepth = kChunkDepth + kBorder * 2;

    std::array<std::uint8_t, static_cast<std::size_t>(kWidth * kDepth * kChunkHeight)> sky_light {};

    static std::size_t index(int x, int y, int z) {
        const int storage_x = x + kBorder;
        const int storage_z = z + kBorder;
        return static_cast<std::size_t>(storage_x + storage_z * kWidth + y * kWidth * kDepth);
    }

    static bool contains(int x, int y, int z) {
        return x >= -kBorder && x < kChunkWidth + kBorder &&
            y >= 0 && y < kChunkHeight &&
            z >= -kBorder && z < kChunkDepth + kBorder;
    }

    std::uint8_t get(int x, int y, int z) const {
        if (y >= kChunkHeight) {
            return 15;
        }
        if (!contains(x, y, z)) {
            return 0;
        }
        return sky_light[index(x, y, z)];
    }

    void set(int x, int y, int z, std::uint8_t value) {
        assert(contains(x, y, z));
        sky_light[index(x, y, z)] = value;
    }
};

struct MaskCell {
    bool valid {false};
    FaceKey key {};
    FaceLightKey light {};
};

bool same_color(const Vec3& lhs, const Vec3& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.z == rhs.z;
}

bool same_mask_cell(const MaskCell& lhs, const MaskCell& rhs) {
    return lhs.valid == rhs.valid &&
        lhs.key.block == rhs.key.block &&
        lhs.key.texture_index == rhs.key.texture_index &&
        same_color(lhs.key.color, rhs.key.color) &&
        lhs.light == rhs.light;
}

MeshSection& mesh_section_for_block(ChunkMesh& mesh, BlockId block, const BlockRegistry& block_registry) {
    switch (block_registry.render_type(block)) {
    case BlockRenderType::Opaque:
        return mesh.opaque_mesh;
    case BlockRenderType::Cutout:
        return mesh.cutout_mesh;
    case BlockRenderType::Transparent:
        return mesh.transparent_mesh;
    default:
        return mesh.opaque_mesh;
    }
}

float face_shade(int face_index) {
    switch (face_index) {
    case 0: return 1.00f; // top
    case 1: return 0.50f; // bottom
    case 2: return 0.82f; // east
    case 3: return 0.82f; // west
    case 4: return 0.68f; // south
    case 5: return 0.68f; // north
    default: return 1.0f;
    }
}

bool is_ao_occluder(BlockId block, const BlockRegistry& block_registry) {
    return block_registry.is_opaque(block) && block_registry.is_solid(block);
}

bool transmits_sky_light(BlockId block, const BlockRegistry& block_registry) {
    return block == BlockId::Air ||
        block_registry.render_type(block) == BlockRenderType::Transparent ||
        block_registry.render_type(block) == BlockRenderType::Cutout;
}

float vertex_ao(bool side_a, bool side_b, bool corner) {
    if (side_a && side_b) {
        return 0.45f;
    }
    const int occlusion = (side_a ? 1 : 0) + (side_b ? 1 : 0) + (corner ? 1 : 0);
    switch (occlusion) {
    case 0: return 1.00f;
    case 1: return 0.82f;
    case 2: return 0.64f;
    default: return 0.52f;
    }
}

std::uint8_t vertex_ao_key(bool side_a, bool side_b, bool corner) {
    if (side_a && side_b) {
        return 3;
    }
    return static_cast<std::uint8_t>((side_a ? 1 : 0) + (side_b ? 1 : 0) + (corner ? 1 : 0));
}

std::uint8_t bucket_sky_light(std::uint8_t level) {
    if (level == 0) {
        return 0;
    }
    if (level <= 3) {
        return 1;
    }
    if (level <= 7) {
        return 2;
    }
    if (level <= 11) {
        return 3;
    }
    return 4;
}

std::uint8_t bucket_ao_key(std::uint8_t raw_ao) {
    return raw_ao;
}

float light_level_to_brightness(std::uint8_t level) {
    const float normalized = static_cast<float>(std::min<std::uint8_t>(level, 15)) / 15.0f;
    return 0.055f + std::pow(normalized, 1.55f) * 0.945f;
}

template <typename SampleFn>
ChunkLightData build_sky_light_map(SampleFn&& sample, const BlockRegistry& block_registry, VerticalRange range) {
    ChunkLightData light {};
    if (range.empty()) {
        return light;
    }
    std::vector<LightNode> queue;
    queue.reserve(static_cast<std::size_t>(kChunkWidth * kChunkDepth * 8));

    const auto push_if_brighter = [&](int x, int y, int z, std::uint8_t value) {
        if (!ChunkLightData::contains(x, y, z)) {
            return;
        }
        if (!transmits_sky_light(sample(x, y, z), block_registry)) {
            return;
        }
        if (light.get(x, y, z) >= value) {
            return;
        }
        light.set(x, y, z, value);
        queue.push_back({x, y, z});
    };

    for (int z = -ChunkLightData::kBorder; z < kChunkDepth + ChunkLightData::kBorder; ++z) {
        for (int x = -ChunkLightData::kBorder; x < kChunkWidth + ChunkLightData::kBorder; ++x) {
            bool open_to_sky = true;
            for (int y = range.max_y; y >= range.min_y; --y) {
                const BlockId block = sample(x, y, z);
                if (!transmits_sky_light(block, block_registry)) {
                    open_to_sky = false;
                }
                if (!open_to_sky) {
                    continue;
                }
                light.set(x, y, z, 15);
                queue.push_back({x, y, z});
            }
        }
    }

    for (std::size_t read = 0; read < queue.size(); ++read) {
        const LightNode node = queue[read];
        const std::uint8_t current = light.get(node.x, node.y, node.z);
        if (current <= 1) {
            continue;
        }
        const std::uint8_t next = static_cast<std::uint8_t>(current - 1);
        push_if_brighter(node.x + 1, node.y, node.z, next);
        push_if_brighter(node.x - 1, node.y, node.z, next);
        push_if_brighter(node.x, node.y, node.z + 1, next);
        push_if_brighter(node.x, node.y, node.z - 1, next);
        push_if_brighter(node.x, node.y + 1, node.z, next);
        push_if_brighter(node.x, node.y - 1, node.z, next);
    }

    return light;
}

std::uint8_t face_vertex_light_level(const ChunkLightData& light, int face_index, const Vec3& local, int origin_x, int origin_y, int origin_z) {
    int x = origin_x + (local.x > 0.5f ? 1 : 0);
    int y = origin_y + (local.y > 0.5f ? 1 : 0);
    int z = origin_z + (local.z > 0.5f ? 1 : 0);

    switch (face_index) {
    case 0: y = origin_y + 1; break;
    case 1: y = origin_y - 1; break;
    case 2: x = origin_x + 1; break;
    case 3: x = origin_x - 1; break;
    case 4: z = origin_z + 1; break;
    case 5: z = origin_z - 1; break;
    default: break;
    }

    return light.get(x, y, z);
}

std::uint8_t greedy_face_vertex_light_level(const ChunkLightData& light, int face_index, const Vec3& vertex, const Vec3& center) {
    const auto axis_cell = [](float value, float center_value) {
        const float adjusted = value > center_value ? value - 0.001f : value;
        return static_cast<int>(std::floor(adjusted));
    };

    int x = axis_cell(vertex.x, center.x);
    int y = axis_cell(vertex.y, center.y);
    int z = axis_cell(vertex.z, center.z);

    switch (face_index) {
    case 0: y = static_cast<int>(std::floor(vertex.y)); break;
    case 1: y = static_cast<int>(std::floor(vertex.y)) - 1; break;
    case 2: x = static_cast<int>(std::floor(vertex.x)); break;
    case 3: x = static_cast<int>(std::floor(vertex.x)) - 1; break;
    case 4: z = static_cast<int>(std::floor(vertex.z)); break;
    case 5: z = static_cast<int>(std::floor(vertex.z)) - 1; break;
    default: break;
    }

    return light.get(x, y, z);
}

std::array<Int3, 3> ao_offsets_for_corner(int face_index, int sx, int sy, int sz) {
    switch (face_index) {
    case 0: return {{{sx, 1, 0}, {0, 1, sz}, {sx, 1, sz}}};
    case 1: return {{{sx, -1, 0}, {0, -1, sz}, {sx, -1, sz}}};
    case 2: return {{{1, sy, 0}, {1, 0, sz}, {1, sy, sz}}};
    case 3: return {{{-1, sy, 0}, {-1, 0, sz}, {-1, sy, sz}}};
    case 4: return {{{sx, 0, 1}, {0, sy, 1}, {sx, sy, 1}}};
    case 5: return {{{sx, 0, -1}, {0, sy, -1}, {sx, sy, -1}}};
    default:
        return {{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}}};
    }
}

std::array<Int3, 3> ao_offsets_for_vertex(int face_index, const Vec3& vertex) {
    const int sx = vertex.x > 0.5f ? 1 : -1;
    const int sy = vertex.y > 0.5f ? 1 : -1;
    const int sz = vertex.z > 0.5f ? 1 : -1;
    return ao_offsets_for_corner(face_index, sx, sy, sz);
}

Int3 greedy_corner_origin(int face_index, const Vec3& vertex, const Vec3& center) {
    const auto axis_cell = [](float value, float center_value) {
        const float adjusted = value > center_value ? value - 0.001f : value;
        return static_cast<int>(std::floor(adjusted));
    };

    Int3 origin {
        axis_cell(vertex.x, center.x),
        axis_cell(vertex.y, center.y),
        axis_cell(vertex.z, center.z)
    };

    switch (face_index) {
    case 0: origin.y = static_cast<int>(std::floor(vertex.y)) - 1; break;
    case 1: origin.y = static_cast<int>(std::floor(vertex.y)); break;
    case 2: origin.x = static_cast<int>(std::floor(vertex.x)) - 1; break;
    case 3: origin.x = static_cast<int>(std::floor(vertex.x)); break;
    case 4: origin.z = static_cast<int>(std::floor(vertex.z)) - 1; break;
    case 5: origin.z = static_cast<int>(std::floor(vertex.z)); break;
    default: break;
    }

    return origin;
}

template <typename SampleFn>
FaceLighting make_face_lighting(
    SampleFn&& sample,
    const BlockRegistry& block_registry,
    const ChunkLightData& light,
    BlockId block,
    int face_index,
    const std::array<Vec3, 4>& local_vertices,
    const Vec3& tint,
    int origin_x,
    int origin_y,
    int origin_z) {
    FaceLighting lighting {};
    const float shade = face_shade(face_index);
    const bool water = block_registry.render_type(block) == BlockRenderType::Transparent;

    for (std::size_t i = 0; i < local_vertices.size(); ++i) {
        const Vec3& local = local_vertices[i];
        const auto offsets = ao_offsets_for_vertex(face_index, local);
        const bool side_a = is_ao_occluder(sample(origin_x + offsets[0].x, origin_y + offsets[0].y, origin_z + offsets[0].z), block_registry);
        const bool side_b = is_ao_occluder(sample(origin_x + offsets[1].x, origin_y + offsets[1].y, origin_z + offsets[1].z), block_registry);
        const bool corner = is_ao_occluder(sample(origin_x + offsets[2].x, origin_y + offsets[2].y, origin_z + offsets[2].z), block_registry);
        const float ao = water ? 1.0f : vertex_ao(side_a, side_b, corner);
        const std::uint8_t light_level = face_vertex_light_level(light, face_index, local, origin_x, origin_y, origin_z);
        const float sky = light_level_to_brightness(light_level);
        const float brightness = std::clamp(sky * shade * ao, 0.035f, 1.0f);
        lighting.colors[i] = {tint.x * brightness, tint.y * brightness, tint.z * brightness};
    }

    return lighting;
}

template <typename SampleFn>
FaceLightKey make_face_light_key(
    SampleFn&& sample,
    const BlockRegistry& block_registry,
    const ChunkLightData& light,
    BlockId block,
    int face_index,
    int origin_x,
    int origin_y,
    int origin_z) {
    FaceLightKey key {};
    const bool water = block_registry.render_type(block) == BlockRenderType::Transparent;
    const std::array<Vec3, 4>& local_vertices = kFaceTemplates[static_cast<std::size_t>(face_index)].vertices;

    for (std::size_t i = 0; i < local_vertices.size(); ++i) {
        const Vec3& local = local_vertices[i];
        const auto offsets = ao_offsets_for_vertex(face_index, local);
        const bool side_a = is_ao_occluder(sample(origin_x + offsets[0].x, origin_y + offsets[0].y, origin_z + offsets[0].z), block_registry);
        const bool side_b = is_ao_occluder(sample(origin_x + offsets[1].x, origin_y + offsets[1].y, origin_z + offsets[1].z), block_registry);
        const bool corner = is_ao_occluder(sample(origin_x + offsets[2].x, origin_y + offsets[2].y, origin_z + offsets[2].z), block_registry);
        key.sky[i] = bucket_sky_light(face_vertex_light_level(light, face_index, local, origin_x, origin_y, origin_z));
        key.ao[i] = water ? 0 : bucket_ao_key(vertex_ao_key(side_a, side_b, corner));
    }

    return key;
}

template <typename SampleFn>
FaceLighting make_greedy_face_lighting(
    SampleFn&& sample,
    const BlockRegistry& block_registry,
    const ChunkLightData& light,
    BlockId block,
    int face_index,
    const std::array<Vec3, 4>& vertices,
    const Vec3& tint) {
    FaceLighting lighting {};
    const float shade = face_shade(face_index);
    const bool water = block_registry.render_type(block) == BlockRenderType::Transparent;

    Vec3 center {};
    for (const Vec3& vertex : vertices) {
        center += vertex;
    }
    center = center / static_cast<float>(vertices.size());

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Vec3& vertex = vertices[i];
        const int sx = vertex.x >= center.x ? 1 : -1;
        const int sy = vertex.y >= center.y ? 1 : -1;
        const int sz = vertex.z >= center.z ? 1 : -1;
        const Int3 origin = greedy_corner_origin(face_index, vertex, center);
        const auto offsets = ao_offsets_for_corner(face_index, sx, sy, sz);
        const bool side_a = is_ao_occluder(sample(origin.x + offsets[0].x, origin.y + offsets[0].y, origin.z + offsets[0].z), block_registry);
        const bool side_b = is_ao_occluder(sample(origin.x + offsets[1].x, origin.y + offsets[1].y, origin.z + offsets[1].z), block_registry);
        const bool corner = is_ao_occluder(sample(origin.x + offsets[2].x, origin.y + offsets[2].y, origin.z + offsets[2].z), block_registry);

        const float ao = water ? 1.0f : vertex_ao(side_a, side_b, corner);
        const std::uint8_t light_level = greedy_face_vertex_light_level(light, face_index, vertex, center);
        const float sky = light_level_to_brightness(light_level);
        const float brightness = std::clamp(sky * shade * ao, 0.035f, 1.0f);
        lighting.colors[i] = {tint.x * brightness, tint.y * brightness, tint.z * brightness};
    }

    return lighting;
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

float smooth_noise3d(float x, float y, float z, WorldSeed seed) {
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int z0 = static_cast<int>(std::floor(z));
    const int x1 = x0 + 1;
    const int y1 = y0 + 1;
    const int z1 = z0 + 1;

    float tx = x - static_cast<float>(x0);
    float ty = y - static_cast<float>(y0);
    float tz = z - static_cast<float>(z0);

    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    tz = tz * tz * (3.0f - 2.0f * tz);

    const float n000 = hash_noise3d(x0, y0, z0, seed);
    const float n100 = hash_noise3d(x1, y0, z0, seed);
    const float n010 = hash_noise3d(x0, y1, z0, seed);
    const float n110 = hash_noise3d(x1, y1, z0, seed);
    const float n001 = hash_noise3d(x0, y0, z1, seed);
    const float n101 = hash_noise3d(x1, y0, z1, seed);
    const float n011 = hash_noise3d(x0, y1, z1, seed);
    const float n111 = hash_noise3d(x1, y1, z1, seed);

    const float nx00 = n000 + tx * (n100 - n000);
    const float nx10 = n010 + tx * (n110 - n010);
    const float nx01 = n001 + tx * (n101 - n001);
    const float nx11 = n011 + tx * (n111 - n011);
    const float nxy0 = nx00 + ty * (nx10 - nx00);
    const float nxy1 = nx01 + ty * (nx11 - nx01);
    return nxy0 + tz * (nxy1 - nxy0);
}

void append_face(MeshSection& mesh, const Vec3& color, const std::array<Vec3, 4>& vertices, const std::array<Vec2, 4>& uvs, std::uint32_t tex_index) {
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

void append_lit_face(MeshSection& mesh, const FaceLighting& lighting, const std::array<Vec3, 4>& vertices, const std::array<Vec2, 4>& uvs, std::uint32_t tex_index) {
    const std::uint32_t base = static_cast<std::uint32_t>(mesh.vertices.size());
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        mesh.vertices.push_back({vertices[i], lighting.colors[i], uvs[i], tex_index});
    }

    mesh.indices.push_back(base + 0);
    mesh.indices.push_back(base + 1);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 2);
    mesh.indices.push_back(base + 3);
    mesh.indices.push_back(base + 0);
}

FaceKey make_face_key(BlockId block, const FaceTemplate& face, const BlockRegistry& block_registry) {
    FaceKey key {};
    key.block = block;
    key.color = {1.0f, 1.0f, 1.0f};
    if (block_registry.render_type(block) == BlockRenderType::Transparent) {
        key.color = block_registry.get(block).debug_color;
    }

    if (face.neighbor_y == 1) {
        key.texture_index = block_registry.get(block).tex_top;
        if (block == BlockId::Grass) {
            key.color = block_registry.get(block).debug_color;
        }
    } else if (face.neighbor_y == -1) {
        key.texture_index = block_registry.get(block).tex_bottom;
    } else {
        key.texture_index = block_registry.get(block).tex_side;
    }

    return key;
}

template <typename SampleFn>
void append_greedy_face(
    ChunkMesh& mesh,
    SampleFn&& sample,
    const BlockRegistry& block_registry,
    const ChunkLightData& light,
    int face_index,
    int slice,
    int u,
    int v,
    int width,
    int height,
    const FaceKey& key) {
    const float fw = static_cast<float>(width);
    const float fh = static_cast<float>(height);

    std::array<Vec3, 4> vertices {};
    switch (face_index) {
    case 0: { // top: u=x, v=z, slice=y
        const float x = static_cast<float>(u);
        const float y = static_cast<float>(slice + 1);
        const float z = static_cast<float>(v);
        vertices = {{{x, y, z}, {x, y, z + fh}, {x + fw, y, z + fh}, {x + fw, y, z}}};
        break;
    }
    case 1: { // bottom: u=x, v=z, slice=y
        const float x = static_cast<float>(u);
        const float y = static_cast<float>(slice);
        const float z = static_cast<float>(v);
        vertices = {{{x, y, z}, {x + fw, y, z}, {x + fw, y, z + fh}, {x, y, z + fh}}};
        break;
    }
    case 2: { // east: u=z, v=y, slice=x
        const float x = static_cast<float>(slice + 1);
        const float y = static_cast<float>(v);
        const float z = static_cast<float>(u);
        vertices = {{{x, y, z}, {x, y + fh, z}, {x, y + fh, z + fw}, {x, y, z + fw}}};
        break;
    }
    case 3: { // west: u=z, v=y, slice=x
        const float x = static_cast<float>(slice);
        const float y = static_cast<float>(v);
        const float z = static_cast<float>(u);
        vertices = {{{x, y, z + fw}, {x, y + fh, z + fw}, {x, y + fh, z}, {x, y, z}}};
        break;
    }
    case 4: { // south: u=x, v=y, slice=z
        const float x = static_cast<float>(u);
        const float y = static_cast<float>(v);
        const float z = static_cast<float>(slice + 1);
        vertices = {{{x + fw, y, z}, {x + fw, y + fh, z}, {x, y + fh, z}, {x, y, z}}};
        break;
    }
    case 5: { // north: u=x, v=y, slice=z
        const float x = static_cast<float>(u);
        const float y = static_cast<float>(v);
        const float z = static_cast<float>(slice);
        vertices = {{{x, y, z}, {x, y + fh, z}, {x + fw, y + fh, z}, {x + fw, y, z}}};
        break;
    }
    default:
        assert(false);
        break;
    }

    std::array<Vec2, 4> uvs {{
        {0.0f, fh},
        {0.0f, 0.0f},
        {fw, 0.0f},
        {fw, fh}
    }};
    if (face_index == 1) {
        // Bottom faces have the opposite vertex walk from top faces, so keep UVs tied to world X/Z.
        uvs = {{
            {0.0f, 0.0f},
            {fw, 0.0f},
            {fw, fh},
            {0.0f, fh}
        }};
    }
    const FaceLighting lighting = make_greedy_face_lighting(sample, block_registry, light, key.block, face_index, vertices, key.color);
    std::array<Vec3, 4> world_vertices = vertices;
    for (Vec3& vertex : world_vertices) {
        vertex.y += static_cast<float>(kWorldMinY);
    }
    append_lit_face(mesh_section_for_block(mesh, key.block, block_registry), lighting, world_vertices, uvs, key.texture_index);
}

template <typename SampleFn>
ChunkMesh build_mesh_from_sampler(
    SampleFn&& sample,
    const BlockRegistry& block_registry,
    LeavesRenderMode leaves_mode,
    std::size_t* face_count_out = nullptr) {
    ChunkMesh mesh {};
    std::size_t face_count = 0;
    VerticalRange occupied_range {};
    for (int y = 0; y < kChunkHeight; ++y) {
        bool has_renderable = false;
        for (int z = 0; z < kChunkDepth && !has_renderable; ++z) {
            for (int x = 0; x < kChunkWidth; ++x) {
                if (block_registry.is_renderable(sample(x, y, z))) {
                    has_renderable = true;
                    break;
                }
            }
        }
        if (!has_renderable) {
            continue;
        }
        if (occupied_range.empty()) {
            occupied_range.min_y = y;
        }
        occupied_range.max_y = y;
    }
    if (occupied_range.empty()) {
        if (face_count_out != nullptr) {
            *face_count_out = 0;
        }
        return mesh;
    }

    const VerticalRange work_range {
        std::max(0, occupied_range.min_y - kMeshVerticalPadding),
        std::min(kChunkHeight - 1, occupied_range.max_y + kMeshVerticalPadding)
    };
    const ChunkLightData light = build_sky_light_map(sample, block_registry, work_range);

    const auto emit_greedy_mask = [&](int face_index, int slice, int mask_width, int mask_height, int v_min, int v_max, std::vector<MaskCell>& mask) {
        (void)mask_height;
        for (int v = v_min; v <= v_max; ++v) {
            for (int u = 0; u < mask_width;) {
                const int index = u + v * mask_width;
                if (!mask[static_cast<std::size_t>(index)].valid) {
                    ++u;
                    continue;
                }

                int width = 1;
                while (u + width < mask_width &&
                    same_mask_cell(
                        mask[static_cast<std::size_t>(index)],
                        mask[static_cast<std::size_t>(u + width + v * mask_width)])) {
                    ++width;
                }

                int height = 1;
                bool can_extend = true;
                while (v + height <= v_max && can_extend) {
                    for (int step = 0; step < width; ++step) {
                        if (!same_mask_cell(
                                mask[static_cast<std::size_t>(index)],
                                mask[static_cast<std::size_t>(u + step + (v + height) * mask_width)])) {
                            can_extend = false;
                            break;
                        }
                    }
                    if (can_extend) {
                        ++height;
                    }
                }

                append_greedy_face(mesh, sample, block_registry, light, face_index, slice, u, v, width, height, mask[static_cast<std::size_t>(index)].key);
                ++face_count;

                for (int clear_v = 0; clear_v < height; ++clear_v) {
                    for (int clear_u = 0; clear_u < width; ++clear_u) {
                        mask[static_cast<std::size_t>(u + clear_u + (v + clear_v) * mask_width)].valid = false;
                    }
                }
                u += width;
            }
        }
    };

    const auto make_mask_cell = [&](int x, int y, int z, BlockId block, BlockId neighbor, int face_index, auto&& eligible) {
        MaskCell cell {};
        if (eligible(block, block_registry) && should_emit_face(block, neighbor, block_registry, face_index, leaves_mode)) {
            cell.valid = true;
            cell.key = make_face_key(block, kFaceTemplates[static_cast<std::size_t>(face_index)], block_registry);
            cell.light = make_face_light_key(sample, block_registry, light, block, face_index, x, y, z);
        }
        return cell;
    };

    const auto build_greedy_faces = [&](auto&& eligible, bool water_only) {
        std::vector<MaskCell> mask {};
        mask.resize(static_cast<std::size_t>(kChunkWidth * kChunkDepth));
        for (int y = work_range.min_y; y <= work_range.max_y; ++y) {
            if (!water_only) {
                std::fill(mask.begin(), mask.end(), MaskCell {});
                for (int z = 0; z < kChunkDepth; ++z) {
                    for (int x = 0; x < kChunkWidth; ++x) {
                        mask[static_cast<std::size_t>(x + z * kChunkWidth)] = make_mask_cell(x, y, z, sample(x, y, z), sample(x, y + 1, z), 0, eligible);
                    }
                }
                emit_greedy_mask(0, y, kChunkWidth, kChunkDepth, 0, kChunkDepth - 1, mask);

                std::fill(mask.begin(), mask.end(), MaskCell {});
                for (int z = 0; z < kChunkDepth; ++z) {
                    for (int x = 0; x < kChunkWidth; ++x) {
                        mask[static_cast<std::size_t>(x + z * kChunkWidth)] = make_mask_cell(x, y, z, sample(x, y, z), sample(x, y - 1, z), 1, eligible);
                    }
                }
                emit_greedy_mask(1, y, kChunkWidth, kChunkDepth, 0, kChunkDepth - 1, mask);
            } else {
                std::fill(mask.begin(), mask.end(), MaskCell {});
                for (int z = 0; z < kChunkDepth; ++z) {
                    for (int x = 0; x < kChunkWidth; ++x) {
                        mask[static_cast<std::size_t>(x + z * kChunkWidth)] = make_mask_cell(x, y, z, sample(x, y, z), sample(x, y + 1, z), 0, eligible);
                    }
                }
                emit_greedy_mask(0, y, kChunkWidth, kChunkDepth, 0, kChunkDepth - 1, mask);
            }
        }

        mask.resize(static_cast<std::size_t>(kChunkDepth * kChunkHeight));
        for (int x = 0; x < kChunkWidth; ++x) {
            std::fill(mask.begin(), mask.end(), MaskCell {});
            for (int y = work_range.min_y; y <= work_range.max_y; ++y) {
                for (int z = 0; z < kChunkDepth; ++z) {
                    mask[static_cast<std::size_t>(z + y * kChunkDepth)] = make_mask_cell(x, y, z, sample(x, y, z), sample(x + 1, y, z), 2, eligible);
                }
            }
            emit_greedy_mask(2, x, kChunkDepth, kChunkHeight, work_range.min_y, work_range.max_y, mask);

            std::fill(mask.begin(), mask.end(), MaskCell {});
            for (int y = work_range.min_y; y <= work_range.max_y; ++y) {
                for (int z = 0; z < kChunkDepth; ++z) {
                    mask[static_cast<std::size_t>(z + y * kChunkDepth)] = make_mask_cell(x, y, z, sample(x, y, z), sample(x - 1, y, z), 3, eligible);
                }
            }
            emit_greedy_mask(3, x, kChunkDepth, kChunkHeight, work_range.min_y, work_range.max_y, mask);
        }

        mask.resize(static_cast<std::size_t>(kChunkWidth * kChunkHeight));
        for (int z = 0; z < kChunkDepth; ++z) {
            std::fill(mask.begin(), mask.end(), MaskCell {});
            for (int y = work_range.min_y; y <= work_range.max_y; ++y) {
                for (int x = 0; x < kChunkWidth; ++x) {
                    mask[static_cast<std::size_t>(x + y * kChunkWidth)] = make_mask_cell(x, y, z, sample(x, y, z), sample(x, y, z + 1), 4, eligible);
                }
            }
            emit_greedy_mask(4, z, kChunkWidth, kChunkHeight, work_range.min_y, work_range.max_y, mask);

            std::fill(mask.begin(), mask.end(), MaskCell {});
            for (int y = work_range.min_y; y <= work_range.max_y; ++y) {
                for (int x = 0; x < kChunkWidth; ++x) {
                    mask[static_cast<std::size_t>(x + y * kChunkWidth)] = make_mask_cell(x, y, z, sample(x, y, z), sample(x, y, z - 1), 5, eligible);
                }
            }
            emit_greedy_mask(5, z, kChunkWidth, kChunkHeight, work_range.min_y, work_range.max_y, mask);
        }
    };

    build_greedy_faces(is_greedy_opaque_block, false);
    build_greedy_faces(
        [&](BlockId block, const BlockRegistry& registry) {
            return uses_greedy_cutout_meshing(block, registry, leaves_mode);
        },
        false
    );
    build_greedy_faces(is_water_block, true);

    std::vector<MaskCell> mask {};

    for (int y = work_range.min_y; y <= work_range.max_y; ++y) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int x = 0; x < kChunkWidth; ++x) {
                const BlockId block = sample(x, y, z);
                if (!block_registry.is_renderable(block) ||
                    is_greedy_opaque_block(block, block_registry) ||
                    uses_greedy_cutout_meshing(block, block_registry, leaves_mode) ||
                    is_water_block(block, block_registry)) {
                    continue;
                }

                const float world_x = static_cast<float>(x);
                const float world_y = static_cast<float>(local_y_to_world_y(y));
                const float world_z = static_cast<float>(z);

                for (const FaceTemplate& face : kFaceTemplates) {
                    const int face_index = static_cast<int>(&face - kFaceTemplates.data());
                    const BlockId neighbor = sample(x + face.neighbor_x, y + face.neighbor_y, z + face.neighbor_z);
                    if (!should_emit_face(block, neighbor, block_registry, face_index, leaves_mode)) {
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

                    const FaceLighting lighting = make_face_lighting(
                        sample,
                        block_registry,
                        light,
                        block,
                        face_index,
                        face.vertices,
                        face_color,
                        x,
                        y,
                        z
                    );
                    append_lit_face(mesh_section_for_block(mesh, block, block_registry), lighting, vertices, uvs, tex_index);
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

BlockId sample_block_for_mesh(const ChunkData& chunk_data, const ChunkMeshNeighbors& neighbors, int x, int y, int z) {
    if (y < 0 || y >= kChunkHeight) {
        return BlockId::Air;
    }
    if (x < 0 && z < 0) {
        return neighbors.northwest != nullptr
            ? neighbors.northwest->get(y)
            : BlockId::Air;
    }
    if (x >= kChunkWidth && z < 0) {
        return neighbors.northeast != nullptr
            ? neighbors.northeast->get(y)
            : BlockId::Air;
    }
    if (x < 0 && z >= kChunkDepth) {
        return neighbors.southwest != nullptr
            ? neighbors.southwest->get(y)
            : BlockId::Air;
    }
    if (x >= kChunkWidth && z >= kChunkDepth) {
        return neighbors.southeast != nullptr
            ? neighbors.southeast->get(y)
            : BlockId::Air;
    }
    if (x < 0) {
        return neighbors.west != nullptr && z >= 0 && z < kChunkDepth
            ? neighbors.west->get(y, z)
            : BlockId::Air;
    }
    if (x >= kChunkWidth) {
        return neighbors.east != nullptr && z >= 0 && z < kChunkDepth
            ? neighbors.east->get(y, z)
            : BlockId::Air;
    }
    if (z < 0) {
        return neighbors.north != nullptr && x >= 0 && x < kChunkWidth
            ? neighbors.north->get(x, y)
            : BlockId::Air;
    }
    if (z >= kChunkDepth) {
        return neighbors.south != nullptr && x >= 0 && x < kChunkWidth
            ? neighbors.south->get(x, y)
            : BlockId::Air;
    }
    return chunk_data.get(x, y, z);
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
    const ChunkMesh single_mesh = build_mesh_from_sampler(single_sample, block_registry, LeavesRenderMode::Fancy, &single_faces);
    assert(single_faces == 6);
    assert(single_mesh.opaque_mesh.vertices.size() == 24);
    assert(single_mesh.opaque_mesh.indices.size() == 36);

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
    const ChunkMesh double_mesh = build_mesh_from_sampler(double_sample, block_registry, LeavesRenderMode::Fancy, &double_faces);
    assert(double_faces == 6);
    assert(double_mesh.opaque_mesh.vertices.size() == 24);
    assert(double_mesh.opaque_mesh.indices.size() == 36);

    ChunkData ao_split_blocks {};
    ao_split_blocks.set(0, 0, 0, BlockId::Stone);
    ao_split_blocks.set(1, 0, 0, BlockId::Stone);
    const auto ao_split_sample = [&](int x, int y, int z) -> BlockId {
        if (y < 0 || y >= kChunkHeight) {
            return BlockId::Air;
        }
        if (x == 0 && y == 1 && z == -1) {
            return BlockId::Stone;
        }
        if (x < 0 || x >= kChunkWidth || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return ao_split_blocks.get(x, y, z);
    };
    std::size_t ao_split_faces = 0;
    const ChunkMesh ao_split_mesh = build_mesh_from_sampler(ao_split_sample, block_registry, LeavesRenderMode::Fancy, &ao_split_faces);
    assert(ao_split_faces > double_faces);
    assert(ao_split_mesh.opaque_mesh.vertices.size() > double_mesh.opaque_mesh.vertices.size());

    ChunkData slab {};
    for (int z = 0; z < 2; ++z) {
        for (int x = 0; x < 3; ++x) {
            slab.set(x, 0, z, BlockId::Stone);
        }
    }
    const auto slab_sample = [&](int x, int y, int z) -> BlockId {
        if (x < 0 || x >= kChunkWidth || y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return slab.get(x, y, z);
    };
    std::size_t slab_faces = 0;
    const ChunkMesh slab_mesh = build_mesh_from_sampler(slab_sample, block_registry, LeavesRenderMode::Fancy, &slab_faces);
    assert(slab_faces == 6);
    assert(slab_mesh.opaque_mesh.vertices.size() == 24);
    assert(slab_mesh.opaque_mesh.indices.size() == 36);
    bool found_tiled_bottom = false;
    for (std::size_t i = 0; i + 3 < slab_mesh.opaque_mesh.vertices.size(); i += 4) {
        const Vertex& a = slab_mesh.opaque_mesh.vertices[i + 0];
        const Vertex& b = slab_mesh.opaque_mesh.vertices[i + 1];
        const Vertex& c = slab_mesh.opaque_mesh.vertices[i + 2];
        const Vertex& d = slab_mesh.opaque_mesh.vertices[i + 3];
        if (a.position.y == static_cast<float>(kWorldMinY) &&
            b.position.y == static_cast<float>(kWorldMinY) &&
            c.position.y == static_cast<float>(kWorldMinY) &&
            d.position.y == static_cast<float>(kWorldMinY)) {
            const float uv_width = std::max({a.uv.x, b.uv.x, c.uv.x, d.uv.x}) - std::min({a.uv.x, b.uv.x, c.uv.x, d.uv.x});
            const float uv_height = std::max({a.uv.y, b.uv.y, c.uv.y, d.uv.y}) - std::min({a.uv.y, b.uv.y, c.uv.y, d.uv.y});
            found_tiled_bottom = uv_width == 3.0f && uv_height == 2.0f;
        }
    }
    assert(found_tiled_bottom);

    ChunkData seam_left {};
    seam_left.set(kChunkWidth - 1, 0, 0, BlockId::Stone);
    const auto seam_left_sample = [&](int x, int y, int z) -> BlockId {
        if (x < 0 || x >= kChunkWidth || y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return seam_left.get(x, y, z);
    };
    const ChunkMesh seam_left_mesh = build_mesh_from_sampler(seam_left_sample, block_registry, LeavesRenderMode::Fancy);

    ChunkData seam_right {};
    seam_right.set(0, 0, 0, BlockId::Stone);
    const auto seam_right_sample = [&](int x, int y, int z) -> BlockId {
        if (x < 0 || x >= kChunkWidth || y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return seam_right.get(x, y, z);
    };
    const ChunkMesh seam_right_mesh = build_mesh_from_sampler(seam_right_sample, block_registry, LeavesRenderMode::Fancy);

    const auto x_extent = [](const ChunkMesh& mesh) {
        float min_x = std::numeric_limits<float>::max();
        float max_x = std::numeric_limits<float>::lowest();
        const auto accumulate = [&](const MeshSection& section) {
            for (const Vertex& vertex : section.vertices) {
                min_x = std::min(min_x, vertex.position.x);
                max_x = std::max(max_x, vertex.position.x);
            }
        };
        accumulate(mesh.opaque_mesh);
        accumulate(mesh.cutout_mesh);
        accumulate(mesh.transparent_mesh);
        return std::array<float, 2> {min_x, max_x};
    };

    const auto left_extent = x_extent(seam_left_mesh);
    const auto right_extent = x_extent(seam_right_mesh);
    assert(left_extent[0] == static_cast<float>(kChunkWidth - 1));
    assert(left_extent[1] == static_cast<float>(kChunkWidth));
    assert(right_extent[0] == 0.0f);
    assert(right_extent[1] == 1.0f);

    const auto seam_left_neighbor_sample = [&](int x, int y, int z) -> BlockId {
        if (y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        if (x >= kChunkWidth) {
            return seam_right.get(0, y, z);
        }
        if (x < 0) {
            return BlockId::Air;
        }
        return seam_left.get(x, y, z);
    };
    std::size_t seam_neighbor_faces = 0;
    const ChunkMesh seam_neighbor_mesh = build_mesh_from_sampler(seam_left_neighbor_sample, block_registry, LeavesRenderMode::Fancy, &seam_neighbor_faces);
    assert(seam_neighbor_faces == 5);
    assert(seam_neighbor_mesh.opaque_mesh.vertices.size() == 20);
    assert(seam_neighbor_mesh.opaque_mesh.indices.size() == 30);

    ChunkSideBorderX seam_east_border {};
    for (int y = 0; y < kChunkHeight; ++y) {
        for (int z = 0; z < kChunkDepth; ++z) {
            seam_east_border.blocks[static_cast<std::size_t>(z + y * kChunkDepth)] = seam_right.get(0, y, z);
        }
    }
    const ChunkMeshNeighbors seam_neighbors {
        nullptr,
        &seam_east_border,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        nullptr
    };
    std::size_t seam_neighbor_mesh_faces = 0;
    const ChunkMesh seam_neighbor_mesh_from_snapshot = build_mesh_from_sampler(
        [&](int x, int y, int z) {
            return sample_block_for_mesh(seam_left, seam_neighbors, x, y, z);
        },
        block_registry,
        LeavesRenderMode::Fancy,
        &seam_neighbor_mesh_faces
    );
    assert(seam_neighbor_mesh_faces == 5);
    assert(seam_neighbor_mesh_from_snapshot.opaque_mesh.vertices.size() == 20);
    assert(seam_neighbor_mesh_from_snapshot.opaque_mesh.indices.size() == 30);

    ChunkData diagonal_ao_chunk {};
    diagonal_ao_chunk.set(0, 0, 0, BlockId::Stone);
    ChunkCornerBorder northwest_corner {};
    northwest_corner.blocks[1] = BlockId::Stone;
    const ChunkMeshNeighbors diagonal_ao_neighbors {
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        &northwest_corner,
        nullptr,
        nullptr,
        nullptr
    };
    const ChunkMesh diagonal_without_neighbor = build_chunk_mesh(diagonal_ao_chunk, {}, block_registry, {}, LeavesRenderMode::Fancy);
    const ChunkMesh diagonal_with_neighbor = build_chunk_mesh(diagonal_ao_chunk, {}, block_registry, diagonal_ao_neighbors, LeavesRenderMode::Fancy);
    const auto color_sum = [](const ChunkMesh& mesh) {
        float sum = 0.0f;
        for (const Vertex& vertex : mesh.opaque_mesh.vertices) {
            sum += vertex.color.x + vertex.color.y + vertex.color.z;
        }
        return sum;
    };
    assert(diagonal_with_neighbor.opaque_mesh.indices.size() == diagonal_without_neighbor.opaque_mesh.indices.size());
    assert(color_sum(diagonal_with_neighbor) < color_sum(diagonal_without_neighbor));

    ChunkData leaves_pair {};
    leaves_pair.set(0, 0, 0, BlockId::OakLeaves);
    leaves_pair.set(1, 0, 0, BlockId::OakLeaves);
    const auto leaves_pair_sample = [&](int x, int y, int z) -> BlockId {
        if (x < 0 || x >= kChunkWidth || y < 0 || y >= kChunkHeight || z < 0 || z >= kChunkDepth) {
            return BlockId::Air;
        }
        return leaves_pair.get(x, y, z);
    };
    std::size_t leaves_fast_faces = 0;
    const ChunkMesh leaves_fast_mesh = build_mesh_from_sampler(leaves_pair_sample, block_registry, LeavesRenderMode::Fast, &leaves_fast_faces);
    std::size_t leaves_fancy_faces = 0;
    const ChunkMesh leaves_fancy_mesh = build_mesh_from_sampler(leaves_pair_sample, block_registry, LeavesRenderMode::Fancy, &leaves_fancy_faces);
    assert(leaves_fast_faces == 6);
    assert(leaves_fast_mesh.cutout_mesh.indices.size() == 36);
    assert(leaves_fancy_faces == 12);
    assert(leaves_fancy_mesh.cutout_mesh.indices.size() == 72);

    log_message(LogLevel::Info, "WorldGenerator: seam self-check passed, chunk boundary blocks remain 1x1x1");
}

}

WorldGenerator::WorldGenerator(const BlockRegistry& block_registry)
    : block_registry_(block_registry) {
}

int WorldGenerator::surface_height_at(int world_x, int world_z, WorldSeed seed) const {
    return static_cast<int>(sample_height(world_x, world_z, seed));
}

ChunkData WorldGenerator::generate_chunk(ChunkCoord coord, WorldSeed seed) const {
    ChunkData chunk {};
    std::array<ColumnProfile, static_cast<std::size_t>(kChunkWidth * kChunkDepth)> profiles {};

    for (int z = 0; z < kChunkDepth; ++z) {
        for (int x = 0; x < kChunkWidth; ++x) {
            const int world_x = coord.x * kChunkWidth + x;
            const int world_z = coord.z * kChunkDepth + z;
            ColumnProfile& profile = profiles[column_index(x, z)];
            profile.surface_y = static_cast<int>(sample_height(world_x, world_z, seed));
            profile.continentalness = sample_continentalness(world_x, world_z, seed);
            profile.deep_ocean = profile.continentalness < -0.45f;
            profile.cave_min_y = kCaveMinY;
            profile.cave_max_y = std::min(profile.surface_y - 2, kCaveMaxY);
            const float cave_mask = smooth_noise(
                static_cast<float>(world_x) * 0.018f,
                static_cast<float>(world_z) * 0.018f,
                seed ^ 0xCAFECA5ECAFECA5Eull
            );
            profile.cave_candidate = profile.cave_max_y >= profile.cave_min_y && cave_mask > 0.18f;

            const int column_top_y = std::max(profile.surface_y, kSeaLevel);
            const int max_local_y = world_y_to_local_y(std::clamp(column_top_y, kWorldMinY, kWorldMaxY));
            for (int local_y = 0; local_y <= max_local_y; ++local_y) {
                const int world_y = local_y_to_world_y(local_y);
                if (world_y > profile.surface_y) {
                    if (world_y <= kSeaLevel) {
                        chunk.set(x, local_y, z, BlockId::Water);
                    }
                    continue;
                }

                if (world_y <= kWorldMinY + 2) {
                    chunk.set(x, local_y, z, BlockId::Stone);
                } else if (world_y == profile.surface_y) {
                    if (world_y <= kSeaLevel + 1) {
                        chunk.set(x, local_y, z, profile.deep_ocean ? BlockId::Gravel : BlockId::Sand);
                    } else {
                        chunk.set(x, local_y, z, BlockId::Grass);
                    }
                } else if (world_y > profile.surface_y - 4) {
                    if (profile.surface_y <= kSeaLevel + 1) {
                        chunk.set(x, local_y, z, profile.deep_ocean ? BlockId::Gravel : BlockId::Sand);
                    } else {
                        chunk.set(x, local_y, z, BlockId::Dirt);
                    }
                } else {
                    chunk.set(x, local_y, z, BlockId::Stone);
                }
            }
        }
    }

    for (int z = 0; z < kChunkDepth; ++z) {
        for (int x = 0; x < kChunkWidth; ++x) {
            const ColumnProfile& profile = profiles[column_index(x, z)];
            if (!profile.cave_candidate) {
                continue;
            }
            const int world_x = coord.x * kChunkWidth + x;
            const int world_z = coord.z * kChunkDepth + z;
            const bool ocean_floor_guard_column = profile.continentalness < -0.16f;
            const int min_local_y = world_y_to_local_y(std::max(profile.cave_min_y, kWorldMinY));
            const int max_local_y = world_y_to_local_y(std::min(profile.cave_max_y, kWorldMaxY));
            for (int local_y = min_local_y; local_y <= max_local_y; ++local_y) {
                const int world_y = local_y_to_world_y(local_y);
                BlockId block = chunk.get(x, local_y, z);
                if (block == BlockId::Air || block == BlockId::Water) {
                    continue;
                }
                if (ocean_floor_guard_column && world_y > profile.surface_y - 8) {
                    continue;
                }
                if (!should_carve_cave(world_x, world_y, world_z, profile.surface_y, seed)) {
                    continue;
                }

                if (world_y <= kAquiferMaxY) {
                    const int aquifer_level = sample_aquifer_level(world_x, world_y, world_z, seed);
                    const float aquifer_presence = smooth_noise3d(
                        static_cast<float>(world_x) * 0.012f,
                        static_cast<float>(world_y) * 0.010f,
                        static_cast<float>(world_z) * 0.012f,
                        seed ^ 0xFA117EDC0DEull
                    );
                    chunk.set(x, local_y, z, (world_y <= aquifer_level && aquifer_presence > 0.36f) ? BlockId::Water : BlockId::Air);
                } else {
                    chunk.set(x, local_y, z, BlockId::Air);
                }
            }
        }
    }
    apply_underwater_gravel_bottom(chunk, coord, kSeaLevel, seed);
    apply_shore_gravel_disks(chunk, coord, kSeaLevel, seed);
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
            if (!contains_world_y(surface_y) || surface_y > water_level - kUnderwaterDepthMin) {
                continue;
            }
            const int surface_local_y = world_y_to_local_y(surface_y);

            const float clump = smooth_noise(static_cast<float>(world_x) * 0.13f, static_cast<float>(world_z) * 0.13f, seed ^ 0xA0C7B157BEEFull);
            const float edge = smooth_noise(static_cast<float>(world_x) * 0.31f, static_cast<float>(world_z) * 0.31f, seed ^ 0x6C8E9CF570932BD5ull);
            if (clump + edge * 0.35f < 0.78f) {
                continue;
            }

            if (can_replace_surface_with_gravel(chunk.get(x, surface_local_y, z)) || chunk.get(x, surface_local_y, z) == BlockId::Stone) {
                chunk.set(x, surface_local_y, z, BlockId::Gravel);
            }
            if (surface_y > kWorldMinY && chunk.get(x, world_y_to_local_y(surface_y - 1), z) == BlockId::Sand && hash_noise(world_x, world_z, seed ^ 0xB0770B077ull) > 0.45f) {
                chunk.set(x, world_y_to_local_y(surface_y - 1), z, BlockId::Gravel);
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
                    if (!contains_world_y(surface_y)) {
                        continue;
                    }
                    if (surface_y < water_level + kShoreCandidateMinYOffset || surface_y > water_level + kShoreCandidateMaxYOffset) {
                        continue;
                    }
                    if (!is_water_adjacent_or_submerged_surface(world_x, world_z, surface_y, water_level, seed)) {
                        continue;
                    }
                    const int surface_local_y = world_y_to_local_y(surface_y);
                    if (can_replace_surface_with_gravel(chunk.get(x, surface_local_y, z))) {
                        chunk.set(x, surface_local_y, z, BlockId::Gravel);
                    }
                    if (surface_y > kWorldMinY && chunk.get(x, world_y_to_local_y(surface_y - 1), z) == BlockId::Sand && hash_noise(world_x * 3, world_z * 5, seed ^ 0x5A7D5A7Dull) > 0.62f) {
                        chunk.set(x, world_y_to_local_y(surface_y - 1), z, BlockId::Gravel);
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
                const int center_y = random_range(kWorldMinY + 12, 120, hash_noise(seed_x, seed_z, seed ^ 0x7F4A7C159E3779B9ull));
                const float radius = 1.5f + hash_noise(seed_x, seed_z, seed ^ 0x0DDC0FFEEC0FFEE0ull) * 1.7f;
                const float radius_sq = radius * radius;

                const int min_x = std::max(0, center_x - max_radius - coord.x * kChunkWidth);
                const int max_x = std::min(kChunkWidth - 1, center_x + max_radius - coord.x * kChunkWidth);
                const int min_z = std::max(0, center_z - max_radius - coord.z * kChunkDepth);
                const int max_z = std::min(kChunkDepth - 1, center_z + max_radius - coord.z * kChunkDepth);
                const int min_y = std::max(kWorldMinY, center_y - max_radius);
                const int max_y = std::min(kWorldMaxY, center_y + max_radius);

                for (int y = min_y; y <= max_y; ++y) {
                    const int local_y = world_y_to_local_y(y);
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
                            if (chunk.get(x, local_y, z) == BlockId::Stone) {
                                chunk.set(x, local_y, z, BlockId::Gravel);
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
    if (ground_y < kWorldMinY + 1 || ground_y + 7 > kWorldMaxY || ground_y <= kSeaLevel + 1) {
        return false;
    }

    const int local_surface = static_cast<int>(sample_height(base_world_x, base_world_z, seed));
    if (local_surface != ground_y) {
        return false;
    }

    for (int dz = -2; dz <= 2; ++dz) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int neighbor_height = static_cast<int>(sample_height(base_world_x + dx, base_world_z + dz, seed));
            if (neighbor_height <= kSeaLevel + 1) {
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
        if (local_x < 0 || local_x >= kChunkWidth || local_z < 0 || local_z >= kChunkDepth || !contains_world_y(y)) {
            return;
        }
        const int local_y = world_y_to_local_y(y);

        const BlockId existing = chunk.get(local_x, local_y, local_z);
        if (block == BlockId::OakLog) {
            if (existing == BlockId::Air || existing == BlockId::OakLeaves) {
                chunk.set(local_x, local_y, local_z, block);
            }
            return;
        }

        if (existing == BlockId::Air) {
            chunk.set(local_x, local_y, local_z, block);
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

    const float continentalness = sample_continentalness(world_x, world_z, seed);
    const float erosion = smooth_noise(x * 0.0065f, z * 0.0065f, seed ^ 0x12345678ull) * 2.0f - 1.0f;
    const float hills = smooth_noise(x * 0.018f, z * 0.018f, seed ^ 0x23456789ull) * 2.0f - 1.0f;
    const float roughness = smooth_noise(x * 0.048f, z * 0.048f, seed ^ 0x3456789Aull) * 2.0f - 1.0f;
    const float detail = smooth_noise(x * 0.11f, z * 0.11f, seed ^ 0x87654321ull) * 2.0f - 1.0f;

    float height = static_cast<float>(kSeaLevel);
    if (continentalness < -0.45f) {
        const float t = std::clamp((continentalness + 1.0f) / 0.55f, 0.0f, 1.0f);
        height = static_cast<float>(kSeaLevel) - 46.0f + t * 18.0f + roughness * 3.0f;
    } else if (continentalness < -0.16f) {
        const float t = (continentalness + 0.45f) / 0.29f;
        height = static_cast<float>(kSeaLevel) - 26.0f + t * 18.0f + roughness * 4.0f;
    } else if (continentalness < 0.08f) {
        const float t = (continentalness + 0.16f) / 0.24f;
        height = static_cast<float>(kSeaLevel) - 5.0f + t * 9.0f + erosion * 2.0f + detail * 1.5f;
    } else {
        const float land = std::clamp((continentalness - 0.08f) / 0.92f, 0.0f, 1.0f);
        height = static_cast<float>(kSeaLevel) + 5.0f + land * 82.0f + hills * (10.0f + land * 24.0f) + roughness * 6.0f + detail * 2.0f;
    }

    return std::clamp(height, static_cast<float>(kWorldMinY + 6), static_cast<float>(kWorldMaxY - 24));
}

float WorldGenerator::sample_continentalness(int world_x, int world_z, WorldSeed seed) const {
    const float x = static_cast<float>(world_x);
    const float z = static_cast<float>(world_z);

    const float continents = smooth_noise(x * 0.0022f, z * 0.0022f, seed ^ 0xC01171E477E55ull) * 2.0f - 1.0f;
    const float islands = smooth_noise(x * 0.006f, z * 0.006f, seed ^ 0x15A11D5BEEFull) * 2.0f - 1.0f;
    const float coast_noise = smooth_noise(x * 0.014f, z * 0.014f, seed ^ 0xC0A57A17ull) * 2.0f - 1.0f;

    return std::clamp(continents * 0.82f + islands * 0.26f + coast_noise * 0.10f, -1.0f, 1.0f);
}

float WorldGenerator::sample_cave_density(int world_x, int world_y, int world_z, WorldSeed seed) const {
    const float x = static_cast<float>(world_x);
    const float y = static_cast<float>(world_y);
    const float z = static_cast<float>(world_z);

    const float cheese_a = smooth_noise3d(x * 0.030f, y * 0.024f, z * 0.030f, seed ^ 0xC4A5E0000000001ull);
    const float cheese_b = smooth_noise3d(x * 0.052f, y * 0.040f, z * 0.052f, seed ^ 0xC4A5E0000000002ull);
    const float cheese_detail = smooth_noise3d(x * 0.095f, y * 0.080f, z * 0.095f, seed ^ 0xC4A5E0000000003ull);
    const float cheese_density = cheese_a * 0.62f + cheese_b * 0.28f + cheese_detail * 0.10f;

    const float tunnel_x = std::abs(smooth_noise3d(x * 0.019f, y * 0.036f, z * 0.019f, seed ^ 0x5EA9E771ull) - 0.5f);
    const float tunnel_z = std::abs(smooth_noise3d(x * 0.021f, y * 0.033f, z * 0.021f, seed ^ 0x5EA9E772ull) - 0.5f);
    const float spaghetti = 0.078f - std::max(tunnel_x, tunnel_z);

    const float noodle_a = std::abs(smooth_noise3d(x * 0.052f, y * 0.060f, z * 0.052f, seed ^ 0x900D1E0000000001ull) - 0.5f);
    const float noodle_b = std::abs(smooth_noise3d(x * 0.047f, y * 0.055f, z * 0.047f, seed ^ 0x900D1E0000000002ull) - 0.5f);
    const float noodle = 0.030f - std::max(noodle_a, noodle_b);

    const float depth = std::clamp((static_cast<float>(kCaveMaxY - world_y) / static_cast<float>(kCaveMaxY - kCaveMinY)), 0.0f, 1.0f);
    const float depth_bias = depth * 0.075f;
    const float cheese = (0.365f + depth_bias) - cheese_density;
    return std::max({cheese, spaghetti + depth_bias * 0.25f, noodle + depth_bias * 0.15f});
}

int WorldGenerator::sample_aquifer_level(int world_x, int world_y, int world_z, WorldSeed seed) const {
    const float local = smooth_noise3d(
        static_cast<float>(world_x) * 0.018f,
        static_cast<float>(world_y) * 0.014f,
        static_cast<float>(world_z) * 0.018f,
        seed ^ 0xA901FEA901FEull
    );
    return std::clamp(kSeaLevel - 30 + static_cast<int>(local * 24.0f), kAquiferMinY, kSeaLevel - 2);
}

bool WorldGenerator::should_carve_cave(int world_x, int world_y, int world_z, int surface_y, WorldSeed seed) const {
    if (world_y < kCaveMinY || world_y > kCaveMaxY || world_y > surface_y - 2) {
        return false;
    }
    if (surface_y - world_y < kSurfaceCaveProtectionDepth) {
        const float mouth = smooth_noise3d(
            static_cast<float>(world_x) * 0.06f,
            static_cast<float>(world_y) * 0.08f,
            static_cast<float>(world_z) * 0.06f,
            seed ^ 0xE4712A9E4712A9ull
        );
        if (mouth < 0.86f) {
            return false;
        }
    }

    return sample_cave_density(world_x, world_y, world_z, seed) > 0.0f;
}

void WorldGenerator::apply_caves_and_aquifers(ChunkData& chunk, ChunkCoord coord, WorldSeed seed) const {
    for (int z = 0; z < kChunkDepth; ++z) {
        for (int x = 0; x < kChunkWidth; ++x) {
            const int world_x = coord.x * kChunkWidth + x;
            const int world_z = coord.z * kChunkDepth + z;
            const int surface_y = static_cast<int>(sample_height(world_x, world_z, seed));
            const float ocean_connectivity = sample_continentalness(world_x, world_z, seed);

            for (int local_y = 0; local_y < kChunkHeight; ++local_y) {
                const int world_y = local_y_to_world_y(local_y);
                BlockId block = chunk.get(x, local_y, z);
                if (block == BlockId::Air || block == BlockId::Water || block == BlockId::OakLog || block == BlockId::OakLeaves) {
                    continue;
                }
                if (!should_carve_cave(world_x, world_y, world_z, surface_y, seed)) {
                    continue;
                }

                const bool ocean_floor_guard = ocean_connectivity < -0.16f && world_y > surface_y - 8;
                if (ocean_floor_guard) {
                    continue;
                }

                const int aquifer_level = sample_aquifer_level(world_x, world_y, world_z, seed);
                const float aquifer_presence = smooth_noise3d(
                    static_cast<float>(world_x) * 0.012f,
                    static_cast<float>(world_y) * 0.010f,
                    static_cast<float>(world_z) * 0.012f,
                    seed ^ 0xFA117EDC0DEull
                );
                if (world_y <= aquifer_level && world_y <= kAquiferMaxY && aquifer_presence > 0.36f) {
                    chunk.set(x, local_y, z, BlockId::Water);
                } else {
                    chunk.set(x, local_y, z, BlockId::Air);
                }
            }
        }
    }
}

ChunkMesh build_chunk_mesh(const ChunkData& chunk_data, ChunkCoord coord, const BlockRegistry& block_registry, LeavesRenderMode leaves_mode) {
    return build_chunk_mesh(chunk_data, coord, block_registry, {}, leaves_mode);
}

ChunkMesh build_chunk_mesh(
    const ChunkData& chunk_data,
    ChunkCoord coord,
    const BlockRegistry& block_registry,
    const ChunkMeshNeighbors& neighbors,
    LeavesRenderMode leaves_mode) {
    (void)coord;
    run_mesh_builder_self_check(block_registry);

    const auto sample = [&](int x, int y, int z) -> BlockId {
        return sample_block_for_mesh(chunk_data, neighbors, x, y, z);
    };

    return build_mesh_from_sampler(sample, block_registry, leaves_mode);
}

}
