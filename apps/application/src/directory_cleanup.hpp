#pragma once

#include <filesystem>
#include <system_error>

namespace axk::app::file_operations_internal {
struct DirectoryCleanup {
    std::filesystem::path path;
    ~DirectoryCleanup() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};
} // namespace axk::app::file_operations_internal
