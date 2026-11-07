#include "download_archive_storage.hpp"

#include <string>
#include <system_error>
#include <utility>

#include "private_storage.hpp"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace axk::app::detail {
namespace {

Error cache_error(std::string message) { return {"archive_storage_unavailable", std::move(message)}; }

} // namespace

Result<void> prepare_download_archive_storage(const std::filesystem::path &path) {
    if (const auto prepared = prepare_private_directory(path); !prepared)
        return std::unexpected(cache_error("download archive cache: " + prepared.error().message));

    std::error_code error;
    std::filesystem::directory_iterator entry{path, error};
    const std::filesystem::directory_iterator end;
    if (error)
        return std::unexpected(cache_error("download archive cache cannot be listed"));
    while (entry != end) {
        const auto status = entry->symlink_status(error);
        if (error || !std::filesystem::is_regular_file(status))
            return std::unexpected(cache_error("download archive cache contains a non-regular entry"));
#if defined(_WIN32)
        const auto attributes = GetFileAttributesW(entry->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            return std::unexpected(cache_error("download archive cache contains an unsafe entry"));
#endif
        // Retained downloads are process-owned cache data, never persistent metadata.
        if (!std::filesystem::remove(entry->path(), error) || error)
            return std::unexpected(cache_error("download archive cache entry cannot be removed"));
        entry.increment(error);
        if (error)
            return std::unexpected(cache_error("download archive cache cannot be listed"));
    }
    return {};
}

} // namespace axk::app::detail
