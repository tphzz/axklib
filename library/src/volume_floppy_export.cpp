#include "axklib/writer_internal.hpp"

#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "axklib/file_publication.hpp"
#include "axklib/io.hpp"
#include "axklib/package_archive.hpp"

namespace axk {
namespace {

constexpr std::uint64_t floppy_image_bytes = 1'474'560U;

Error export_error(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::io, std::move(message));
}

bool valid_directory_name(const std::filesystem::path &path) {
    return !path.empty() && path != "." && path != ".." && path.filename() == path && !path.has_root_path();
}

Result<void> write_member(const std::filesystem::path &path, std::span<const std::byte> bytes) {
    auto publication = detail::TemporaryPublication::create(path);
    if (!publication)
        return std::unexpected{publication.error()};
    if (auto resized = publication->resize(bytes.size()); !resized)
        return std::unexpected{resized.error()};
    if (auto written = publication->write_at(0U, bytes); !written)
        return std::unexpected{written.error()};
    if (auto flushed = publication->flush(); !flushed)
        return std::unexpected{flushed.error()};
    auto published = publication->publish(detail::PublicationMode::create_only);
    if (!published)
        return std::unexpected{published.error()};
    return {};
}

Result<std::string> file_sha256(const std::filesystem::path &path, const CancellationToken &cancellation) {
    auto reader = FileReader::open(path);
    if (!reader)
        return std::unexpected{reader.error()};
    auto digest = package_internal::sha256_reader(**reader, cancellation);
    if (!digest)
        return std::unexpected{digest.error()};
    return package_internal::hex_digest(*digest);
}

} // namespace

Result<WrittenVolumeFloppyExportBatch>
write_volume_floppy_export(std::shared_ptr<const RandomAccessReader> source_reader,
                           const std::filesystem::path &source_path, const VolumeFloppyExportRequest &request,
                           const std::filesystem::path &output_root, std::span<const VolumeFloppyExportTarget> targets,
                           const MediaBuildLimits &limits, const CancellationToken &cancellation) {
    std::map<std::uint32_t, std::filesystem::path> directory_by_volume;
    std::set<std::filesystem::path> directory_names;
    for (const auto &target : targets) {
        if (!valid_directory_name(target.directory_name) ||
            !directory_by_volume.emplace(target.directory_id, target.directory_name).second ||
            !directory_names.emplace(target.directory_name).second) {
            return std::unexpected{export_error("volume floppy export targets must use unique directory names")};
        }
    }
    auto prepared =
        detail::prepare_volume_floppy_export(std::move(source_reader), source_path, request, limits, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    for (const auto &volume : prepared->summary.volumes) {
        if (volume.object_count != 0U && volume.can_export && !directory_by_volume.contains(volume.directory_id)) {
            return std::unexpected{export_error("every exportable volume requires an output directory")};
        }
    }

    std::error_code filesystem_error;
    std::filesystem::create_directories(output_root, filesystem_error);
    if (filesystem_error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create floppy export directory")};
    }

    WrittenVolumeFloppyExportBatch result{prepared->summary, {}};
    result.volumes.reserve(prepared->volumes.size());
    for (std::size_t index = 0U; index < prepared->volumes.size(); ++index) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto &summary = prepared->summary.volumes[index];
        const auto &conversion = prepared->volumes[index];
        WrittenVolumeFloppyExport volume{
            summary.directory_id, summary.name, VolumeFloppyExportStatus::failed, {}, 0U, {}, {}};
        if (summary.object_count == 0U) {
            volume.status = VolumeFloppyExportStatus::skipped_empty;
            result.volumes.push_back(std::move(volume));
            continue;
        }
        if (!summary.can_export) {
            volume.status = VolumeFloppyExportStatus::blocked;
            result.volumes.push_back(std::move(volume));
            continue;
        }

        const auto output_directory = output_root / directory_by_volume.at(summary.directory_id);
        volume.directory_path = output_directory;
        const auto directory_created = std::filesystem::create_directory(output_directory, filesystem_error);
        Result<void> written;
        if (filesystem_error || !directory_created) {
            written = std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                                 "could not create a volume floppy export directory")};
        } else {
            auto plan = detail::plan_floppy_disk_set(conversion.image, summary.name, cancellation);
            if (!plan) {
                written = std::unexpected{plan.error()};
            } else if (plan->disks.size() == 1U) {
                const auto path = output_directory / "disk01.ima";
                auto image = detail::write_prepared_media_image(conversion.image, path, false, cancellation);
                if (!image) {
                    written = std::unexpected{image.error()};
                } else {
                    auto digest = file_sha256(path, cancellation);
                    if (!digest) {
                        written = std::unexpected{digest.error()};
                    } else {
                        volume.disks.push_back({1U, path, image->size_bytes, std::move(*digest)});
                        volume.size_bytes = image->size_bytes;
                    }
                }
            } else {
                auto members = detail::build_floppy_disk_members(conversion.image, *plan, cancellation);
                if (!members) {
                    written = std::unexpected{members.error()};
                } else {
                    for (std::size_t disk_index = 0U; disk_index < members->size(); ++disk_index) {
                        const auto path = output_directory / std::format("disk{:02}.ima", disk_index + 1U);
                        written = write_member(path, (*members)[disk_index].bytes);
                        if (!written)
                            break;
                        volume.disks.push_back(
                            {disk_index + 1U, path, floppy_image_bytes, (*members)[disk_index].sha256});
                        volume.size_bytes += floppy_image_bytes;
                    }
                }
            }
            if (written) {
                filesystem_error.clear();
                static_cast<void>(std::filesystem::remove(output_directory / ".axklib-publication", filesystem_error));
                if (filesystem_error) {
                    written = std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                                         "could not clean volume floppy publication staging")};
                }
            }
        }
        if (!written) {
            if (written.error().code == ErrorCode::operation_cancelled)
                return std::unexpected{written.error()};
            if (directory_created)
                std::filesystem::remove_all(output_directory, filesystem_error);
            volume.status = VolumeFloppyExportStatus::failed;
            volume.directory_path.clear();
            volume.size_bytes = 0U;
            volume.disks.clear();
            volume.error = written.error();
        } else {
            volume.status = VolumeFloppyExportStatus::exported;
        }
        result.volumes.push_back(std::move(volume));
    }
    return result;
}

} // namespace axk
