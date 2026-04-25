#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace ml {

class AssetPackResolver {
public:
    static constexpr std::string_view kClassicPackId {"classic"};

    explicit AssetPackResolver(std::vector<std::string> overlay_packs = {});

    std::filesystem::path resolve_file(std::string_view relative_path) const;
    std::vector<std::filesystem::path> resolve_directories(std::string_view relative_path) const;
    std::string resolve_file_utf8(std::string_view relative_path) const;

private:
    std::vector<std::string> pack_order_;

    std::vector<std::filesystem::path> candidate_pack_roots(const std::string& pack_id) const;
    static std::filesystem::path relative_pack_root(const std::string& pack_id);
    static std::filesystem::path executable_pack_root(const std::string& pack_id);
};

std::string path_to_utf8(const std::filesystem::path& path);

}
