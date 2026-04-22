#include "game/block.hpp"

namespace ml {

BlockRegistry::BlockRegistry() {
    defs_[static_cast<std::size_t>(BlockId::Air)] = {BlockId::Air, "air", BlockFlags::None, {0.0f, 0.0f, 0.0f}, 0, 0, 0};
    defs_[static_cast<std::size_t>(BlockId::Grass)] = {BlockId::Grass, "grass", BlockFlags::Opaque | BlockFlags::Solid, {0.32f, 0.72f, 0.24f}, 1, 0, 2};
    defs_[static_cast<std::size_t>(BlockId::Dirt)] = {BlockId::Dirt, "dirt", BlockFlags::Opaque | BlockFlags::Solid, {0.50f, 0.31f, 0.16f}, 0, 0, 0};
    defs_[static_cast<std::size_t>(BlockId::Stone)] = {BlockId::Stone, "stone", BlockFlags::Opaque | BlockFlags::Solid, {0.55f, 0.58f, 0.60f}, 3, 3, 3};
    defs_[static_cast<std::size_t>(BlockId::Water)] = {BlockId::Water, "water", BlockFlags::None, {0.18f, 0.36f, 0.80f}, 4, 4, 4};
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

}
