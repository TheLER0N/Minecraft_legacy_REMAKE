#pragma once

#include "common/math.hpp"

#include <array>
#include <cstdint>

namespace ml {

enum class BlockId : std::uint8_t {
    Air = 0,
    Grass,
    Dirt,
    Stone,
    Water,
    Sand,
    Gravel,
    OakLog,
    OakLeaves,
    Count
};

enum class BlockFlags : std::uint32_t {
    None = 0,
    Opaque = 1u << 0u,
    Solid = 1u << 1u
};

enum class BlockRenderType : std::uint8_t {
    Opaque,
    Cutout,
    Transparent
};

inline BlockFlags operator|(BlockFlags lhs, BlockFlags rhs) {
    return static_cast<BlockFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

inline bool has_flag(BlockFlags flags, BlockFlags flag) {
    return (static_cast<std::uint32_t>(flags) & static_cast<std::uint32_t>(flag)) != 0u;
}

struct BlockDef {
    BlockId id {};
    const char* name {""};
    BlockFlags flags {BlockFlags::None};
    BlockRenderType render_type {BlockRenderType::Opaque};
    Vec3 debug_color {};
    std::uint32_t tex_top {0};
    std::uint32_t tex_bottom {0};
    std::uint32_t tex_side {0};
    std::uint8_t light_emission {0};
    std::uint8_t light_dampening {15};
    float hardness {0.0f};
};

class BlockRegistry {
public:
    BlockRegistry();
    const BlockDef& get(BlockId id) const;
    bool is_opaque(BlockId id) const;
    bool is_solid(BlockId id) const;
    bool is_replaceable(BlockId id) const;
    bool is_renderable(BlockId id) const;
    BlockRenderType render_type(BlockId id) const;
    std::uint8_t light_emission(BlockId id) const;
    std::uint8_t light_dampening(BlockId id) const;
    float hardness(BlockId id) const;

private:
    std::array<BlockDef, static_cast<std::size_t>(BlockId::Count)> defs_ {};
};

}
