#include "common/asset_pack.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <system_error>
#include <utility>

namespace ml {

namespace {

#ifdef __ANDROID__
constexpr const char* kTexturePackRoot = "textures/texture_pack";
#else
constexpr const char* kTexturePackRoot = "assets/textures/texture_pack";
#endif

std::filesystem::path sdl_base_path() {
    const char* base_path = SDL_GetBasePath();
    if (base_path == nullptr) {
        return {};
    }
    return std::filesystem::path(reinterpret_cast<const char8_t*>(base_path));
}

}

AssetPackResolver::AssetPackResolver(std::vector<std::string> overlay_packs) {
    const std::string classic_pack_id(kClassicPackId);
    pack_order_.reserve(overlay_packs.size() + 1);
    for (std::string& pack_id : overlay_packs) {
        if (!pack_id.empty() && pack_id != classic_pack_id) {
            pack_order_.push_back(std::move(pack_id));
        }
    }
    pack_order_.push_back(classic_pack_id);
}

std::filesystem::path AssetPackResolver::resolve_file(std::string_view relative_path) const {
    const std::filesystem::path relative_resource_path {std::string(relative_path)};
#ifdef __ANDROID__
    return relative_pack_root(std::string(kClassicPackId)) / relative_resource_path;
#else
    for (const std::string& pack_id : pack_order_) {
        for (const std::filesystem::path& root : candidate_pack_roots(pack_id)) {
            const std::filesystem::path candidate = root / relative_resource_path;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) {
                return candidate;
            }
        }
    }

    return relative_pack_root(std::string(kClassicPackId)) / relative_resource_path;
#endif
}

std::vector<std::filesystem::path> AssetPackResolver::resolve_directories(std::string_view relative_path) const {
#ifdef __ANDROID__
    (void)relative_path;
    return {};
#else
    const std::filesystem::path relative_resource_path {std::string(relative_path)};
    std::vector<std::filesystem::path> directories;
    for (const std::string& pack_id : pack_order_) {
        for (const std::filesystem::path& root : candidate_pack_roots(pack_id)) {
            const std::filesystem::path candidate = root / relative_resource_path;
            std::error_code error;
            if (!std::filesystem::is_directory(candidate, error)) {
                continue;
            }
            if (std::find(directories.begin(), directories.end(), candidate) == directories.end()) {
                directories.push_back(candidate);
            }
        }
    }
    return directories;
#endif
}

std::string AssetPackResolver::resolve_file_utf8(std::string_view relative_path) const {
    return path_to_utf8(resolve_file(relative_path));
}

std::vector<std::filesystem::path> AssetPackResolver::candidate_pack_roots(const std::string& pack_id) const {
    std::vector<std::filesystem::path> roots;
    roots.push_back(relative_pack_root(pack_id));

    const std::filesystem::path executable_root = executable_pack_root(pack_id);
    if (!executable_root.empty() && executable_root != roots.front()) {
        roots.push_back(executable_root);
    }

    return roots;
}

std::filesystem::path AssetPackResolver::relative_pack_root(const std::string& pack_id) {
    return std::filesystem::path(kTexturePackRoot) / pack_id;
}

std::filesystem::path AssetPackResolver::executable_pack_root(const std::string& pack_id) {
    const std::filesystem::path base_path = sdl_base_path();
    if (base_path.empty()) {
        return {};
    }
    return base_path / kTexturePackRoot / pack_id;
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto utf8 = path.u8string();
    return {reinterpret_cast<const char*>(utf8.data()), utf8.size()};
}

}
