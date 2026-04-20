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

#include "../../stb/stb_image.h"



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

bool CopyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = g_WorldRenderer.UploadCommandPool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    if (g_WorldRenderer.Context.AllocateCommandBuffers(g_WorldRenderer.Context.Device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
        return false;
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_WorldRenderer.Context.BeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = 0;
    copyRegion.dstOffset = 0;
    copyRegion.size = size;
    g_WorldRenderer.Context.CmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    g_WorldRenderer.Context.EndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    g_WorldRenderer.Context.QueueSubmit(g_WorldRenderer.Context.Queue, 1, &submitInfo, VK_NULL_HANDLE);
    g_WorldRenderer.Context.QueueWaitIdle(g_WorldRenderer.Context.Queue);

    g_WorldRenderer.Context.FreeCommandBuffers(g_WorldRenderer.Context.Device, g_WorldRenderer.UploadCommandPool, 1, &commandBuffer);

    return true;
}

void UploadWorldMeshData()
{
    if (!g_WorldRenderer.PendingMeshUpload || g_WorldRenderer.CpuVertices.empty() || g_WorldRenderer.CpuIndices.empty()) {
        return;
    }

    VkDeviceSize vertexBufferSize = sizeof(g_WorldRenderer.CpuVertices[0]) * g_WorldRenderer.CpuVertices.size();
    VkDeviceSize indexBufferSize = sizeof(g_WorldRenderer.CpuIndices[0]) * g_WorldRenderer.CpuIndices.size();

    GpuBuffer stagingVertexBuffer;
    if (!CreateBufferResource(vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingVertexBuffer)) {
        return;
    }

    void* data;
    g_WorldRenderer.Context.MapMemory(g_WorldRenderer.Context.Device, stagingVertexBuffer.Memory, 0, vertexBufferSize, 0, &data);
    memcpy(data, g_WorldRenderer.CpuVertices.data(), (size_t)vertexBufferSize);
    g_WorldRenderer.Context.UnmapMemory(g_WorldRenderer.Context.Device, stagingVertexBuffer.Memory);

    if (!CreateBufferResource(vertexBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, g_WorldRenderer.VertexBuffer)) {
        return;
    }
    CopyBuffer(stagingVertexBuffer.Buffer, g_WorldRenderer.VertexBuffer.Buffer, vertexBufferSize);
    DestroyGpuBuffer(stagingVertexBuffer);

    GpuBuffer stagingIndexBuffer;
    if (!CreateBufferResource(indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingIndexBuffer)) {
        return;
    }

    g_WorldRenderer.Context.MapMemory(g_WorldRenderer.Context.Device, stagingIndexBuffer.Memory, 0, indexBufferSize, 0, &data);
    memcpy(data, g_WorldRenderer.CpuIndices.data(), (size_t)indexBufferSize);
    g_WorldRenderer.Context.UnmapMemory(g_WorldRenderer.Context.Device, stagingIndexBuffer.Memory);

    if (!CreateBufferResource(indexBufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, g_WorldRenderer.IndexBuffer)) {
        return;
    }
    CopyBuffer(stagingIndexBuffer.Buffer, g_WorldRenderer.IndexBuffer.Buffer, indexBufferSize);
    DestroyGpuBuffer(stagingIndexBuffer);

    g_WorldRenderer.PendingMeshUpload = false;
    g_WorldRenderer.GpuReady = true;
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
    return; // CPU rendering disabled

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


void UploadWorldTexturesVulkan()
{
    if (!g_WorldRenderer.PendingTextureUpload) return;
    
    int texWidth = 0, texHeight = 0, texChannels = 0;
    std::vector<unsigned char> allPixels;
    
    for (uint32_t i = 0; i < kWorldTextureLayerCount; ++i) {
        std::string path = GetRuntimePath(kWorldTextureFiles[i]);
        int w, h, channels;
        unsigned char* pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
        if (!pixels) {
            std::fprintf(stderr, "Failed to load texture: %s\n", path.c_str());
            return;
        }
        if (i == 0) {
            texWidth = w;
            texHeight = h;
            texChannels = 4;
        } else if (w != texWidth || h != texHeight) {
            std::fprintf(stderr, "Texture size mismatch: %s\n", path.c_str());
            stbi_image_free(pixels);
            return;
        }
        
        allPixels.insert(allPixels.end(), pixels, pixels + (w * h * 4));
        stbi_image_free(pixels);
    }
    
    VkDeviceSize imageSize = static_cast<VkDeviceSize>(texWidth) * static_cast<VkDeviceSize>(texHeight) * 4 * kWorldTextureLayerCount;
    
    GpuBuffer stagingBuffer;
    if (!CreateBufferResource(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer)) {
        return;
    }
    
    void* data;
    g_WorldRenderer.Context.MapMemory(g_WorldRenderer.Context.Device, stagingBuffer.Memory, 0, imageSize, 0, &data);
    memcpy(data, allPixels.data(), static_cast<size_t>(imageSize));
    g_WorldRenderer.Context.UnmapMemory(g_WorldRenderer.Context.Device, stagingBuffer.Memory);
    
    // Destroy old image/view/sampler if they exist
    if (g_WorldRenderer.TextureArray.Sampler != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroySampler(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.Sampler, g_WorldRenderer.Context.Allocator);
    }
    if (g_WorldRenderer.TextureArray.View != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroyImageView(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.View, g_WorldRenderer.Context.Allocator);
    }
    if (g_WorldRenderer.TextureArray.Image != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroyImage(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.Image, g_WorldRenderer.Context.Allocator);
    }
    if (g_WorldRenderer.TextureArray.Memory != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.FreeMemory(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.Memory, g_WorldRenderer.Context.Allocator);
    }
    g_WorldRenderer.TextureArray = {};
    
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = static_cast<uint32_t>(texWidth);
    imageInfo.extent.height = static_cast<uint32_t>(texHeight);
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = kWorldTextureLayerCount;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    
    if (g_WorldRenderer.Context.CreateImage(g_WorldRenderer.Context.Device, &imageInfo, g_WorldRenderer.Context.Allocator, &g_WorldRenderer.TextureArray.Image) != VK_SUCCESS) {
        DestroyGpuBuffer(stagingBuffer);
        return;
    }
    
    VkMemoryRequirements memReqs;
    g_WorldRenderer.Context.GetImageMemoryRequirements(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.Image, &memReqs);
    
    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memReqs.size;
    allocInfo.memoryTypeIndex = FindMemoryTypeIndex(g_WorldRenderer.Context, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    
    if (g_WorldRenderer.Context.AllocateMemory(g_WorldRenderer.Context.Device, &allocInfo, g_WorldRenderer.Context.Allocator, &g_WorldRenderer.TextureArray.Memory) != VK_SUCCESS) {
        DestroyGpuBuffer(stagingBuffer);
        return;
    }
    
    g_WorldRenderer.Context.BindImageMemory(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.Image, g_WorldRenderer.TextureArray.Memory, 0);
    
    // Copy buffer to image
    VkCommandBufferAllocateInfo cmdAllocInfo{};
    cmdAllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAllocInfo.commandPool = g_WorldRenderer.UploadCommandPool;
    cmdAllocInfo.commandBufferCount = 1;
    
    VkCommandBuffer commandBuffer;
    g_WorldRenderer.Context.AllocateCommandBuffers(g_WorldRenderer.Context.Device, &cmdAllocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    g_WorldRenderer.Context.BeginCommandBuffer(commandBuffer, &beginInfo);
    
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = g_WorldRenderer.TextureArray.Image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = kWorldTextureLayerCount;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    
    g_WorldRenderer.Context.CmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );
    
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = kWorldTextureLayerCount;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {
        static_cast<uint32_t>(texWidth),
        static_cast<uint32_t>(texHeight),
        1
    };
    
    g_WorldRenderer.Context.CmdCopyBufferToImage(commandBuffer, stagingBuffer.Buffer, g_WorldRenderer.TextureArray.Image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    
    g_WorldRenderer.Context.CmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier
    );
    
    g_WorldRenderer.Context.EndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    
    g_WorldRenderer.Context.QueueSubmit(g_WorldRenderer.Context.Queue, 1, &submitInfo, VK_NULL_HANDLE);
    g_WorldRenderer.Context.QueueWaitIdle(g_WorldRenderer.Context.Queue);
    g_WorldRenderer.Context.FreeCommandBuffers(g_WorldRenderer.Context.Device, g_WorldRenderer.UploadCommandPool, 1, &commandBuffer);
    
    DestroyGpuBuffer(stagingBuffer);
    
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = g_WorldRenderer.TextureArray.Image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = kWorldTextureLayerCount;
    
    g_WorldRenderer.Context.CreateImageView(g_WorldRenderer.Context.Device, &viewInfo, g_WorldRenderer.Context.Allocator, &g_WorldRenderer.TextureArray.View);
    
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    
    g_WorldRenderer.Context.CreateSampler(g_WorldRenderer.Context.Device, &samplerInfo, g_WorldRenderer.Context.Allocator, &g_WorldRenderer.TextureArray.Sampler);
    
    if (g_WorldRenderer.DescriptorSet == VK_NULL_HANDLE) {
        VkDescriptorSetAllocateInfo dsAllocInfo{};
        dsAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsAllocInfo.descriptorPool = g_WorldRenderer.Context.DescriptorPool;
        dsAllocInfo.descriptorSetCount = 1;
        dsAllocInfo.pSetLayouts = &g_WorldRenderer.DescriptorSetLayout;
        if (g_WorldRenderer.Context.AllocateDescriptorSets(g_WorldRenderer.Context.Device, &dsAllocInfo, &g_WorldRenderer.DescriptorSet) != VK_SUCCESS) {
            std::fprintf(stderr, "Failed to allocate descriptor set\n");
            return;
        }
    }
    
    VkDescriptorImageInfo imageInfoDesc{};
    imageInfoDesc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfoDesc.imageView = g_WorldRenderer.TextureArray.View;
    imageInfoDesc.sampler = g_WorldRenderer.TextureArray.Sampler;
    
    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = g_WorldRenderer.DescriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfoDesc;
    
    g_WorldRenderer.Context.UpdateDescriptorSets(g_WorldRenderer.Context.Device, 1, &descriptorWrite, 0, nullptr);
    
    g_WorldRenderer.PendingTextureUpload = false;
}

bool InitializeWorldSystem(float main_scale)
{
    g_WorldState.SystemInitialized = true;
    g_WorldState.MainScale = main_scale;
    return EnsureWorldTexturesLoaded();
}


#include <fstream>

static std::vector<uint32_t> LoadSpvFile(const wchar_t* filename)
{
    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) return {};
    size_t fileSize = (size_t)file.tellg();
    std::vector<uint32_t> buffer(fileSize / sizeof(uint32_t));
    file.seekg(0);
    file.read((char*)buffer.data(), fileSize);
    file.close();
    return buffer;
}

static VkShaderModule CreateShaderModule(const AppVulkanContext& ctx, const std::vector<uint32_t>& code)
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size() * sizeof(uint32_t);
    createInfo.pCode = code.data();
    VkShaderModule shaderModule;
    if (ctx.CreateShaderModule(ctx.Device, &createInfo, ctx.Allocator, &shaderModule) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return shaderModule;
}

bool CreateWorldGraphicsPipeline()
{
    auto& ctx = g_WorldRenderer.Context;

    auto vertShaderCode = LoadSpvFile(Utf8Path("C:/Users/Пользователь/Desktop/Minecraft legacy/Minecraft legacy/src/world/shaders/chunk.vert.spv").c_str());
    auto fragShaderCode = LoadSpvFile(Utf8Path("C:/Users/Пользователь/Desktop/Minecraft legacy/Minecraft legacy/src/world/shaders/chunk.frag.spv").c_str());
    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        std::fprintf(stderr, "Failed to load shader files from absolute paths\n");
        return false;
    }

    VkShaderModule vertShaderModule = CreateShaderModule(ctx, vertShaderCode);
    VkShaderModule fragShaderModule = CreateShaderModule(ctx, fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

    // Vertex Input
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(GpuVertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attributeDescriptions[3]{};
    // Position
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(GpuVertex, Position);
    // UV & Layer
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(GpuVertex, TexCoord);
    // Color
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    attributeDescriptions[2].offset = offsetof(GpuVertex, Color);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = 3;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Descriptor Set Layout
    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 0;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.pImmutableSamplers = nullptr;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerLayoutBinding;
    if (ctx.CreateDescriptorSetLayout(ctx.Device, &layoutInfo, ctx.Allocator, &g_WorldRenderer.DescriptorSetLayout) != VK_SUCCESS)
        return false;

    // Push Constants
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(Mat4);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &g_WorldRenderer.DescriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (ctx.CreatePipelineLayout(ctx.Device, &pipelineLayoutInfo, ctx.Allocator, &g_WorldRenderer.PipelineLayout) != VK_SUCCESS)
        return false;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = g_WorldRenderer.PipelineLayout;
    pipelineInfo.renderPass = ctx.RenderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    if (ctx.CreateGraphicsPipelines(ctx.Device, VK_NULL_HANDLE, 1, &pipelineInfo, ctx.Allocator, &g_WorldRenderer.Pipeline) != VK_SUCCESS)
        return false;

    ctx.DestroyShaderModule(ctx.Device, vertShaderModule, ctx.Allocator);
    ctx.DestroyShaderModule(ctx.Device, fragShaderModule, ctx.Allocator);

    return true;
}

bool InitializeWorldRenderer(const AppVulkanContext& context)
{
    g_WorldRenderer.Context = context;
    if (!CreateWorldGraphicsPipeline()) {
        return false;
    }
    
    // Create Command Pool for uploads
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    poolInfo.queueFamilyIndex = context.QueueFamily;
    if (context.CreateCommandPool(context.Device, &poolInfo, context.Allocator, &g_WorldRenderer.UploadCommandPool) != VK_SUCCESS) {
        return false;
    }
    
    g_WorldRenderer.Initialized = true;
    g_WorldRenderer.PendingTextureUpload = true;
    g_WorldRenderer.PendingMeshUpload = true;
    return true;
}

void ShutdownWorldRenderer()
{
    if (g_WorldRenderer.Context.Device == VK_NULL_HANDLE) {
        g_WorldRenderer = {};
        return;
    }

    if (g_WorldRenderer.TextureArray.Sampler != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroySampler(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.Sampler, g_WorldRenderer.Context.Allocator);
    }
    if (g_WorldRenderer.TextureArray.View != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroyImageView(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.View, g_WorldRenderer.Context.Allocator);
    }
    if (g_WorldRenderer.TextureArray.Image != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroyImage(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.Image, g_WorldRenderer.Context.Allocator);
    }
    if (g_WorldRenderer.TextureArray.Memory != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.FreeMemory(g_WorldRenderer.Context.Device, g_WorldRenderer.TextureArray.Memory, g_WorldRenderer.Context.Allocator);
    }

    DestroyGpuBuffer(g_WorldRenderer.VertexBuffer);
    DestroyGpuBuffer(g_WorldRenderer.IndexBuffer);

    if (g_WorldRenderer.Pipeline != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroyPipeline(g_WorldRenderer.Context.Device, g_WorldRenderer.Pipeline, g_WorldRenderer.Context.Allocator);
    }
    if (g_WorldRenderer.PipelineLayout != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroyPipelineLayout(g_WorldRenderer.Context.Device, g_WorldRenderer.PipelineLayout, g_WorldRenderer.Context.Allocator);
    }
    if (g_WorldRenderer.DescriptorSetLayout != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroyDescriptorSetLayout(g_WorldRenderer.Context.Device, g_WorldRenderer.DescriptorSetLayout, g_WorldRenderer.Context.Allocator);
    }
    if (g_WorldRenderer.UploadCommandPool != VK_NULL_HANDLE) {
        g_WorldRenderer.Context.DestroyCommandPool(g_WorldRenderer.Context.Device, g_WorldRenderer.UploadCommandPool, g_WorldRenderer.Context.Allocator);
    }

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

void RenderWorldVulkan(VkCommandBuffer cmd, int fb_width, int fb_height)
{
    UploadWorldMeshData();
    UploadWorldTexturesVulkan();
    
    if (g_WorldRenderer.VertexBuffer.Buffer == VK_NULL_HANDLE || g_WorldRenderer.IndexBuffer.Buffer == VK_NULL_HANDLE || g_WorldRenderer.CpuIndices.empty()) {
        return;
    }
    if (g_WorldRenderer.DescriptorSet == VK_NULL_HANDLE || g_WorldRenderer.Pipeline == VK_NULL_HANDLE) {
        return;
    }

    g_WorldRenderer.Context.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_WorldRenderer.Pipeline);
    
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(fb_width);
    viewport.height = static_cast<float>(fb_height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    g_WorldRenderer.Context.CmdSetViewport(cmd, 0, 1, &viewport);
    
    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {static_cast<uint32_t>(fb_width), static_cast<uint32_t>(fb_height)};
    g_WorldRenderer.Context.CmdSetScissor(cmd, 0, 1, &scissor);
    
    g_WorldRenderer.Context.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_WorldRenderer.PipelineLayout, 0, 1, &g_WorldRenderer.DescriptorSet, 0, nullptr);
    
    const float aspect = static_cast<float>(fb_width) / static_cast<float>(fb_height);
    Mat4 proj = PerspectiveMatrix(kWorldFieldOfViewYRadians, aspect, kNearClipDistance, kFarClipDistance);
    
    const Vec3 camera_position = { g_WorldState.Position.X, g_WorldState.Position.Y + kEyeHeight, g_WorldState.Position.Z };
    const Vec3 forward = GetCameraForward();
    const Vec3 right = GetCameraRight();
    const Vec3 up = GetCameraUp();
    
    Mat4 view = ViewMatrix(camera_position, forward, right, up);
    Mat4 mvp = MultiplyMatrix(proj, view);
    
    g_WorldRenderer.Context.CmdPushConstants(cmd, g_WorldRenderer.PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4), &mvp);
    
    VkDeviceSize offsets[] = {0};
    g_WorldRenderer.Context.CmdBindVertexBuffers(cmd, 0, 1, &g_WorldRenderer.VertexBuffer.Buffer, offsets);
    g_WorldRenderer.Context.CmdBindIndexBuffer(cmd, g_WorldRenderer.IndexBuffer.Buffer, 0, VK_INDEX_TYPE_UINT32);
    
    g_WorldRenderer.Context.CmdDrawIndexed(cmd, static_cast<uint32_t>(g_WorldRenderer.CpuIndices.size()), 1, 0, 0, 0);
}
