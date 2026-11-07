#include "state_storage_paths.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "axklib/utf8.hpp"

namespace axk::server::detail {
namespace {

std::string component_key(const std::filesystem::path &component) {
    auto value = axk::text::path_to_utf8(component);
#if defined(_WIN32) || defined(__APPLE__)
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
#endif
    return value;
}

bool contains(const std::filesystem::path &root, const std::filesystem::path &path) {
    auto component = path.begin();
    for (const auto &part : root) {
        if (component == path.end() || component_key(part) != component_key(*component))
            return false;
        ++component;
    }
    return true;
}

app::Result<std::filesystem::path> canonical_path(const std::filesystem::path &path) {
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (!error)
        absolute = std::filesystem::weakly_canonical(absolute, error);
    if (error)
        return std::unexpected(app::Error{"invalid_server_argument", "download cache paths cannot be resolved"});
    return absolute;
}

} // namespace

app::Result<void> validate_download_cache_paths(const Config &config) {
    std::error_code error;
    const auto state = config.state_directory.empty() ? std::filesystem::temp_directory_path(error) / "axklib-server"
                                                      : config.state_directory;
    if (error)
        return std::unexpected(app::Error{"invalid_server_argument", "download cache path is unavailable"});
    const auto cache = state / "download-archives";
    const auto resolved_cache = canonical_path(cache);
    if (!resolved_cache)
        return std::unexpected(resolved_cache.error());
    const auto absolute_cache = std::filesystem::absolute(cache, error).lexically_normal();
    if (error)
        return std::unexpected(app::Error{"invalid_server_argument", "download cache path is unavailable"});

    std::vector<std::filesystem::path> persistent;
    if (!config.workspace_store.empty()) {
        persistent.push_back(config.workspace_store);
        persistent.push_back(config.workspace_store.parent_path() /
                             ("." + config.workspace_store.filename().string() + ".owner.lock"));
    }
    if (!config.connection_file.empty()) {
        persistent.push_back(config.connection_file);
        auto control = config.connection_file;
        persistent.push_back(control.replace_extension("shutdown"));
        persistent.push_back(control.replace_extension("shutdown.pending"));
    }
    for (const auto &path : persistent) {
        const auto resolved = canonical_path(path);
        if (!resolved)
            return std::unexpected(resolved.error());
        const auto absolute = std::filesystem::absolute(path, error).lexically_normal();
        if (error)
            return std::unexpected(app::Error{"invalid_server_argument", "download cache paths cannot be resolved"});
        if (contains(absolute_cache, absolute) || contains(*resolved_cache, *resolved)) {
            return std::unexpected(app::Error{"invalid_server_argument",
                                              "persistent metadata must not be inside the disposable download cache"});
        }
    }
    return {};
}

} // namespace axk::server::detail
