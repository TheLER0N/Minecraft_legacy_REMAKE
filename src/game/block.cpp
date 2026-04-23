#include "game/block.hpp"

namespace ml {

BlockRegistry::BlockRegistry() {
    defs_[static_cast<std::size_t>(BlockId::Air)] = {BlockId::Air, "air", BlockFlags::None, {0.0f, 0.0f, 0.0f}, 0, 0, 0, 0.0f};
    defs_[static_cast<std::size_t>(BlockId::Grass)] = {BlockId::Grass, "grass", BlockFlags::Opaque | BlockFlags::Solid, {0.32f, 0.72f, 0.24f}, 1, 0, 2, 0.5f};
    defs_[static_cast<std::size_t>(BlockId::Dirt)] = {BlockId::Dirt, "dirt", BlockFlags::Opaque | BlockFlags::Solid, {0.50f, 0.31f, 0.16f}, 0, 0, 0, 0.5f};
    defs_[static_cast<std::size_t>(BlockId::Stone)] = {BlockId::Stone, "stone", BlockFlags::Opaque | BlockFlags::Solid, {0.55f, 0.58f, 0.60f}, 3, 3, 3, 1.5f};
    defs_[static_cast<std::size_t>(BlockId::Water)] = {BlockId::Water, "water", BlockFlags::None, {0.18f, 0.36f, 0.80f}, 4, 4, 4, 0.0f};
    defs_[static_cast<std::size_t>(BlockId::Sand)] = {BlockId::Sand, "sand", BlockFlags::Opaque | BlockFlags::Solid, {0.86f, 0.84f, 0.60f}, 5, 5, 5, 0.5f};
    defs_[static_cast<std::size_t>(BlockId::Gravel)] = {BlockId::Gravel, "gravel", BlockFlags::Opaque | BlockFlags::Solid, {0.52f, 0.51f, 0.50f}, 6, 6, 6, 0.6f};
    defs_[static_cast<std::size_t>(BlockId::OakLog)] = {BlockId::OakLog, "oak_log", BlockFlags::Opaque | BlockFlags::Solid, {0.60f, 0.42f, 0.24f}, 8, 8, 7, 2.0f};
    defs_[static_cast<std::size_t>(BlockId::OakLeaves)] = {BlockId::OakLeaves, "oak_leaves", BlockFlags::Solid, {0.42f, 0.72f, 0.24f}, 9, 9, 9, 0.2f};
}

const BlockDef& BlockRegistry::get(BlockId id) const {
    return defs_[static_cast<std::size_t>(id)];
}

bool BlockRegistry::is_opaque(BlockId id) const {
    return has_flag(get(id).flags, BlockFlags::Opaque);
}

bool BlockRegistry::is_solid(BlockId id) const {
    return has_flag(get(id).flags, BlockFlags::Solid);
}

bool BlockRegistry::is_replaceable(BlockId id) const {
    return id == BlockId::Air || id == BlockId::Water;
}

bool BlockRegistry::is_renderable(BlockId id) const {
    return id != BlockId::Air;
}

float BlockRegistry::hardness(BlockId id) const {
    return get(id).hardness;
}

}
