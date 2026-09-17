#include "axklib/application/filesystem_export.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>

#include "axklib/utf8.hpp"
#include "filesystem_export_internal.hpp"

namespace axk::app {
namespace {
Result<void> write_staging(const ImageSessionRead &session, const detail::FilesystemExportPlan &plan,
                           const std::filesystem::path &staging, const CancellationToken &cancellation,
                           ProgressSink *progress) {
    std::uint64_t completed{};
    for (const auto &entry : plan.summary.entries) {
        if (cancellation.is_cancelled())
            return std::unexpected(Error{"operation_cancelled", "Filesystem export cancelled"});
        auto path = staging;
        for (const auto &component : entry.relative_path) {
            const auto native = text::path_from_utf8(component);
            if (!native)
                return std::unexpected(Error{"filesystem_export_path", native.error().message});
            path /= *native;
        }
        std::error_code error;
        if (entry.directory) {
            if (!std::filesystem::create_directory(path, error) || error)
                return std::unexpected(Error{"filesystem_export_failed", "Export directory could not be created"});
            continue;
        }
        // Let the host reject additional name equivalences without replacing a staged file.
        std::ofstream output{path, std::ios::binary | std::ios::noreplace};
        if (!output)
            return std::unexpected(Error{"filesystem_export_failed", "Export file could not be opened"});
        std::uint64_t offset{};
        do {
            const auto count =
                static_cast<std::size_t>(std::min<std::uint64_t>(1024U * 1024U, entry.size_bytes - offset));
            auto bytes = detail::read_filesystem_range(*session.media, plan.files.at(entry.entry_id), offset, count,
                                                       cancellation);
            if (!bytes)
                return std::unexpected(bytes.error());
            if (bytes->size() != count)
                return std::unexpected(
                    Error{"filesystem_export_failed", "Filesystem read returned an incomplete range"});
            if (!bytes->empty())
                output.write(reinterpret_cast<const char *>(bytes->data()),
                             static_cast<std::streamsize>(bytes->size()));
            if (!output)
                return std::unexpected(Error{"filesystem_export_failed", "Export file could not be written"});
            offset += count;
            completed += count;
            if (progress)
                progress->report(
                    {ProgressPhase::exporting, completed, plan.summary.total_bytes, entry.source_path, std::nullopt});
        } while (offset < entry.size_bytes);
        output.close();
        if (!output)
            return std::unexpected(Error{"filesystem_export_failed", "Export file could not be closed"});
    }
    if (auto verified = session.verify_source_unchanged(); !verified)
        return std::unexpected(Error{"image_source_changed", verified.error().message, {}, true});
    return {};
}
} // namespace

detail::StagedFilesystemExport::~StagedFilesystemExport() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

Result<std::unique_ptr<detail::StagedFilesystemExport>>
detail::stage_filesystem_export(ImageSessionManager &images, const Sandbox &sandbox, std::string_view image_id,
                                std::string_view owner_id, std::uint64_t expected_revision,
                                std::span<const std::string> entry_ids, const CancellationToken &cancellation,
                                ProgressSink *progress, FilesystemExportLayout layout) {
    if (cancellation.is_cancelled())
        return std::unexpected(Error{"operation_cancelled", "Filesystem export cancelled"});
    auto session = images.begin_read(image_id, owner_id, expected_revision);
    if (!session)
        return std::unexpected(session.error());
    auto plan = plan_filesystem_export(*session->media, entry_ids, cancellation, layout);
    if (!plan)
        return std::unexpected(plan.error());
    const auto staging = sandbox.create_staging_directory("filesystem-export");
    if (!staging)
        return std::unexpected(staging.error());
    auto result = std::make_unique<StagedFilesystemExport>();
    result->path = *staging;
    result->source_lease = session->lease;
    if (auto written = write_staging(*session, *plan, *staging, cancellation, progress); !written)
        return std::unexpected(written.error());
    result->summary = std::move(plan->summary);
    return result;
}

Result<FilesystemExportSummary> inspect_filesystem_export(ImageSessionManager &images, std::string_view image_id,
                                                          std::string_view owner_id, std::uint64_t expected_revision,
                                                          std::span<const std::string> entry_ids,
                                                          const CancellationToken &cancellation,
                                                          FilesystemExportLayout layout) {
    auto session = images.begin_read(image_id, owner_id, expected_revision);
    if (!session)
        return std::unexpected(session.error());
    auto plan = detail::plan_filesystem_export(*session->media, entry_ids, cancellation, layout);
    if (!plan)
        return std::unexpected(plan.error());
    return std::move(plan->summary);
}

Result<FilesystemExportSummary>
export_filesystem_entries(ImageSessionManager &images, const Sandbox &sandbox, PathReservationCoordinator &reservations,
                          std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
                          std::span<const std::string> entry_ids, const DirectoryRef &destination,
                          const CancellationToken &cancellation, ProgressSink *progress,
                          FilesystemExportLayout layout) {
    if (cancellation.is_cancelled())
        return std::unexpected(Error{"operation_cancelled", "Filesystem export cancelled"});
    if (const auto resolved = sandbox.resolve_output_directory(destination, false); !resolved)
        return std::unexpected(resolved.error());
    auto destination_lease =
        reservations.try_acquire({{destination.root_id, destination.relative_path}, PathAccessMode::exclusive});
    if (!destination_lease)
        return std::unexpected(destination_lease.error());
    auto staged = detail::stage_filesystem_export(images, sandbox, image_id, owner_id, expected_revision, entry_ids,
                                                  cancellation, progress, layout);
    if (!staged)
        return std::unexpected(staged.error());
    if (progress)
        progress->report({ProgressPhase::publishing, 0U, 1U, "Publishing filesystem export", std::nullopt});
    if (auto published = sandbox.publish_directory(destination, false, (*staged)->path, cancellation); !published)
        return std::unexpected(published.error());
    if (progress)
        progress->report({ProgressPhase::publishing, 1U, 1U, "Filesystem export ready", std::nullopt});
    return std::move((*staged)->summary);
}
} // namespace axk::app
