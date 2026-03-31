#include "world.h"

#include "imgui.h"

#include "../app/vulkan_context.h"
#include "../menu/menu_assets.h"
#include "../menu/menu_fonts.h"
#include "../menu/menu_state.h"

#include "../../bgfx/tools/texturev/vs_texture_cube.bin.h"
#include "../../bgfx/tools/texturev/fs_texture_array.bin.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace
{
constexpr int kChunkSizeX = 16;
constexpr int kChunkSizeY = 64;
constexpr int kChunkSizeZ = 16;
constexpr int kWorldRadiusChunks = 3;
constexpr float kBaseMoveSpeed = 5.4f;
constexpr float kJumpVelocity = 6.4f;
constexpr float kGravity = 18.0f;
constexpr float kMouseSensitivity = 0.0019f;
constexpr float kEyeHeight = 1.62f;
constexpr float kPlayerRadius = 0.28f;
constexpr float kPlayerHeight = 1.8f;
constexpr float kNearClipDistance = 0.05f;
constexpr float kFarClipDistance = 80.0f;
constexpr float kWorldFieldOfViewYRadians = 70.0f * 3.14159265f / 180.0f;
constexpr float kInitialWorldYaw = -1.57079633f;
constexpr float kInitialWorldPitch = -0.18f;
constexpr int kTestChunkOriginX = -8;
constexpr int kTestChunkOriginY = 0;
constexpr int kTestChunkOriginZ = -8;

enum class BlockId : uint8_t
{
    Air = 0,
    Grass,
    Dirt,
    Stone,
    Bedrock,
};

enum class WorldFace : uint8_t
{
    Top = 0,
    Bottom,
    North,
    South,
    West,
    East,
};

struct WorldTextureSet
{
    MenuInternal::TextureSlot GrassTop = {};
    MenuInternal::TextureSlot GrassSide = {};
    MenuInternal::TextureSlot Dirt = {};
    MenuInternal::TextureSlot Stone = {};
    MenuInternal::TextureSlot Bedrock = {};
    bool Loaded = false;
    bool LoadAttempted = false;
};

struct Vec3
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
};

struct ChunkCoord
{
    int X = 0;
    int Z = 0;
};

struct WorldMeta
{
    std::string DirectoryName;
    std::string Name;
    uint32_t Seed = 0;
    Vec3 Spawn = { 0.0f, 40.0f, 0.0f };
    std::string LastPlayed = "Today";
    std::string GameMode = "Survival";
    std::string Description = "A blocky world generated from a saved seed.";
};

struct Chunk
{
    ChunkCoord Coord = {};
    std::vector<BlockId> Blocks = std::vector<BlockId>(static_cast<size_t>(kChunkSizeX * kChunkSizeY * kChunkSizeZ), BlockId::Air);
};

struct MeshFace
{
    Vec3 Corners[4] = {};
    Vec3 Center = {};
    Vec3 Normal = {};
    BlockId Block = BlockId::Air;
    WorldFace Face = WorldFace::Top;
    float Shade = 1.0f;
};

struct GpuVertex
{
    float Position[3] = {};
    float TexCoord[3] = {};
    uint8_t Color[4] = {};
};

struct GpuBuffer
{
    VkBuffer Buffer = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    VkDeviceSize Size = 0;
};

struct TextureArrayResource
{
    VkImage Image = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    VkImageView View = VK_NULL_HANDLE;
    VkSampler Sampler = VK_NULL_HANDLE;
    uint32_t LayerCount = 0;
    uint32_t Width = 0;
    uint32_t Height = 0;
};

struct WorldRenderer
{
    bool Initialized = false;
    bool GpuReady = false;
    bool PendingMeshUpload = false;
    bool PendingTextureUpload = false;
    AppVulkanContext Context = {};
    VkDescriptorSetLayout DescriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout PipelineLayout = VK_NULL_HANDLE;
    VkPipeline Pipeline = VK_NULL_HANDLE;
    VkDescriptorSet DescriptorSet = VK_NULL_HANDLE;
    VkCommandPool UploadCommandPool = VK_NULL_HANDLE;
    GpuBuffer VertexBuffer = {};
    GpuBuffer IndexBuffer = {};
    GpuBuffer VertexUniformBuffer = {};
    GpuBuffer FragmentUniformBuffer = {};
    TextureArrayResource TextureArray = {};
    std::vector<GpuVertex> CpuVertices = {};
    std::vector<uint32_t> CpuIndices = {};
};

struct VertexUniformBlock
{
    float ModelViewProj[16] = {};
};

struct FragmentUniformBlock
{
    float Params[4] = { 0.0f, 0.0f, 1.0f, 0.0f };
};

struct Mat4
{
    float M[16] = {};
};

struct WorldState
{
    bool SystemInitialized = false;
    bool Loaded = false;
    float MainScale = 1.0f;
    WorldMeta Meta = {};
    std::vector<Chunk> Chunks = {};
    std::vector<MeshFace> MeshFaces = {};
    Vec3 Position = {};
    Vec3 Velocity = {};
    float Yaw = -1.2f;
    float Pitch = -0.45f;
    bool MouseCaptured = false;
    bool OnGround = false;
    bool ShowWireframe = false;
};

struct ProjectedFace
{
    const MeshFace* Face = nullptr;
    ImVec2 Screen[4] = {};
    float Depth = 0.0f;
    float Fog = 0.0f;
};

WorldTextureSet g_WorldTextures = {};
WorldState g_WorldState = {};
WorldRenderer g_WorldRenderer = {};

constexpr std::array<const char*, 5> kWorldTextureFiles =
{
    "assets\\world\\blocks\\grass_top.png",
    "assets\\world\\blocks\\grass_side.png",
    "assets\\world\\blocks\\dirt.png",
    "assets\\world\\blocks\\stone.png",
    "assets\\world\\blocks\\bedrock.png",
};

constexpr uint32_t kWorldTextureLayerCount = 5;
constexpr uint8_t kSpirvOldFragmentBinding = 48;
constexpr uint8_t kSpirvSamplerShift = 16;

enum class TextureLayer : uint32_t
{
    GrassTop = 0,
    GrassSide = 1,
    Dirt = 2,
    Stone = 3,
    Bedrock = 4,
};

float ClampFloat(float value, float min_value, float max_value)
{
    return std::max(min_value, std::min(max_value, value));
}

float LerpFloat(float a, float b, float t)
{
    return a + (b - a) * t;
}

Vec3 operator+(const Vec3& a, const Vec3& b)
{
    return { a.X + b.X, a.Y + b.Y, a.Z + b.Z };
}

Vec3 operator-(const Vec3& a, const Vec3& b)
{
    return { a.X - b.X, a.Y - b.Y, a.Z - b.Z };
}

Vec3 operator*(const Vec3& value, float scalar)
{
    return { value.X * scalar, value.Y * scalar, value.Z * scalar };
}

float Dot(const Vec3& a, const Vec3& b)
{
    return a.X * b.X + a.Y * b.Y + a.Z * b.Z;
}

