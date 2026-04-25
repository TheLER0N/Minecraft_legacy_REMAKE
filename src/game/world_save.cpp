#include "game/world_save.hpp"

#include "common/log.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <limits>
#include <random>
#include <string>

namespace ml {

namespace {

constexpr std::uint32_t kChunkSnapshotMagic = 0x31484C4D; // MLH1
constexpr std::uint32_t kChunkSnapshotVersion = 1;

std::optional<WorldSeed> parse_world_seed(const std::string& text) {
    const std::string key = "\"worldSeed\"";
    const std::size_t key_pos = text.find(key);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t colon_pos = text.find(':', key_pos + key.size());
    if (colon_pos == std::string::npos) {
        return std::nullopt;
    }
    std::size_t value_begin = colon_pos + 1;
    while (value_begin < text.size() && std::isspace(static_cast<unsigned char>(text[value_begin])) != 0) {
        ++value_begin;
    }
    std::size_t value_end = value_begin;
    while (value_end < text.size() && std::isdigit(static_cast<unsigned char>(text[value_end])) != 0) {
        ++value_end;
    }
    if (value_begin == value_end) {
        return std::nullopt;
    }
    WorldSeed seed = 0;
    const auto [ptr, ec] = std::from_chars(text.data() + value_begin, text.data() + value_end, seed);
    if (ec != std::errc() || ptr != text.data() + value_end) {
        return std::nullopt;
    }
    return seed;
}

void write_u32(std::ofstream& file, std::uint32_t value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

void write_i32(std::ofstream& file, std::int32_t value) {
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
}

bool read_u32(std::ifstream& file, std::uint32_t& value) {
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(file);
}

bool read_i32(std::ifstream& file, std::int32_t& value) {
    file.read(reinterpret_cast<char*>(&value), sizeof(value));
    return static_cast<bool>(file);
}

}

WorldSave::WorldSave(std::filesystem::path root)
    : root_(std::move(root))
    , chunks_directory_(root_ / "chunks") {
}

const std::filesystem::path& WorldSave::root() const {
    return root_;
}

const std::filesystem::path& WorldSave::chunks_directory() const {
    return chunks_directory_;
}

WorldMetadata WorldSave::load_or_create_metadata() {
    std::error_code error;
    std::filesystem::create_directories(root_, error);
    if (error) {
        log_message(LogLevel::Warning, "WorldSave: failed to create save root: " + error.message());
    }
    std::filesystem::create_directories(chunks_directory_, error);
    if (error) {
        log_message(LogLevel::Warning, "WorldSave: failed to create chunk directory: " + error.message());
    }

    const std::filesystem::path metadata_path = root_ / "world.json";
    {
        std::ifstream input(metadata_path);
        if (input) {
            const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            if (std::optional<WorldSeed> seed = parse_world_seed(contents); seed.has_value()) {
                log_message(LogLevel::Info, "WorldSave: loaded worldSeed=" + std::to_string(*seed));
                return {1, *seed};
            }
            log_message(LogLevel::Warning, "WorldSave: world.json exists but worldSeed is invalid; creating a new seed");
        }
    }

    const WorldSeed seed = random_world_seed();
    std::filesystem::remove_all(chunks_directory_, error);
    if (error) {
        log_message(LogLevel::Warning, "WorldSave: failed to clear old chunk snapshots: " + error.message());
    }
    std::filesystem::create_directories(chunks_directory_, error);
    if (error) {
        log_message(LogLevel::Warning, "WorldSave: failed to recreate chunk directory: " + error.message());
    }

    std::ofstream output(metadata_path, std::ios::trunc);
    output << "{\n  \"version\": 1,\n  \"worldSeed\": " << seed << "\n}\n";
    log_message(LogLevel::Info, "WorldSave: created worldSeed=" + std::to_string(seed));
    return {1, seed};
}

std::optional<ChunkData> WorldSave::load_chunk(ChunkCoord coord) const {
    std::ifstream input(chunk_path(coord), std::ios::binary);
    if (!input) {
        return std::nullopt;
    }

    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::int32_t chunk_x = 0;
    std::int32_t chunk_z = 0;
    std::uint32_t run_count = 0;
    if (!read_u32(input, magic) || !read_u32(input, version) || !read_i32(input, chunk_x) ||
        !read_i32(input, chunk_z) || !read_u32(input, run_count)) {
        return std::nullopt;
    }
    if (magic != kChunkSnapshotMagic || version != kChunkSnapshotVersion || chunk_x != coord.x || chunk_z != coord.z) {
        log_message(LogLevel::Warning, "WorldSave: invalid chunk snapshot header");
        return std::nullopt;
    }

    ChunkData chunk {};
    std::size_t cursor = 0;
    for (std::uint32_t i = 0; i < run_count; ++i) {
        std::uint32_t value = 0;
        std::uint32_t count = 0;
        if (!read_u32(input, value) || !read_u32(input, count)) {
            return std::nullopt;
        }
        if (count == 0 || cursor + count > chunk.blocks.size()) {
            return std::nullopt;
        }
        std::fill_n(chunk.blocks.begin() + static_cast<std::ptrdiff_t>(cursor), count, static_cast<BlockId>(value));
        cursor += count;
    }
    if (cursor != chunk.blocks.size()) {
        return std::nullopt;
    }
    return chunk;
}

bool WorldSave::save_chunk(ChunkCoord coord, const ChunkData& chunk) const {
    std::error_code error;
    std::filesystem::create_directories(chunks_directory_, error);
    if (error) {
        log_message(LogLevel::Warning, "WorldSave: failed to create chunk directory: " + error.message());
        return false;
    }

    std::ofstream output(chunk_path(coord), std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }

    write_u32(output, kChunkSnapshotMagic);
    write_u32(output, kChunkSnapshotVersion);
    write_i32(output, coord.x);
    write_i32(output, coord.z);

    std::vector<std::pair<std::uint32_t, std::uint32_t>> runs;
    runs.reserve(4096);
    for (std::size_t i = 0; i < chunk.blocks.size();) {
        const BlockId value = chunk.blocks[i];
        std::size_t end = i + 1;
        while (end < chunk.blocks.size() && chunk.blocks[end] == value && end - i < std::numeric_limits<std::uint32_t>::max()) {
            ++end;
        }
        runs.push_back({static_cast<std::uint32_t>(value), static_cast<std::uint32_t>(end - i)});
        i = end;
    }

    write_u32(output, static_cast<std::uint32_t>(runs.size()));
    for (const auto& [value, count] : runs) {
        write_u32(output, value);
        write_u32(output, count);
    }
    return static_cast<bool>(output);
}

std::filesystem::path WorldSave::chunk_path(ChunkCoord coord) const {
    return chunks_directory_ / ("c." + std::to_string(coord.x) + "." + std::to_string(coord.z) + ".mlchunk");
}

WorldSeed random_world_seed() {
    std::random_device random_device;
    WorldSeed seed = static_cast<WorldSeed>(random_device()) << 32;
    seed ^= static_cast<WorldSeed>(random_device());
    seed = splitmix64(seed ^ static_cast<WorldSeed>(
        std::chrono::high_resolution_clock::now().time_since_epoch().count()
    ));
    return seed == 0 ? 1 : seed;
}

WorldSeed splitmix64(WorldSeed value) {
    value += 0x9E3779B97F4A7C15ull;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

WorldSeed get_chunk_seed(WorldSeed world_seed, int chunk_x, int chunk_z) {
    WorldSeed hash = world_seed;
    hash ^= splitmix64(static_cast<WorldSeed>(static_cast<std::int64_t>(chunk_x)) * 0x9E3779B97F4A7C15ull);
    hash ^= splitmix64(static_cast<WorldSeed>(static_cast<std::int64_t>(chunk_z)) * 0xBF58476D1CE4E5B9ull);
    return splitmix64(hash);
}

}
