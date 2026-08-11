#include "axklib/writer_internal.hpp"

#include <filesystem>
#include <format>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/file_publication.hpp"
#include "axklib/package_archive.hpp"

namespace axk::detail {
namespace {

constexpr std::uint64_t floppy_image_bytes = 1'474'560U;

Error floppy_error(std::string message) {
    return make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported, std::move(message));
}

std::string disk_path(std::size_t index) { return std::format("payloads/disk{:02}.ima", index); }

nlohmann::ordered_json disk_set_manifest(const FloppyDiskSetPlan &plan, std::span<const FloppyDiskMember> members) {
    nlohmann::ordered_json disks = nlohmann::ordered_json::array();
    for (std::size_t index = 0U; index < plan.disks.size(); ++index) {
        disks.push_back({{"index", index + 1U},
                         {"logicalName", plan.disks[index].name},
                         {"memberMarker", plan.disks[index].marker_name},
                         {"path", disk_path(index + 1U)},
                         {"sizeBytes", floppy_image_bytes},
                         {"sha256", members[index].sha256},
                         {"yamahaSymbolSha256", members[index].yamaha_symbol_sha256}});
    }
    return {{"schema", "axklib.floppy-disk-set.v1"}, {"format", "YAMAHA_A_SERIES_MULTI_FLOPPY"},
            {"diskCount", plan.disks.size()},        {"hardwareValidation", "LOAD_AND_AUDITION_VERIFIED"},
            {"yamahaSymbolMetadata", "SYNTHESIZED"}, {"disks", std::move(disks)}};
}

std::vector<std::byte> json_bytes(const nlohmann::ordered_json &json) {
    auto text = json.dump(2);
    text.push_back('\n');
    const auto bytes = std::as_bytes(std::span{text});
    return {bytes.begin(), bytes.end()};
}

} // namespace

Result<std::uint64_t> projected_floppy_archive_bytes(const FloppyDiskSetPlan &plan) {
    const FloppyDiskMember placeholder{{}, std::string(64U, '0'), std::string(64U, '0')};
    const std::vector<FloppyDiskMember> placeholders(plan.disks.size(), placeholder);
    const auto manifest = json_bytes(disk_set_manifest(plan, placeholders));
    std::uint64_t result = 22U;
    const auto account = [&](std::string_view path, std::uint64_t bytes) -> Result<void> {
        const auto overhead = 76U + 2U * path.size();
        if (bytes > std::numeric_limits<std::uint64_t>::max() - overhead ||
            result > std::numeric_limits<std::uint64_t>::max() - bytes - overhead) {
            return std::unexpected{floppy_error("multi-floppy archive size overflowed")};
        }
        result += bytes + overhead;
        return {};
    };
    if (auto added = account("manifest.json", manifest.size()); !added)
        return std::unexpected{added.error()};
    for (std::size_t index = 1U; index <= plan.disks.size(); ++index) {
        if (auto added = account(disk_path(index), floppy_image_bytes); !added)
            return std::unexpected{added.error()};
    }
    return result;
}

Result<WrittenMediaImage> write_floppy_disk_set(const PreparedMediaImage &image, const FloppyDiskSetPlan &plan,
                                                const std::filesystem::path &output_path, bool overwrite,
                                                const CancellationToken &cancellation) {
    auto members = build_floppy_disk_members(image, plan, cancellation);
    if (!members)
        return std::unexpected{members.error()};

    std::vector<package_internal::ArchiveEntry> entries;
    entries.reserve(members->size() + 1U);
    entries.push_back({"manifest.json", json_bytes(disk_set_manifest(plan, *members))});
    for (std::size_t index = 0U; index < members->size(); ++index)
        entries.push_back({disk_path(index + 1U), std::move((*members)[index].bytes)});
    auto archive = package_internal::write_archive(std::move(entries));
    if (!archive)
        return std::unexpected{archive.error()};
    if (archive->size() != plan.projected_archive_bytes)
        return std::unexpected{floppy_error("multi-floppy archive size differs from its inspected projection")};
    if (archive->size() > image.limits.maximum_output_bytes)
        return std::unexpected{floppy_error("multi-floppy archive exceeds the configured output limit")};
    auto reopened = package_internal::read_archive(*archive);
    if (!reopened)
        return std::unexpected{reopened.error()};
    if (reopened->size() != plan.disks.size() + 1U || reopened->front().path != "manifest.json")
        return std::unexpected{floppy_error("multi-floppy archive failed deterministic reopen validation")};

    if (!overwrite && std::filesystem::exists(output_path))
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "fresh media output already exists")};
    std::error_code filesystem_error;
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error)
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create media output directory")};
    auto publication = TemporaryPublication::create(output_path);
    if (!publication)
        return std::unexpected{publication.error()};
    if (auto resized = publication->resize(archive->size()); !resized)
        return std::unexpected{resized.error()};
    if (auto written = publication->write_at(0U, *archive); !written)
        return std::unexpected{written.error()};
    if (auto flushed = publication->flush(); !flushed)
        return std::unexpected{flushed.error()};
    const auto mode = overwrite ? PublicationMode::replace_existing : PublicationMode::create_only;
    auto published = publication->publish(mode);
    if (!published)
        return std::unexpected{published.error()};
    return WrittenMediaImage{output_path,           MediaImageFormat::fat12_floppy,
                             archive->size(),       image.objects.size(),
                             std::move(*published), MediaConversionArtifactKind::floppy_disk_set,
                             plan.disks.size()};
}

} // namespace axk::detail
