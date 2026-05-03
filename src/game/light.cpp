#include "game/light.hpp"

#include <algorithm>
#include <vector>

namespace ml {

namespace {

struct LightNode {
    int x {0};
    int y {0};
    int z {0};
};

constexpr int kPaddedMinX = -kLightBorder;
constexpr int kPaddedMinZ = -kLightBorder;
constexpr int kPaddedWidth = kChunkWidth + kLightBorder * 2;
constexpr int kPaddedDepth = kChunkDepth + kLightBorder * 2;



struct PaddedSkyLight {
    std::vector<std::uint8_t> sky;
    std::vector<std::uint8_t> block;

    PaddedSkyLight()
        : sky(static_cast<std::size_t>(kPaddedWidth * kPaddedDepth * kChunkHeight), 0)
        , block(static_cast<std::size_t>(kPaddedWidth * kPaddedDepth * kChunkHeight), 0) {
    }

    static std::size_t index(int x, int y, int z) {
        const int px = x - kPaddedMinX;
        const int pz = z - kPaddedMinZ;
        return static_cast<std::size_t>(px + kPaddedWidth * (pz + y * kPaddedDepth));
    }

    std::uint8_t get(int x, int y, int z) const {
        return sky[index(x, y, z)];
    }

    void set(int x, int y, int z, std::uint8_t value) {
        sky[index(x, y, z)] = value;
    }

    std::uint8_t get_block(int x, int y, int z) const {
        return block[index(x, y, z)];
    }

