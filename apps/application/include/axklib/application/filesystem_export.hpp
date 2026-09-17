#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/application/filesystem.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/path_reservations.hpp"
#include "axklib/io.hpp"

namespace axk::app {

enum class FilesystemExportLayout { selected_entries, export_folder };

struct FilesystemExportRoot {
    std::string entry_id;
    std::string source_path;
    std::string name;
};

struct FilesystemExportEntry {
    std::string entry_id;
    std::string source_path;
    std::vector<std::string> relative_path;
    bool directory{};
    std::uint64_t size_bytes{};
};

struct FilesystemExportNotice {
    std::string entry_id;
    std::string source_path;
    std::string message;
};

struct FilesystemExportSummary {
    std::optional<FilesystemExportRoot> root_directory;
    std::vector<FilesystemExportEntry> entries;
    std::vector<FilesystemExportNotice> notices;
    std::uint64_t total_bytes{};
};

// Selected directories include descendants. Structural metadata is omitted
// with notices; selecting it directly is rejected. No object conversion occurs.
[[nodiscard]] Result<FilesystemExportSummary>
inspect_filesystem_export(ImageSessionManager &images, std::string_view image_id, std::string_view owner_id,
                          std::uint64_t expected_revision, std::span<const std::string> entry_ids,
                          const CancellationToken &cancellation = {},
                          FilesystemExportLayout layout = FilesystemExportLayout::selected_entries);

// Publish to a new or empty destination through the sandbox's atomic publisher.
[[nodiscard]] Result<FilesystemExportSummary>
export_filesystem_entries(ImageSessionManager &images, const Sandbox &sandbox, PathReservationCoordinator &reservations,
                          std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                          std::span<const std::string> entry_ids, const DirectoryRef &destination,
                          const CancellationToken &cancellation = {}, ProgressSink *progress = nullptr,
                          FilesystemExportLayout layout = FilesystemExportLayout::selected_entries);

} // namespace axk::app