Vec3 Cross(const Vec3& a, const Vec3& b)
{
    return
    {
        a.Y * b.Z - a.Z * b.Y,
        a.Z * b.X - a.X * b.Z,
        a.X * b.Y - a.Y * b.X,
    };
}

float LengthSquared(const Vec3& value)
{
    return Dot(value, value);
}

Mat4 IdentityMatrix()
{
    Mat4 matrix = {};
    matrix.M[0] = 1.0f;
    matrix.M[5] = 1.0f;
    matrix.M[10] = 1.0f;
    matrix.M[15] = 1.0f;
    return matrix;
}

Mat4 MultiplyMatrix(const Mat4& a, const Mat4& b)
{
    Mat4 result = {};
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            result.M[col + row * 4] =
                a.M[0 + row * 4] * b.M[col + 0 * 4] +
                a.M[1 + row * 4] * b.M[col + 1 * 4] +
                a.M[2 + row * 4] * b.M[col + 2 * 4] +
                a.M[3 + row * 4] * b.M[col + 3 * 4];
        }
    }
    return result;
}

Mat4 PerspectiveMatrix(float vertical_fov_radians, float aspect, float near_plane, float far_plane)
{
    Mat4 matrix = {};
    const float tan_half_fov = std::tan(vertical_fov_radians * 0.5f);
    matrix.M[0] = 1.0f / (aspect * tan_half_fov);
    matrix.M[5] = -1.0f / tan_half_fov;
    matrix.M[10] = far_plane / (near_plane - far_plane);
    matrix.M[11] = -1.0f;
    matrix.M[14] = (near_plane * far_plane) / (near_plane - far_plane);
    return matrix;
}

Mat4 ViewMatrix(const Vec3& position, const Vec3& forward, const Vec3& right, const Vec3& up)
{
    Mat4 matrix = IdentityMatrix();
    matrix.M[0] = right.X;
    matrix.M[1] = up.X;
    matrix.M[2] = -forward.X;
    matrix.M[4] = right.Y;
    matrix.M[5] = up.Y;
    matrix.M[6] = -forward.Y;
    matrix.M[8] = right.Z;
    matrix.M[9] = up.Z;
    matrix.M[10] = -forward.Z;
    matrix.M[12] = -Dot(right, position);
    matrix.M[13] = -Dot(up, position);
    matrix.M[14] = Dot(forward, position);
    return matrix;
}

Vec3 Normalize(const Vec3& value)
{
    const float length_squared = LengthSquared(value);
    if (length_squared <= 0.000001f)
    {
        return {};
    }

    const float inverse_length = 1.0f / std::sqrt(length_squared);
    return value * inverse_length;
}

int FloorToInt(float value)
{
    return static_cast<int>(std::floor(value));
}

uint32_t FindMemoryTypeIndex(const AppVulkanContext& context, uint32_t type_bits, VkMemoryPropertyFlags required_flags)
{
    VkPhysicalDeviceMemoryProperties properties = {};
    context.GetPhysicalDeviceMemoryProperties(context.PhysicalDevice, &properties);
    for (uint32_t index = 0; index < properties.memoryTypeCount; ++index)
    {
        if ((type_bits & (1u << index)) != 0 && (properties.memoryTypes[index].propertyFlags & required_flags) == required_flags)
        {
            return index;
        }
    }

    return UINT32_MAX;
}

const uint32_t* GetBgfxShaderBytecode(const uint8_t* shader_bin, size_t shader_bin_size, size_t& out_size_bytes)
{
    if (shader_bin_size < 32)
    {
        return nullptr;
    }

    size_t cursor = 4;
    cursor += 4;
    cursor += 4;
    const uint16_t uniform_count = static_cast<uint16_t>(shader_bin[cursor] | (shader_bin[cursor + 1] << 8));
    cursor += 2;
    for (uint16_t index = 0; index < uniform_count; ++index)
    {
        const uint8_t name_length = shader_bin[cursor++];
        cursor += name_length;
        cursor += 1;
        cursor += 1;
        cursor += 2;
        cursor += 2;
        if (shader_bin[3] >= 8)
        {
            cursor += 2;
        }
        if (shader_bin[3] >= 10)
        {
            cursor += 2;
        }
    }

    const uint32_t shader_size =
        static_cast<uint32_t>(shader_bin[cursor + 0]) |
        (static_cast<uint32_t>(shader_bin[cursor + 1]) << 8) |
        (static_cast<uint32_t>(shader_bin[cursor + 2]) << 16) |
        (static_cast<uint32_t>(shader_bin[cursor + 3]) << 24);
    cursor += 4;
    out_size_bytes = shader_size;
    return reinterpret_cast<const uint32_t*>(shader_bin + cursor);
}

void DestroyGpuBuffer(GpuBuffer& buffer)
{
    if (g_WorldRenderer.Context.Device == VK_NULL_HANDLE)
    {
        buffer = {};
        return;
    }

    if (buffer.Buffer != VK_NULL_HANDLE)
    {
        g_WorldRenderer.Context.DestroyBuffer(g_WorldRenderer.Context.Device, buffer.Buffer, g_WorldRenderer.Context.Allocator);
    }
    if (buffer.Memory != VK_NULL_HANDLE)
    {
        g_WorldRenderer.Context.FreeMemory(g_WorldRenderer.Context.Device, buffer.Memory, g_WorldRenderer.Context.Allocator);
    }
    buffer = {};
}