    void set_block(int x, int y, int z, std::uint8_t value) {
        block[index(x, y, z)] = value;
    }
};


bool in_padded_xz(int x, int z) {
    return x >= kPaddedMinX && x < kChunkWidth + kLightBorder &&
        z >= kPaddedMinZ && z < kChunkDepth + kLightBorder;
}

BlockId sample_block_for_light(const LightBuildSnapshot& snapshot, int x, int y, int z) {
    if (y < 0 || y >= kChunkHeight) {
        return BlockId::Air;
    }
    if (x >= 0 && x < kChunkWidth && z >= 0 && z < kChunkDepth) {
        return snapshot.chunk.get(x, y, z);
    }

    if (x < 0 && z < 0) {
        return snapshot.northwest.has_value() && x >= -kLightBorder && z >= -kLightBorder
            ? snapshot.northwest->get(x + kLightBorder, y, z + kLightBorder)
            : BlockId::Air;
    }
    if (x >= kChunkWidth && z < 0) {
        return snapshot.northeast.has_value() && x < kChunkWidth + kLightBorder && z >= -kLightBorder
            ? snapshot.northeast->get(x - kChunkWidth, y, z + kLightBorder)
            : BlockId::Air;
    }
    if (x < 0 && z >= kChunkDepth) {
        return snapshot.southwest.has_value() && x >= -kLightBorder && z < kChunkDepth + kLightBorder
            ? snapshot.southwest->get(x + kLightBorder, y, z - kChunkDepth)
            : BlockId::Air;
    }
    if (x >= kChunkWidth && z >= kChunkDepth) {
        return snapshot.southeast.has_value() && x < kChunkWidth + kLightBorder && z < kChunkDepth + kLightBorder
            ? snapshot.southeast->get(x - kChunkWidth, y, z - kChunkDepth)
            : BlockId::Air;
    }
    if (x < 0) {
        return snapshot.west.has_value() && x >= -kLightBorder && z >= 0 && z < kChunkDepth
            ? snapshot.west->get(x + kLightBorder, y, z)
            : BlockId::Air;
    }
    if (x >= kChunkWidth) {
        return snapshot.east.has_value() && x < kChunkWidth + kLightBorder && z >= 0 && z < kChunkDepth
            ? snapshot.east->get(x - kChunkWidth, y, z)
            : BlockId::Air;
    }
    if (z < 0) {
        return snapshot.north.has_value() && x >= 0 && x < kChunkWidth && z >= -kLightBorder
            ? snapshot.north->get(x, y, z + kLightBorder)
            : BlockId::Air;
    }
    if (z >= kChunkDepth) {
        return snapshot.south.has_value() && x >= 0 && x < kChunkWidth && z < kChunkDepth + kLightBorder
            ? snapshot.south->get(x, y, z - kChunkDepth)
            : BlockId::Air;
    }
    return BlockId::Air;
}

bool transmits_light(BlockId block, const BlockRegistry& block_registry) {
    return block_registry.light_dampening(block) < 15;
}

std::uint8_t propagated_light(std::uint8_t current, BlockId target, const BlockRegistry& block_registry, bool vertical_sky) {
    if (!transmits_light(target, block_registry)) {
        return 0;
    }
    if (vertical_sky && current == 15 && target == BlockId::Air) {
        return 15;
    }
    const std::uint8_t dampening = std::max<std::uint8_t>(1, block_registry.light_dampening(target));
    return current > dampening ? static_cast<std::uint8_t>(current - dampening) : 0;
}

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void hash_u8(std::uint64_t& hash, std::uint8_t value) {
    hash ^= static_cast<std::uint64_t>(value);
    hash *= kFnvPrime;
}

std::uint64_t compute_border_signature(const ChunkLight& light) {
    std::uint64_t hash = kFnvOffset;

    for (int y = 0; y < kChunkHeight; ++y) {
        for (int z = 0; z < kChunkDepth; ++z) {
            hash_u8(hash, light.sky(0, y, z));
            hash_u8(hash, light.block(0, y, z));
            hash_u8(hash, light.sky(kChunkWidth - 1, y, z));
            hash_u8(hash, light.block(kChunkWidth - 1, y, z));
        }
        for (int x = 0; x < kChunkWidth; ++x) {
            hash_u8(hash, light.sky(x, y, 0));
            hash_u8(hash, light.block(x, y, 0));
            hash_u8(hash, light.sky(x, y, kChunkDepth - 1));
            hash_u8(hash, light.block(x, y, kChunkDepth - 1));
        }
    }

    return hash;
}

}


ChunkLightResult calculate_chunk_light(const LightBuildSnapshot& snapshot, const BlockRegistry& block_registry) {
    ChunkLightResult result {};
    ChunkLight& light = result.light;
    PaddedSkyLight padded {};
    std::vector<LightNode> sky_queue;
    std::vector<LightNode> block_queue;
    sky_queue.reserve(static_cast<std::size_t>(kPaddedWidth * kPaddedDepth * 8));
    block_queue.reserve(static_cast<std::size_t>(kPaddedWidth * kPaddedDepth * 8));

    const auto push_sky_if_brighter = [&](int x, int y, int z, std::uint8_t value, bool vertical_sky) {
        if (!in_padded_xz(x, z) || y < 0 || y >= kChunkHeight || value == 0) {
            return;
        }
        const BlockId target = sample_block_for_light(snapshot, x, y, z);
        const std::uint8_t next = vertical_sky ? propagated_light(value, target, block_registry, true) : propagated_light(value, target, block_registry, false);
        if (next == 0 || padded.get(x, y, z) >= next) {
            return;
        }
        padded.set(x, y, z, next);
        sky_queue.push_back({x, y, z});
    };

    const auto push_block_if_brighter = [&](int x, int y, int z, std::uint8_t value) {
        if (!in_padded_xz(x, z) || y < 0 || y >= kChunkHeight || value == 0) {
            return;
        }
        const BlockId target = sample_block_for_light(snapshot, x, y, z);
        const std::uint8_t next = propagated_light(value, target, block_registry, false);
        if (next == 0 || padded.get_block(x, y, z) >= next) {
            return;
        }
        padded.set_block(x, y, z, next);
        block_queue.push_back({x, y, z});
    };

    for (int z = kPaddedMinZ; z < kChunkDepth + kLightBorder; ++z) {
        for (int x = kPaddedMinX; x < kChunkWidth + kLightBorder; ++x) {
            std::uint8_t column_light = 15;
            for (int y = kChunkHeight - 1; y >= 0; --y) {
                const BlockId block = sample_block_for_light(snapshot, x, y, z);
                if (!transmits_light(block, block_registry)) {
                    column_light = 0;
                    continue;
                }
                if (column_light > padded.get(x, y, z)) {
                    padded.set(x, y, z, column_light);
                    sky_queue.push_back({x, y, z});
                }
                if (block != BlockId::Air) {
                    const std::uint8_t dampening = std::max<std::uint8_t>(1, block_registry.light_dampening(block));
                    column_light = column_light > dampening ? static_cast<std::uint8_t>(column_light - dampening) : 0;
                }
            }
        }
    }

    for (std::size_t read = 0; read < sky_queue.size(); ++read) {
        const LightNode node = sky_queue[read];
        const std::uint8_t current = padded.get(node.x, node.y, node.z);
        if (current <= 1) {
            continue;
        }
        push_sky_if_brighter(node.x + 1, node.y, node.z, current, false);
        push_sky_if_brighter(node.x - 1, node.y, node.z, current, false);
        push_sky_if_brighter(node.x, node.y, node.z + 1, current, false);
        push_sky_if_brighter(node.x, node.y, node.z - 1, current, false);
        push_sky_if_brighter(node.x, node.y + 1, node.z, current, false);
        push_sky_if_brighter(node.x, node.y - 1, node.z, current, false);
    }

    for (int y = 0; y < kChunkHeight; ++y) {
        for (int z = kPaddedMinZ; z < kChunkDepth + kLightBorder; ++z) {
            for (int x = kPaddedMinX; x < kChunkWidth + kLightBorder; ++x) {
                const std::uint8_t emission = block_registry.light_emission(sample_block_for_light(snapshot, x, y, z));
                if (emission > padded.get_block(x, y, z)) {
                    padded.set_block(x, y, z, emission);
                    block_queue.push_back({x, y, z});
                }
            }
        }
    }

    for (std::size_t read = 0; read < block_queue.size(); ++read) {
        const LightNode node = block_queue[read];
        const std::uint8_t current = padded.get_block(node.x, node.y, node.z);
        if (current <= 1) {
            continue;
        }
        push_block_if_brighter(node.x + 1, node.y, node.z, current);
        push_block_if_brighter(node.x - 1, node.y, node.z, current);
        push_block_if_brighter(node.x, node.y, node.z + 1, current);
        push_block_if_brighter(node.x, node.y, node.z - 1, current);
        push_block_if_brighter(node.x, node.y + 1, node.z, current);
        push_block_if_brighter(node.x, node.y - 1, node.z, current);
    }

    for (int y = 0; y < kChunkHeight; ++y) {
        for (int z = 0; z < kChunkDepth; ++z) {
            for (int x = 0; x < kChunkWidth; ++x) {
                light.set_sky(x, y, z, padded.get(x, y, z));
                light.set_block(x, y, z, padded.get_block(x, y, z));
            }
        }
    }

    light.dirty = false;
    light.borders_ready = snapshot.complete_borders;
    light.border_signature = compute_border_signature(light);
    result.provisional = !snapshot.complete_borders;
    result.borders_ready = light.borders_ready;
    result.border_signature = light.border_signature;
    return result;
}


std::uint8_t sample_sky_light(const LightMeshSnapshot& snapshot, int x, int y, int z) {
    if (y >= kChunkHeight) {
        return 15;
    }
    if (y < 0 || snapshot.center == nullptr) {
        return 0;
    }
    if (x >= 0 && x < kChunkWidth && z >= 0 && z < kChunkDepth) {
        return snapshot.center->sky(x, y, z);
    }

    const auto fallback_center_edge_light = [&]() -> std::uint8_t {
        return snapshot.center->sky(
            std::clamp(x, 0, kChunkWidth - 1),
            y,
            std::clamp(z, 0, kChunkDepth - 1)
        );
    };

    if (x < 0 && z < 0) {
        return snapshot.northwest != nullptr && x >= -kLightBorder && z >= -kLightBorder
            ? snapshot.northwest->get(x + kLightBorder, y, z + kLightBorder)
            : fallback_center_edge_light();
    }
    if (x >= kChunkWidth && z < 0) {
        return snapshot.northeast != nullptr && x < kChunkWidth + kLightBorder && z >= -kLightBorder
            ? snapshot.northeast->get(x - kChunkWidth, y, z + kLightBorder)
            : fallback_center_edge_light();
    }
    if (x < 0 && z >= kChunkDepth) {
        return snapshot.southwest != nullptr && x >= -kLightBorder && z < kChunkDepth + kLightBorder
            ? snapshot.southwest->get(x + kLightBorder, y, z - kChunkDepth)
            : fallback_center_edge_light();
    }
    if (x >= kChunkWidth && z >= kChunkDepth) {
        return snapshot.southeast != nullptr && x < kChunkWidth + kLightBorder && z < kChunkDepth + kLightBorder
            ? snapshot.southeast->get(x - kChunkWidth, y, z - kChunkDepth)
            : fallback_center_edge_light();
    }
    if (x < 0) {
        return snapshot.west != nullptr && x >= -kLightBorder && z >= 0 && z < kChunkDepth
            ? snapshot.west->get(x + kLightBorder, y, z)
            : fallback_center_edge_light();
    }
    if (x >= kChunkWidth) {
        return snapshot.east != nullptr && x < kChunkWidth + kLightBorder && z >= 0 && z < kChunkDepth
            ? snapshot.east->get(x - kChunkWidth, y, z)
            : fallback_center_edge_light();
    }
    if (z < 0) {
        return snapshot.north != nullptr && x >= 0 && x < kChunkWidth && z >= -kLightBorder
            ? snapshot.north->get(x, y, z + kLightBorder)
            : fallback_center_edge_light();
    }
    if (z >= kChunkDepth) {
        return snapshot.south != nullptr && x >= 0 && x < kChunkWidth && z < kChunkDepth + kLightBorder
            ? snapshot.south->get(x, y, z - kChunkDepth)
            : fallback_center_edge_light();
    }
    return fallback_center_edge_light();
}


}
