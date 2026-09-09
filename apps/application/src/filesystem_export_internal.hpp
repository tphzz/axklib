#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

#include "axklib/application/filesystem_export.hpp"
#include "image_filesystem_internal.hpp"

namespace axk::app::detail {
struct FilesystemExportPlan {
    FilesystemExportSummary summary;
    std::unordered_map<std::string, FilesystemFileLocator> files;
};

struct StagedFilesystemExport {
    FilesystemExportSummary summary;
    std::filesystem::path path;
    std::shared_ptr<void> source_lease;
    ~StagedFilesystemExport();
};

[[nodiscard]] Result<std::unique_ptr<StagedFilesystemExport>>
stage_filesystem_export(ImageSessionManager &images, const Sandbox &sandbox, std::string_view image_id,
                        std::string_view owner_id, std::uint64_t expected_revision,
                        std::span<const std::string> entry_ids, const CancellationToken &cancellation,
                        ProgressSink *progress, FilesystemExportLayout layout);

[[nodiscard]] Result<FilesystemExportPlan> plan_filesystem_export(const MediaContainer &media,
                                                                  std::span<const std::string> entry_ids,
                                                                  const CancellationToken &cancellation,
                                                                  FilesystemExportLayout layout);
} // namespace axk::app::detail