bool CreateBufferResource(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags memory_flags, GpuBuffer& out_buffer)
{
    DestroyGpuBuffer(out_buffer);
    VkBufferCreateInfo buffer_info = {};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (g_WorldRenderer.Context.CreateBuffer(g_WorldRenderer.Context.Device, &buffer_info, g_WorldRenderer.Context.Allocator, &out_buffer.Buffer) != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements requirements = {};
    g_WorldRenderer.Context.GetBufferMemoryRequirements(g_WorldRenderer.Context.Device, out_buffer.Buffer, &requirements);
    VkMemoryAllocateInfo alloc_info = {};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = requirements.size;
    alloc_info.memoryTypeIndex = FindMemoryTypeIndex(g_WorldRenderer.Context, requirements.memoryTypeBits, memory_flags);
    if (alloc_info.memoryTypeIndex == UINT32_MAX)
    {
        DestroyGpuBuffer(out_buffer);
        return false;
    }

    if (g_WorldRenderer.Context.AllocateMemory(g_WorldRenderer.Context.Device, &alloc_info, g_WorldRenderer.Context.Allocator, &out_buffer.Memory) != VK_SUCCESS)
    {
        DestroyGpuBuffer(out_buffer);
        return false;
    }

    if (g_WorldRenderer.Context.BindBufferMemory(g_WorldRenderer.Context.Device, out_buffer.Buffer, out_buffer.Memory, 0) != VK_SUCCESS)
    {
        DestroyGpuBuffer(out_buffer);
        return false;
    }

    out_buffer.Size = size;
    return true;
}

size_t GetBlockIndex(int x, int y, int z)
{
    return static_cast<size_t>(x + z * kChunkSizeX + y * kChunkSizeX * kChunkSizeZ);
}

std::string GetRuntimePath(const char* relative_path)
{
    return MenuInternal::GetProjectAssetPath(relative_path);
}

#ifdef _WIN32
fs::path Utf8Path(const std::string& utf8_path)
{
    if (utf8_path.empty())
    {
        return {};
    }

    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, nullptr, 0);
    if (wide_length <= 0)
    {
        return fs::path(utf8_path);
    }

    std::wstring wide_path(static_cast<size_t>(wide_length - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_path.c_str(), -1, wide_path.data(), wide_length);
    return fs::path(wide_path);
}
#else
fs::path Utf8Path(const std::string& utf8_path)
{
    return fs::path(utf8_path);
}
#endif

fs::path GetSavesRoot()
{
    return Utf8Path(GetRuntimePath("saves"));
}

fs::path GetWorldPath(const std::string& directory_name)
{
    return GetSavesRoot() / directory_name;
}

fs::path GetWorldMetaPath(const std::string& directory_name)
{
    return GetWorldPath(directory_name) / "world.meta";
}

fs::path GetChunkDirectoryPath(const std::string& directory_name)
{
    return GetWorldPath(directory_name) / "chunks";
}

fs::path GetChunkPath(const std::string& directory_name, int chunk_x, int chunk_z)
{
    return GetChunkDirectoryPath(directory_name) / ("c." + std::to_string(chunk_x) + "." + std::to_string(chunk_z) + ".bin");
}

std::string MakeSafeDirectoryName(const std::string& name)
{
    std::string result;
    result.reserve(name.size());
    for (unsigned char ch : name)
    {
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'))
        {
            result.push_back(static_cast<char>(ch));
        }
        else if (ch == ' ' || ch == '-' || ch == '_')
        {
            result.push_back('_');
        }
    }

    if (result.empty())
    {
        result = "world";
    }

    return result;
}

uint32_t ParseSeedText(const char* seed_text)
{
    if (seed_text == nullptr || seed_text[0] == '\0')
    {
        return static_cast<uint32_t>(SDL_GetTicks());
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(seed_text, &end, 10);
    if (end != nullptr && *end == '\0')
    {
        return static_cast<uint32_t>(parsed & 0xFFFFFFFFu);
    }

    uint32_t seed = 2166136261u;
    for (const unsigned char* cursor = reinterpret_cast<const unsigned char*>(seed_text); *cursor != 0; ++cursor)
    {
        seed ^= *cursor;
        seed *= 16777619u;
    }

    return seed;
}

float HashToUnitFloat(int x, int z, uint32_t seed)
{
    uint32_t value = seed;
    value ^= static_cast<uint32_t>(x) * 374761393u;
    value ^= static_cast<uint32_t>(z) * 668265263u;
    value = (value ^ (value >> 13)) * 1274126177u;
    value ^= value >> 16;
    return static_cast<float>(value & 0xFFFFu) / 65535.0f;
}

int ComputeTerrainHeight(int world_x, int world_z, uint32_t seed)
{
    const float base = 20.0f;
    const float large = (HashToUnitFloat(world_x / 7, world_z / 7, seed) - 0.5f) * 10.0f;
    const float medium = (HashToUnitFloat(world_x / 3, world_z / 3, seed ^ 0xA511E9B3u) - 0.5f) * 6.0f;
    const float detail = (HashToUnitFloat(world_x, world_z, seed ^ 0xC3A5C85Cu) - 0.5f) * 2.0f;
    return std::clamp(static_cast<int>(std::round(base + large + medium + detail)), 6, kChunkSizeY - 3);
}

BlockId GetGeneratedBlock(int world_x, int world_y, int world_z, uint32_t seed)
{
    const int surface_height = ComputeTerrainHeight(world_x, world_z, seed);
    if (world_y <= 0)
    {
        return BlockId::Bedrock;
    }
    if (world_y > surface_height)
    {
        return BlockId::Air;
    }
    if (world_y == surface_height)
    {
        return BlockId::Grass;
    }
    if (world_y >= surface_height - 3)
    {
        return BlockId::Dirt;
    }
    return BlockId::Stone;
}

bool SaveWorldMeta(const WorldMeta& meta)
{
    try
    {
        fs::create_directories(GetWorldPath(meta.DirectoryName));
        std::ofstream output(GetWorldMetaPath(meta.DirectoryName), std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return false;
        }

        output << "name=" << meta.Name << "\n";
        output << "seed=" << meta.Seed << "\n";
        output << "spawn_x=" << meta.Spawn.X << "\n";
        output << "spawn_y=" << meta.Spawn.Y << "\n";
        output << "spawn_z=" << meta.Spawn.Z << "\n";
        output << "game_mode=" << meta.GameMode << "\n";
        output << "last_played=" << meta.LastPlayed << "\n";
        output << "description=" << meta.Description << "\n";
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool LoadWorldMeta(const fs::path& meta_path, WorldMeta& out_meta)
{
    std::ifstream input(meta_path, std::ios::binary);
    if (!input)
    {
        return false;
    }

    WorldMeta meta = {};
    meta.DirectoryName = meta_path.parent_path().filename().string();

    std::string line;
    while (std::getline(input, line))
    {
        const size_t separator = line.find('=');
        if (separator == std::string::npos)
        {
            continue;
        }

        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 1);
        if (key == "name") meta.Name = value;
        else if (key == "seed") meta.Seed = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        else if (key == "spawn_x") meta.Spawn.X = std::strtof(value.c_str(), nullptr);
        else if (key == "spawn_y") meta.Spawn.Y = std::strtof(value.c_str(), nullptr);
        else if (key == "spawn_z") meta.Spawn.Z = std::strtof(value.c_str(), nullptr);
        else if (key == "game_mode") meta.GameMode = value;
        else if (key == "last_played") meta.LastPlayed = value;
        else if (key == "description") meta.Description = value;
    }

    if (meta.Name.empty())
    {
        meta.Name = meta.DirectoryName;
    }
    if (meta.GameMode.empty())
    {
        meta.GameMode = "Survival";
    }
    if (meta.LastPlayed.empty())
    {
        meta.LastPlayed = "Today";
    }
    if (meta.Description.empty())
    {
        meta.Description = "A blocky world generated from a saved seed.";
    }

    out_meta = meta;
    return true;
}

bool SaveChunk(const WorldMeta& meta, const Chunk& chunk)
{
    try
    {
        fs::create_directories(GetChunkDirectoryPath(meta.DirectoryName));
        std::ofstream output(GetChunkPath(meta.DirectoryName, chunk.Coord.X, chunk.Coord.Z), std::ios::binary | std::ios::trunc);
        if (!output)
        {
            return false;
        }

        static constexpr uint32_t kMagic = 0x4B4E4843u;
        static constexpr uint32_t kVersion = 1u;
        output.write(reinterpret_cast<const char*>(&kMagic), sizeof(kMagic));
        output.write(reinterpret_cast<const char*>(&kVersion), sizeof(kVersion));
        output.write(reinterpret_cast<const char*>(&chunk.Coord.X), sizeof(chunk.Coord.X));
        output.write(reinterpret_cast<const char*>(&chunk.Coord.Z), sizeof(chunk.Coord.Z));
        output.write(reinterpret_cast<const char*>(chunk.Blocks.data()), static_cast<std::streamsize>(chunk.Blocks.size() * sizeof(BlockId)));
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool LoadChunk(const WorldMeta& meta, int chunk_x, int chunk_z, Chunk& out_chunk)
{
    std::ifstream input(GetChunkPath(meta.DirectoryName, chunk_x, chunk_z), std::ios::binary);
    if (!input)
    {
        return false;
    }

    uint32_t magic = 0;
    uint32_t version = 0;
    Chunk chunk = {};
    input.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    input.read(reinterpret_cast<char*>(&version), sizeof(version));
    input.read(reinterpret_cast<char*>(&chunk.Coord.X), sizeof(chunk.Coord.X));
    input.read(reinterpret_cast<char*>(&chunk.Coord.Z), sizeof(chunk.Coord.Z));
    if (!input || magic != 0x4B4E4843u || version != 1u)
    {
        return false;
    }

    input.read(reinterpret_cast<char*>(chunk.Blocks.data()), static_cast<std::streamsize>(chunk.Blocks.size() * sizeof(BlockId)));
    if (!input)
    {
        return false;
    }

    out_chunk = chunk;
    return true;
}

Chunk GenerateChunk(const WorldMeta& meta, int chunk_x, int chunk_z)
{
    Chunk chunk = {};
    chunk.Coord = { chunk_x, chunk_z };
    for (int y = 0; y < kChunkSizeY; ++y)
    {
        for (int z = 0; z < kChunkSizeZ; ++z)
        {
            for (int x = 0; x < kChunkSizeX; ++x)
            {
                const int world_x = chunk_x * kChunkSizeX + x;
                const int world_z = chunk_z * kChunkSizeZ + z;
                chunk.Blocks[GetBlockIndex(x, y, z)] = GetGeneratedBlock(world_x, y, world_z, meta.Seed);
            }
        }
    }
    return chunk;
}

const Chunk* FindChunk(int chunk_x, int chunk_z)
{
    for (const Chunk& chunk : g_WorldState.Chunks)
    {
        if (chunk.Coord.X == chunk_x && chunk.Coord.Z == chunk_z)
        {
            return &chunk;
        }
    }

    return nullptr;
}

BlockId GetBlockAtWorld(int world_x, int world_y, int world_z)
{
    if (world_y < 0 || world_y >= kChunkSizeY)
    {
        return BlockId::Air;
    }

    const int chunk_x = FloorToInt(static_cast<float>(world_x) / static_cast<float>(kChunkSizeX));
    const int chunk_z = FloorToInt(static_cast<float>(world_z) / static_cast<float>(kChunkSizeZ));
    const Chunk* chunk = FindChunk(chunk_x, chunk_z);
    if (chunk == nullptr)
    {
        return GetGeneratedBlock(world_x, world_y, world_z, g_WorldState.Meta.Seed);
    }

    const int local_x = world_x - chunk_x * kChunkSizeX;
    const int local_z = world_z - chunk_z * kChunkSizeZ;
    if (local_x < 0 || local_x >= kChunkSizeX || local_z < 0 || local_z >= kChunkSizeZ)
    {
        return BlockId::Air;
    }

    return chunk->Blocks[GetBlockIndex(local_x, world_y, local_z)];
}

bool IsSolidBlock(BlockId block)
{
    return block != BlockId::Air;
}

void EnsureWorldLoadedAroundSpawn(const WorldMeta& meta)
{
    g_WorldState.Chunks.clear();
    const int center_chunk_x = FloorToInt(meta.Spawn.X / static_cast<float>(kChunkSizeX));
    const int center_chunk_z = FloorToInt(meta.Spawn.Z / static_cast<float>(kChunkSizeZ));
    for (int chunk_z = center_chunk_z - kWorldRadiusChunks; chunk_z <= center_chunk_z + kWorldRadiusChunks; ++chunk_z)
    {
        for (int chunk_x = center_chunk_x - kWorldRadiusChunks; chunk_x <= center_chunk_x + kWorldRadiusChunks; ++chunk_x)
        {
            Chunk chunk = {};
            if (!LoadChunk(meta, chunk_x, chunk_z, chunk))
            {
                chunk = GenerateChunk(meta, chunk_x, chunk_z);
                SaveChunk(meta, chunk);
            }
            g_WorldState.Chunks.push_back(std::move(chunk));
        }
    }
}

bool EnsureWorldTexturesLoaded()
{
    if (g_WorldTextures.Loaded)
    {
        return true;
    }
    if (g_WorldTextures.LoadAttempted)
    {
        return false;
    }

    g_WorldTextures.LoadAttempted = true;
    if (!MenuInternal::LoadTextureSlot(g_WorldTextures.GrassTop, kWorldTextureFiles[0], true)) return false;
    if (!MenuInternal::LoadTextureSlot(g_WorldTextures.GrassSide, kWorldTextureFiles[1], true)) return false;
    if (!MenuInternal::LoadTextureSlot(g_WorldTextures.Dirt, kWorldTextureFiles[2], true)) return false;
    if (!MenuInternal::LoadTextureSlot(g_WorldTextures.Stone, kWorldTextureFiles[3], true)) return false;
    if (!MenuInternal::LoadTextureSlot(g_WorldTextures.Bedrock, kWorldTextureFiles[4], true)) return false;
    g_WorldTextures.Loaded = true;
    return true;
}

void ResetWorldTextures()
{
    MenuInternal::ResetTextureSlot(g_WorldTextures.GrassTop);
    MenuInternal::ResetTextureSlot(g_WorldTextures.GrassSide);
    MenuInternal::ResetTextureSlot(g_WorldTextures.Dirt);
    MenuInternal::ResetTextureSlot(g_WorldTextures.Stone);
    MenuInternal::ResetTextureSlot(g_WorldTextures.Bedrock);
    g_WorldTextures.Loaded = false;
    g_WorldTextures.LoadAttempted = false;
}

MenuInternal::TextureSlot* GetTextureForFace(BlockId block, WorldFace face)
{
    switch (block)
    {
    case BlockId::Grass:
        if (face == WorldFace::Top) return &g_WorldTextures.GrassTop;
        if (face == WorldFace::Bottom) return &g_WorldTextures.Dirt;
        return &g_WorldTextures.GrassSide;
    case BlockId::Dirt:
        return &g_WorldTextures.Dirt;
    case BlockId::Stone:
        return &g_WorldTextures.Stone;
    case BlockId::Bedrock:
        return &g_WorldTextures.Bedrock;
    default:
        return nullptr;
    }
}

TextureLayer GetTextureLayerForFace(BlockId block, WorldFace face)
{
    switch (block)
    {
    case BlockId::Grass:
        if (face == WorldFace::Top) return TextureLayer::GrassTop;
        if (face == WorldFace::Bottom) return TextureLayer::Dirt;
        return TextureLayer::GrassSide;
    case BlockId::Dirt:
        return TextureLayer::Dirt;
    case BlockId::Stone:
        return TextureLayer::Stone;
    case BlockId::Bedrock:
        return TextureLayer::Bedrock;
    default:
        return TextureLayer::GrassTop;
    }
}

float GetFaceShade(WorldFace face)
{
    switch (face)
    {
    case WorldFace::Top: return 1.0f;
    case WorldFace::Bottom: return 0.55f;
    case WorldFace::North:
    case WorldFace::South: return 0.78f;
    case WorldFace::West:
    case WorldFace::East: return 0.88f;
    }

    return 1.0f;
}

ImU32 ShadeColor(ImU32 color, float multiplier)
{
    ImVec4 rgba = ImGui::ColorConvertU32ToFloat4(color);
    rgba.x = ClampFloat(rgba.x * multiplier, 0.0f, 1.0f);
    rgba.y = ClampFloat(rgba.y * multiplier, 0.0f, 1.0f);
    rgba.z = ClampFloat(rgba.z * multiplier, 0.0f, 1.0f);
    return ImGui::ColorConvertFloat4ToU32(rgba);
}

Vec3 GetCameraForward()
{
    return Normalize(
    {
        std::cos(g_WorldState.Pitch) * std::cos(g_WorldState.Yaw),
        std::sin(g_WorldState.Pitch),
        std::cos(g_WorldState.Pitch) * std::sin(g_WorldState.Yaw),
    });
}

Vec3 GetCameraRight()
{
    return Normalize(Cross(GetCameraForward(), { 0.0f, 1.0f, 0.0f }));
}

Vec3 GetCameraUp()
{
    return Normalize(Cross(GetCameraRight(), GetCameraForward()));
}

MeshFace BuildFaceForBlock(int block_x, int block_y, int block_z, BlockId block, WorldFace face)
{
    const float x = static_cast<float>(block_x);
    const float y = static_cast<float>(block_y);
    const float z = static_cast<float>(block_z);

    MeshFace mesh_face = {};
    mesh_face.Block = block;
    mesh_face.Face = face;
    mesh_face.Shade = GetFaceShade(face);

    switch (face)
    {
    case WorldFace::Top:
        mesh_face.Corners[0] = { x, y + 1.0f, z };
        mesh_face.Corners[1] = { x + 1.0f, y + 1.0f, z };
        mesh_face.Corners[2] = { x + 1.0f, y + 1.0f, z + 1.0f };
        mesh_face.Corners[3] = { x, y + 1.0f, z + 1.0f };
        mesh_face.Normal = { 0.0f, 1.0f, 0.0f };
        break;
    case WorldFace::Bottom:
        mesh_face.Corners[0] = { x, y, z + 1.0f };
        mesh_face.Corners[1] = { x + 1.0f, y, z + 1.0f };
        mesh_face.Corners[2] = { x + 1.0f, y, z };
        mesh_face.Corners[3] = { x, y, z };
        mesh_face.Normal = { 0.0f, -1.0f, 0.0f };
        break;
    case WorldFace::North:
        mesh_face.Corners[0] = { x, y + 1.0f, z };
        mesh_face.Corners[1] = { x + 1.0f, y + 1.0f, z };
        mesh_face.Corners[2] = { x + 1.0f, y, z };
        mesh_face.Corners[3] = { x, y, z };
        mesh_face.Normal = { 0.0f, 0.0f, -1.0f };
        break;
    case WorldFace::South:
        mesh_face.Corners[0] = { x + 1.0f, y + 1.0f, z + 1.0f };
        mesh_face.Corners[1] = { x, y + 1.0f, z + 1.0f };
        mesh_face.Corners[2] = { x, y, z + 1.0f };
        mesh_face.Corners[3] = { x + 1.0f, y, z + 1.0f };
        mesh_face.Normal = { 0.0f, 0.0f, 1.0f };
        break;
    case WorldFace::West:
        mesh_face.Corners[0] = { x, y + 1.0f, z + 1.0f };
        mesh_face.Corners[1] = { x, y + 1.0f, z };
        mesh_face.Corners[2] = { x, y, z };
        mesh_face.Corners[3] = { x, y, z + 1.0f };
        mesh_face.Normal = { -1.0f, 0.0f, 0.0f };
        break;
    case WorldFace::East:
        mesh_face.Corners[0] = { x + 1.0f, y + 1.0f, z };
        mesh_face.Corners[1] = { x + 1.0f, y + 1.0f, z + 1.0f };
        mesh_face.Corners[2] = { x + 1.0f, y, z + 1.0f };
        mesh_face.Corners[3] = { x + 1.0f, y, z };
        mesh_face.Normal = { 1.0f, 0.0f, 0.0f };
        break;
    }

    mesh_face.Center =
    {
        (mesh_face.Corners[0].X + mesh_face.Corners[1].X + mesh_face.Corners[2].X + mesh_face.Corners[3].X) * 0.25f,
        (mesh_face.Corners[0].Y + mesh_face.Corners[1].Y + mesh_face.Corners[2].Y + mesh_face.Corners[3].Y) * 0.25f,
        (mesh_face.Corners[0].Z + mesh_face.Corners[1].Z + mesh_face.Corners[2].Z + mesh_face.Corners[3].Z) * 0.25f,
    };
    return mesh_face;
}

void AppendChunkBlockFaces(int world_x, int world_y, int world_z, BlockId block)
{
    g_WorldState.MeshFaces.push_back(BuildFaceForBlock(world_x, world_y, world_z, block, WorldFace::Top));
    g_WorldState.MeshFaces.push_back(BuildFaceForBlock(world_x, world_y, world_z, block, WorldFace::Bottom));
    g_WorldState.MeshFaces.push_back(BuildFaceForBlock(world_x, world_y, world_z, block, WorldFace::North));
    g_WorldState.MeshFaces.push_back(BuildFaceForBlock(world_x, world_y, world_z, block, WorldFace::South));
    g_WorldState.MeshFaces.push_back(BuildFaceForBlock(world_x, world_y, world_z, block, WorldFace::West));
    g_WorldState.MeshFaces.push_back(BuildFaceForBlock(world_x, world_y, world_z, block, WorldFace::East));
}

void BuildWorldMesh()
{
    g_WorldState.MeshFaces.clear();
    g_WorldState.MeshFaces.reserve(static_cast<size_t>(kChunkSizeX * kChunkSizeY * kChunkSizeZ * 6));

    for (int y = 0; y < kChunkSizeY; ++y)
    {
        for (int z = 0; z < kChunkSizeZ; ++z)
        {
            for (int x = 0; x < kChunkSizeX; ++x)
            {
                const int world_x = kTestChunkOriginX + x;
                const int world_y = kTestChunkOriginY + y;
                const int world_z = kTestChunkOriginZ + z;

                BlockId block = BlockId::Stone;
                if (y == kChunkSizeY - 1)
                {
                    block = BlockId::Grass;
                }
                else if (y >= kChunkSizeY - 4)
                {
                    block = BlockId::Dirt;
                }
                else if (y == 0)
                {
                    block = BlockId::Bedrock;
                }

                AppendChunkBlockFaces(world_x, world_y, world_z, block);
            }
        }
    }

    g_WorldRenderer.PendingMeshUpload = true;
}

void PushFaceVertices(const MeshFace& face)
{
    const TextureLayer layer = GetTextureLayerForFace(face.Block, face.Face);
    const uint32_t layer_index = static_cast<uint32_t>(layer);
    const uint32_t base_index = static_cast<uint32_t>(g_WorldRenderer.CpuVertices.size());
    const ImU32 shaded_color = ShadeColor(IM_COL32_WHITE, face.Shade);
    const unsigned char color_r = static_cast<unsigned char>((shaded_color >> IM_COL32_R_SHIFT) & 0xFFu);
    const unsigned char color_g = static_cast<unsigned char>((shaded_color >> IM_COL32_G_SHIFT) & 0xFFu);
    const unsigned char color_b = static_cast<unsigned char>((shaded_color >> IM_COL32_B_SHIFT) & 0xFFu);
    const unsigned char color_a = static_cast<unsigned char>((shaded_color >> IM_COL32_A_SHIFT) & 0xFFu);
    static constexpr float uvs[4][2] =
    {
        { 0.0f, 0.0f },
        { 1.0f, 0.0f },
        { 1.0f, 1.0f },
        { 0.0f, 1.0f },
    };

    for (int i = 0; i < 4; ++i)
    {
        GpuVertex vertex = {};
        vertex.Position[0] = face.Corners[i].X;
        vertex.Position[1] = face.Corners[i].Y;
        vertex.Position[2] = face.Corners[i].Z;
        vertex.TexCoord[0] = uvs[i][0];
        vertex.TexCoord[1] = uvs[i][1];
        vertex.TexCoord[2] = static_cast<float>(layer_index);
        vertex.Color[0] = color_r;
        vertex.Color[1] = color_g;
        vertex.Color[2] = color_b;
        vertex.Color[3] = color_a;
        g_WorldRenderer.CpuVertices.push_back(vertex);
    }

    g_WorldRenderer.CpuIndices.push_back(base_index + 0);
    g_WorldRenderer.CpuIndices.push_back(base_index + 1);
    g_WorldRenderer.CpuIndices.push_back(base_index + 2);
    g_WorldRenderer.CpuIndices.push_back(base_index + 0);
    g_WorldRenderer.CpuIndices.push_back(base_index + 2);
    g_WorldRenderer.CpuIndices.push_back(base_index + 3);
}

void RebuildGpuMeshData()
{
    g_WorldRenderer.CpuVertices.clear();
    g_WorldRenderer.CpuIndices.clear();
    g_WorldRenderer.CpuVertices.reserve(g_WorldState.MeshFaces.size() * 4);
    g_WorldRenderer.CpuIndices.reserve(g_WorldState.MeshFaces.size() * 6);
    for (const MeshFace& face : g_WorldState.MeshFaces)
    {
        PushFaceVertices(face);
    }
}

ImU32 GetSkyColor(float vertical_factor)
{
    const ImVec4 top = ImGui::ColorConvertU32ToFloat4(IM_COL32(118, 181, 255, 255));
    const ImVec4 bottom = ImGui::ColorConvertU32ToFloat4(IM_COL32(204, 232, 255, 255));
    const float t = ClampFloat(vertical_factor, 0.0f, 1.0f);
    const ImVec4 mixed = ImVec4(LerpFloat(bottom.x, top.x, t), LerpFloat(bottom.y, top.y, t), LerpFloat(bottom.z, top.z, t), 1.0f);
    return ImGui::ColorConvertFloat4ToU32(mixed);
}

void SetMouseCaptured(SDL_Window* window, bool captured)
{
    if (window == nullptr)
    {
        return;
    }

    SDL_SetWindowRelativeMouseMode(window, captured);
    SDL_SetWindowMouseGrab(window, captured);
    if (captured)
    {
        SDL_HideCursor();
    }
    else
    {
        SDL_ShowCursor();
    }
    g_WorldState.MouseCaptured = captured;
}

bool CheckPlayerCollisionAt(const Vec3& position)
{
    const float min_x = position.X - kPlayerRadius;
    const float max_x = position.X + kPlayerRadius;
    const float min_y = position.Y;
    const float max_y = position.Y + kPlayerHeight;
    const float min_z = position.Z - kPlayerRadius;
    const float max_z = position.Z + kPlayerRadius;

    for (int y = FloorToInt(min_y); y <= FloorToInt(max_y); ++y)
    {
        for (int z = FloorToInt(min_z); z <= FloorToInt(max_z); ++z)
        {
            for (int x = FloorToInt(min_x); x <= FloorToInt(max_x); ++x)
            {
                if (IsSolidBlock(GetBlockAtWorld(x, y, z)))
                {
                    return true;
                }
            }
        }
    }

    return false;
}

bool MovePlayerAxis(Vec3& position, float delta, int axis)
{
    Vec3 candidate = position;
    if (axis == 0) candidate.X += delta;
    if (axis == 1) candidate.Y += delta;
    if (axis == 2) candidate.Z += delta;
    if (CheckPlayerCollisionAt(candidate))
    {
        return false;
    }

    position = candidate;
    return true;
}

void MovePlayer(Vec3 motion)
{
    const int steps = std::max(1, static_cast<int>(std::ceil(std::max({ std::fabs(motion.X), std::fabs(motion.Y), std::fabs(motion.Z) }) / 0.1f)));
    const Vec3 step = motion * (1.0f / static_cast<float>(steps));
    for (int i = 0; i < steps; ++i)
    {
        if (!MovePlayerAxis(g_WorldState.Position, step.X, 0))
        {
            g_WorldState.Velocity.X = 0.0f;
        }
        if (!MovePlayerAxis(g_WorldState.Position, step.Y, 1))
        {
            if (step.Y < 0.0f)
            {
                g_WorldState.OnGround = true;
            }
            g_WorldState.Velocity.Y = 0.0f;
        }
        if (!MovePlayerAxis(g_WorldState.Position, step.Z, 2))
        {
            g_WorldState.Velocity.Z = 0.0f;
        }
    }
}

bool ProjectWorldPoint(const Vec3& point, const Vec3& camera_position, const Vec3& right, const Vec3& up, const Vec3& forward, float aspect, const ImVec2& viewport_pos, const ImVec2& viewport_size, ImVec2& out_screen, float& out_depth)
{
    const Vec3 relative = point - camera_position;
    const float camera_x = Dot(relative, right);
    const float camera_y = Dot(relative, up);
    const float camera_z = Dot(relative, forward);
    if (camera_z <= kNearClipDistance || camera_z >= kFarClipDistance)
    {
        return false;
    }

    const float tan_half_fov = std::tan(kWorldFieldOfViewYRadians * 0.5f);
    const float ndc_x = camera_x / (camera_z * tan_half_fov * aspect);
    const float ndc_y = camera_y / (camera_z * tan_half_fov);
    if (ndc_x < -1.6f || ndc_x > 1.6f || ndc_y < -1.6f || ndc_y > 1.6f)
    {
        return false;
    }

    out_screen.x = viewport_pos.x + (ndc_x * 0.5f + 0.5f) * viewport_size.x;
    out_screen.y = viewport_pos.y + (0.5f - ndc_y * 0.5f) * viewport_size.y;
    out_depth = camera_z;
    return true;
}

void DrawWorldSky(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    draw_list->AddRectFilledMultiColor(
        viewport_pos,
        ImVec2(viewport_pos.x + viewport_size.x, viewport_pos.y + viewport_size.y),
        GetSkyColor(1.0f),
        GetSkyColor(1.0f),
        GetSkyColor(0.0f),
        GetSkyColor(0.0f));
}

void RenderWorldGeometry(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    if (g_WorldState.MeshFaces.empty() || viewport_size.x <= 0.0f || viewport_size.y <= 0.0f)
    {
        return;
    }

    const Vec3 camera_position = { g_WorldState.Position.X, g_WorldState.Position.Y + kEyeHeight, g_WorldState.Position.Z };
    const Vec3 forward = GetCameraForward();
    const Vec3 right = GetCameraRight();
    const Vec3 up = GetCameraUp();
    const float aspect = viewport_size.x / viewport_size.y;

    std::vector<ProjectedFace> projected_faces;
    projected_faces.reserve(g_WorldState.MeshFaces.size());

    for (const MeshFace& face : g_WorldState.MeshFaces)
    {
        const Vec3 to_face = face.Center - camera_position;
        const float depth_along_view = Dot(to_face, forward);
        if (depth_along_view <= kNearClipDistance || depth_along_view >= kFarClipDistance)
        {
            continue;
        }

        if (Dot(face.Normal, camera_position - face.Center) <= 0.0f)
        {
            continue;
        }

        ProjectedFace projected = {};
        projected.Face = &face;
        projected.Depth = depth_along_view;
        projected.Fog = ClampFloat(depth_along_view / kFarClipDistance, 0.0f, 1.0f);

        bool visible = true;
        bool touches_view = false;
        for (int corner_index = 0; corner_index < 4; ++corner_index)
        {
            float corner_depth = 0.0f;
            if (!ProjectWorldPoint(face.Corners[corner_index], camera_position, right, up, forward, aspect, viewport_pos, viewport_size, projected.Screen[corner_index], corner_depth))
            {
                visible = false;
                break;
            }

            const bool inside_view =
                projected.Screen[corner_index].x >= viewport_pos.x &&
                projected.Screen[corner_index].x <= viewport_pos.x + viewport_size.x &&
                projected.Screen[corner_index].y >= viewport_pos.y &&
                projected.Screen[corner_index].y <= viewport_pos.y + viewport_size.y;
            touches_view = touches_view || inside_view;
        }

        if (visible && touches_view)
        {
            projected_faces.push_back(projected);
        }
    }

    std::sort(projected_faces.begin(), projected_faces.end(), [](const ProjectedFace& a, const ProjectedFace& b)
    {
        return a.Depth > b.Depth;
    });

    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    const ImU32 fog_color = GetSkyColor(0.35f);
    const ImU32 wire_color = IM_COL32(20, 20, 20, 210);
    for (const ProjectedFace& projected : projected_faces)
    {
        MenuInternal::TextureSlot* texture = GetTextureForFace(projected.Face->Block, projected.Face->Face);
        if (texture == nullptr || texture->Texture == nullptr)
        {
            continue;
        }

        const ImU32 shaded_color = ShadeColor(IM_COL32_WHITE, projected.Face->Shade);
        const ImVec4 shaded_rgba = ImGui::ColorConvertU32ToFloat4(shaded_color);
        const ImVec4 fog_rgba = ImGui::ColorConvertU32ToFloat4(fog_color);
        const float fog_amount = projected.Fog * 0.4f;
        const ImVec4 mixed = ImVec4(
            LerpFloat(shaded_rgba.x, fog_rgba.x, fog_amount),
            LerpFloat(shaded_rgba.y, fog_rgba.y, fog_amount),
            LerpFloat(shaded_rgba.z, fog_rgba.z, fog_amount),
            1.0f);
        const ImU32 tint = ImGui::ColorConvertFloat4ToU32(mixed);

        draw_list->AddImageQuad(
            texture->Texture->GetTexRef(),
            projected.Screen[0],
            projected.Screen[1],
            projected.Screen[2],
            projected.Screen[3],
            ImVec2(0.0f, 0.0f),
            ImVec2(1.0f, 0.0f),
            ImVec2(1.0f, 1.0f),
            ImVec2(0.0f, 1.0f),
            tint);

        if (g_WorldState.ShowWireframe)
        {
            draw_list->AddPolyline(projected.Screen, 4, wire_color, ImDrawFlags_Closed, 1.2f);
        }
    }
}

void DrawWorldHud(const ImVec2& viewport_pos, const ImVec2& viewport_size)
{
    ImDrawList* draw_list = ImGui::GetForegroundDrawList();
    const ImVec2 center = ImVec2(viewport_pos.x + viewport_size.x * 0.5f, viewport_pos.y + viewport_size.y * 0.5f);
    draw_list->AddLine(ImVec2(center.x - 6.0f, center.y), ImVec2(center.x + 6.0f, center.y), IM_COL32(255, 255, 255, 220), 1.5f);
    draw_list->AddLine(ImVec2(center.x, center.y - 6.0f), ImVec2(center.x, center.y + 6.0f), IM_COL32(255, 255, 255, 220), 1.5f);

    char debug_line[128] = {};
    std::snprintf(debug_line, sizeof(debug_line), "%s  x=%.1f y=%.1f z=%.1f  faces=%zu  wire=%s", g_WorldState.Meta.Name.c_str(), g_WorldState.Position.X, g_WorldState.Position.Y, g_WorldState.Position.Z, g_WorldState.MeshFaces.size(), g_WorldState.ShowWireframe ? "on" : "off");
    MenuInternal::DrawTextOutlined(draw_list, MenuInternal::g_FontSubtitle, 24.0f * g_WorldState.MainScale, ImVec2(viewport_pos.x + 16.0f * g_WorldState.MainScale, viewport_pos.y + 16.0f * g_WorldState.MainScale), IM_COL32(245, 245, 245, 255), IM_COL32(0, 0, 0, 255), 1.0f * g_WorldState.MainScale, debug_line);
    MenuInternal::DrawTextOutlined(draw_list, MenuInternal::g_FontSubtitle, 20.0f * g_WorldState.MainScale, ImVec2(viewport_pos.x + 16.0f * g_WorldState.MainScale, viewport_pos.y + viewport_size.y - 40.0f * g_WorldState.MainScale), IM_COL32(255, 242, 90, 255), IM_COL32(0, 0, 0, 255), 1.0f * g_WorldState.MainScale, "WASD - Fly  Space/Shift - Up/Down  F3 - Wireframe  Esc - Save and Quit");
}
}

bool InitializeWorldSystem(float main_scale)
{
    g_WorldState.SystemInitialized = true;
    g_WorldState.MainScale = main_scale;
    return EnsureWorldTexturesLoaded();
}

bool InitializeWorldRenderer(const AppVulkanContext& context)
{
    g_WorldRenderer.Context = context;
    g_WorldRenderer.Initialized = true;
    g_WorldRenderer.PendingTextureUpload = true;
    g_WorldRenderer.PendingMeshUpload = true;
    return true;
}

void ShutdownWorldRenderer()
{
    g_WorldRenderer = {};
}

void OnWorldRendererSwapchainChanged()
{
    g_WorldRenderer.PendingTextureUpload = true;
    g_WorldRenderer.PendingMeshUpload = true;
}

void ShutdownWorldSystem()
{
    LeaveWorld();
    ResetWorldTextures();
    g_WorldState.SystemInitialized = false;
}

bool CreateWorldFromMenu(const char* world_name, const char* seed_text)
{
    const std::string name = world_name != nullptr ? world_name : "";
    if (name.empty())
    {
        MenuInternal::g_MenuStatusMessage = "Enter a world name first.";
        MenuInternal::g_MenuStatusTime = SDL_GetTicks();
        return false;
    }

    WorldMeta meta = {};
    meta.Name = name;
    meta.Seed = ParseSeedText(seed_text);
    meta.DirectoryName = MakeSafeDirectoryName(name) + "_" + std::to_string(meta.Seed);
    meta.Spawn.X = 8.0f;
    meta.Spawn.Z = 8.0f;
    meta.Spawn.Y = static_cast<float>(ComputeTerrainHeight(8, 8, meta.Seed) + 1);
    if (!SaveWorldMeta(meta))
    {
        MenuInternal::g_MenuStatusMessage = "Failed to create save folder.";
        MenuInternal::g_MenuStatusTime = SDL_GetTicks();
        return false;
    }

    for (int chunk_z = -kWorldRadiusChunks; chunk_z <= kWorldRadiusChunks; ++chunk_z)
    {
        for (int chunk_x = -kWorldRadiusChunks; chunk_x <= kWorldRadiusChunks; ++chunk_x)
        {
            Chunk chunk = GenerateChunk(meta, chunk_x, chunk_z);
            if (!SaveChunk(meta, chunk))
            {
                MenuInternal::g_MenuStatusMessage = "Failed to save generated chunks.";
                MenuInternal::g_MenuStatusTime = SDL_GetTicks();
                return false;
            }
        }
    }

    MenuInternal::RefreshPlayGameWorldEntries();
    return LoadWorldFromMenu(meta.DirectoryName.c_str());
}

bool LoadWorldFromMenu(const char* world_directory_name)
{
    if (world_directory_name == nullptr || world_directory_name[0] == '\0')
    {
        return false;
    }

    WorldMeta meta = {};
    if (!LoadWorldMeta(GetWorldMetaPath(world_directory_name), meta))
    {
        MenuInternal::g_MenuStatusMessage = "Failed to load world metadata.";
        MenuInternal::g_MenuStatusTime = SDL_GetTicks();
        return false;
    }

    if (!EnsureWorldTexturesLoaded())
    {
        MenuInternal::g_MenuStatusMessage = "Failed to load block textures.";
        MenuInternal::g_MenuStatusTime = SDL_GetTicks();
        return false;
    }

    LeaveWorld();
    g_WorldState.Meta = meta;
    EnsureWorldLoadedAroundSpawn(meta);
    g_WorldState.Position = meta.Spawn;
    g_WorldState.Position.Y += 0.02f;
    g_WorldState.Velocity = {};
    g_WorldState.Yaw = kInitialWorldYaw;
    g_WorldState.Pitch = kInitialWorldPitch;
    BuildWorldMesh();
    g_WorldState.Loaded = true;
    g_WorldState.OnGround = false;
    return true;
}

void SaveCurrentWorld()
{
    if (!g_WorldState.Loaded)
    {
        return;
    }

    g_WorldState.Meta.Spawn = g_WorldState.Position;
    SaveWorldMeta(g_WorldState.Meta);
    for (const Chunk& chunk : g_WorldState.Chunks)
    {
        SaveChunk(g_WorldState.Meta, chunk);
    }
}

void LeaveWorld()
{
    g_WorldState.Loaded = false;
    g_WorldState.Meta = {};
    g_WorldState.Chunks.clear();
    g_WorldState.MeshFaces.clear();
    g_WorldState.Position = {};
    g_WorldState.Velocity = {};
    g_WorldState.OnGround = false;
    g_WorldState.MouseCaptured = false;
    g_WorldState.ShowWireframe = false;
}

bool IsWorldLoaded()
{
    return g_WorldState.Loaded;
}

void HandleWorldEvent(const SDL_Event& event, SDL_Window* window)
{
    if (!g_WorldState.Loaded)
    {
        return;
    }

    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && !g_WorldState.MouseCaptured)
    {
        SetMouseCaptured(window, true);
    }

    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.key == SDLK_F3)
    {
        g_WorldState.ShowWireframe = !g_WorldState.ShowWireframe;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION && g_WorldState.MouseCaptured)
    {
        g_WorldState.Yaw += event.motion.xrel * kMouseSensitivity;
        g_WorldState.Pitch -= event.motion.yrel * kMouseSensitivity;
        g_WorldState.Pitch = ClampFloat(g_WorldState.Pitch, -1.4f, 1.4f);
    }
}

void UpdateWorld(float delta_seconds, SDL_Window* window)
{
    if (!g_WorldState.Loaded)
    {
        return;
    }

    if (!g_WorldState.MouseCaptured)
    {
        SetMouseCaptured(window, true);
    }

    int key_count = 0;
    const bool* keys = SDL_GetKeyboardState(&key_count);
    Vec3 move = {};
    const Vec3 forward = GetCameraForward();
    const Vec3 right = GetCameraRight();
    if (keys != nullptr)
    {
        if (keys[SDL_SCANCODE_W]) move = move + forward;
        if (keys[SDL_SCANCODE_S]) move = move - forward;
        if (keys[SDL_SCANCODE_D]) move = move + right;
        if (keys[SDL_SCANCODE_A]) move = move - right;
        if (keys[SDL_SCANCODE_SPACE]) move.Y += 1.0f;
        if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) move.Y -= 1.0f;
    }

    move = Normalize(move);
    move = move * (kBaseMoveSpeed * delta_seconds);
    g_WorldState.Position = g_WorldState.Position + move;
    g_WorldState.Velocity = {};
    g_WorldState.OnGround = false;
}

void RenderWorld()
{
    if (!g_WorldState.Loaded || !EnsureWorldTexturesLoaded())
    {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 viewport_pos = viewport->Pos;
    const ImVec2 viewport_size = viewport->Size;
    if (viewport_size.x <= 0.0f || viewport_size.y <= 0.0f)
    {
        return;
    }

    DrawWorldSky(viewport_pos, viewport_size);
    RenderWorldGeometry(viewport_pos, viewport_size);
    DrawWorldHud(viewport_pos, viewport_size);
}

void RenderWorldVulkan(VkCommandBuffer, int, int)
{
}
