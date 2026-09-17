#include "axklib/media.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <tuple>

#include <nlohmann/json.hpp>

#include "axklib/bytes.hpp"
#include "axklib/floppy_catalog_internal.hpp"
#include "axklib/package_archive.hpp"
#include "media_floppy_assembly.hpp"
#include "media_internal.hpp"

namespace axk {
namespace {

constexpr std::uint64_t floppy_image_bytes = 1'474'560U;

Error set_error(ErrorCode code, std::string message, std::string_view source = {}) {
    return detail::media_error(code, std::move(message), source);
}

class ArchiveSliceReader final : public RandomAccessReader {
  public:
    ArchiveSliceReader(std::shared_ptr<const RandomAccessReader> parent, std::uint64_t offset, std::uint64_t size)
        : parent_(std::move(parent)), offset_(offset), size_(size) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        if (offset > size_ || destination.size() > size_ - offset) {
            return std::unexpected{
                make_error(ErrorCode::io_short_read, ErrorCategory::io, "floppy archive member read is out of range")};
        }
        return parent_->read_exact_at(offset_ + offset, destination);
    }

  private:
    std::shared_ptr<const RandomAccessReader> parent_;
    std::uint64_t offset_{};
    std::uint64_t size_{};
};

std::string trimmed(std::string value) {
    while (!value.empty() && value.back() == ' ')
        value.pop_back();
    return value;
}

Result<void> require_exact_keys(const nlohmann::json &value, std::span<const std::string_view> keys,
                                std::string_view subject, std::string_view source) {
    if (!value.is_object() || value.size() != keys.size()) {
        return std::unexpected{
            set_error(ErrorCode::container_invalid_geometry, std::format("{} has an invalid shape", subject), source)};
    }
    for (const auto key : keys) {
        if (!value.contains(key)) {
            return std::unexpected{set_error(ErrorCode::container_invalid_geometry,
                                             std::format("{} is missing '{}'", subject, key), source)};
        }
    }
    return {};
}

template <typename T>
Result<T> required_json(const nlohmann::json &value, std::string_view key, std::string_view source) {
    try {
        return value.at(key).get<T>();
    } catch (const nlohmann::json::exception &) {
        return std::unexpected{set_error(ErrorCode::container_invalid_geometry,
                                         std::format("floppy-set manifest field '{}' has the wrong type", key),
                                         source)};
    }
}

Result<std::string> catalog_digest(const FatImage &image, const CancellationToken &cancellation) {
    const auto catalog = std::ranges::find(image.files(), std::string{"YAMAHA.SYM"}, &FatFile::path);
    if (catalog == image.files().end())
        return std::unexpected{set_error(ErrorCode::container_invalid_geometry, "floppy member has no YAMAHA.SYM")};
    auto bytes = image.read_file(*catalog, cancellation);
    if (!bytes)
        return std::unexpected{bytes.error()};
    return package_internal::hex_digest(package_internal::sha256(*bytes));
}

std::optional<FloppySetMarker> marker_from_name(std::string_view name) {
    if (name == "A3000.SYM")
        return FloppySetMarker::ordinary;
    if (name == "A3000F.SYM")
        return FloppySetMarker::continuation;
    if (name == "A3000E.SYM")
        return FloppySetMarker::final;
    return std::nullopt;
}

Result<detail::FloppyPendingObject> pending_object(const FatImage &member, MediaObject object) {
    const auto file = std::ranges::find(member.files(), object.logical_path, &FatFile::path);
    if (file == member.files().end())
        return std::unexpected{set_error(ErrorCode::object_missing, "floppy object has no backing FAT file")};
    const auto slot = detail::yamaha_floppy_filename_slot(file->name);
    if (!slot || !member.yamaha_catalog())
        return std::unexpected{set_error(ErrorCode::container_invalid_geometry,
                                         std::format("floppy object '{}' has no trusted catalog slot", file->path))};
    const auto entry = std::ranges::find(member.yamaha_catalog()->files, *slot, &YamahaFloppyCatalogEntry::slot);
    if (entry == member.yamaha_catalog()->files.end())
        return std::unexpected{set_error(ErrorCode::container_invalid_geometry,
                                         std::format("floppy object '{}' is not cataloged", file->path))};
    object.logical_path = entry->logical_path;
    object.scope_key = std::format("fat12-floppy-set:{}", member.disk_identity().set_name);
    object.raw_volume = member.disk_identity().set_name;
    object.volume_label = {trimmed(member.disk_identity().set_name), LabelStatus::confirmed,
                           "Yamaha floppy disk-set catalog label"};
    return detail::FloppyPendingObject{std::move(object), *slot, entry->logical_path};
}

} // namespace

Result<FloppyDiskSet> FloppyDiskSet::open(std::vector<FatImage> members, std::string source_name,
                                          const CancellationToken &cancellation) {
    return open_members(std::move(members), std::move(source_name), false, cancellation);
}

Result<FloppyDiskSet> FloppyDiskSet::open_members(std::vector<FatImage> members, std::string source_name,
                                                  bool allow_manifest_verified_ordinary_markers,
                                                  const CancellationToken &cancellation) {
    if (members.empty() || members.size() > maximum_members) {
        return std::unexpected{
            set_error(ErrorCode::invalid_argument, "floppy disk set requires between one and 32 members", source_name)};
    }
    std::ranges::sort(members, {}, [](const FatImage &member) { return member.disk_identity().index; });
    const auto set_name = members.front().disk_identity().set_name;
    for (std::size_t offset = 0U; offset < members.size(); ++offset) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        const auto &identity = members[offset].disk_identity();
        const auto expected = static_cast<std::uint16_t>(offset + 1U);
        const auto verified_ordinary = allow_manifest_verified_ordinary_markers &&
                                       identity.marker == FloppySetMarker::ordinary && identity.set_name == set_name &&
                                       identity.index == expected && members[offset].validation_issues().empty();
        if ((!identity.trusted_for_disk_set && !verified_ordinary) || identity.set_name != set_name ||
            identity.index != expected) {
            return std::unexpected{set_error(ErrorCode::container_invalid_geometry,
                                             std::format("floppy member '{}' is not trusted disk {} of this set",
                                                         members[offset].source_name(), expected),
                                             source_name)};
        }
        const auto valid_intermediate =
            identity.marker == FloppySetMarker::continuation ||
            (allow_manifest_verified_ordinary_markers && identity.marker == FloppySetMarker::ordinary);
        if (offset + 1U < members.size() && !valid_intermediate) {
            return std::unexpected{set_error(ErrorCode::container_invalid_geometry,
                                             "a final floppy marker appears before the last selected member",
                                             source_name)};
        }
    }

    FloppyDiskSet result;
    result.source_name_ = source_name.empty() ? members.front().source_name() : std::move(source_name);
    result.members_ = std::move(members);
    const auto marker = result.members_.back().disk_identity().marker;
    result.status_ = marker == FloppySetMarker::final ? FloppySetStatus::complete : FloppySetStatus::incomplete;
    if (result.status_ == FloppySetStatus::incomplete)
        result.next_required_index_ = static_cast<std::uint16_t>(result.members_.size() + 1U);
    return result;
}

Result<FloppyDiskSet> FloppyDiskSet::open_archive(std::shared_ptr<const RandomAccessReader> reader,
                                                  std::string source_name, const CancellationToken &cancellation) {
    if (!reader)
        return std::unexpected{set_error(ErrorCode::invalid_argument, "floppy archive reader is null", source_name)};
    package_internal::ArchiveLimits limits;
    limits.maximum_entries = maximum_members + 1U;
    limits.maximum_entry_bytes = floppy_image_bytes;
    limits.maximum_total_bytes = maximum_members * floppy_image_bytes + 64U * 1024U;
    limits.maximum_archive_bytes = limits.maximum_total_bytes + 64U * 1024U;
    limits.maximum_directory_bytes = 64U * 1024U;
    limits.maximum_manifest_bytes = 64U * 1024U;
    const auto archive = package_internal::inspect_archive(*reader, cancellation, limits);
    if (!archive)
        return std::unexpected{archive.error()};
    const auto manifest_text =
        std::string(reinterpret_cast<const char *>(archive->manifest.bytes.data()), archive->manifest.bytes.size());
    const auto manifest = nlohmann::json::parse(manifest_text, nullptr, false);
    constexpr std::array manifest_keys{std::string_view{"schema"},
                                       std::string_view{"format"},
                                       std::string_view{"diskCount"},
                                       std::string_view{"hardwareValidation"},
                                       std::string_view{"yamahaSymbolMetadata"},
                                       std::string_view{"disks"}};
    if (manifest.is_discarded())
        return std::unexpected{
            set_error(ErrorCode::container_invalid_geometry, "floppy-set manifest is not valid JSON", source_name)};
    if (auto valid = require_exact_keys(manifest, manifest_keys, "floppy-set manifest", source_name); !valid)
        return std::unexpected{valid.error()};
    const auto schema = required_json<std::string>(manifest, "schema", source_name);
    if (!schema)
        return std::unexpected{schema.error()};
    if (*schema != "axklib.floppy-disk-set.v1") {
        return std::unexpected{
            set_error(ErrorCode::container_unrecognized, "ZIP is not an axklib floppy disk set", source_name)};
    }
    const auto format = required_json<std::string>(manifest, "format", source_name);
    const auto hardware = required_json<std::string>(manifest, "hardwareValidation", source_name);
    const auto symbol_metadata = required_json<std::string>(manifest, "yamahaSymbolMetadata", source_name);
    const auto disk_count = required_json<std::size_t>(manifest, "diskCount", source_name);
    if (!format || !hardware || !symbol_metadata || !disk_count)
        return std::unexpected{(!format            ? format.error()
                                : !hardware        ? hardware.error()
                                : !symbol_metadata ? symbol_metadata.error()
                                                   : disk_count.error())};
    if (*format != "YAMAHA_A_SERIES_MULTI_FLOPPY" || *hardware != "LOAD_AND_AUDITION_VERIFIED" ||
        *symbol_metadata != "SYNTHESIZED" || *disk_count < 2U || *disk_count > maximum_members ||
        !manifest.at("disks").is_array() || manifest.at("disks").size() != *disk_count ||
        archive->entries.size() != *disk_count + 1U) {
        return std::unexpected{
            set_error(ErrorCode::container_invalid_geometry, "floppy-set manifest profile is invalid", source_name)};
    }

    constexpr std::array disk_keys{
        std::string_view{"index"},
        std::string_view{"logicalName"},
        std::string_view{"memberMarker"},
        std::string_view{"path"},
        std::string_view{"sizeBytes"},
        std::string_view{"sha256"},
        std::string_view{"yamahaSymbolSha256"},
    };
    std::vector<FatImage> members;
    members.reserve(*disk_count);
    for (std::size_t offset = 0U; offset < *disk_count; ++offset) {
        const auto &disk = manifest.at("disks").at(offset);
        if (auto valid = require_exact_keys(disk, disk_keys, "floppy-set disk entry", source_name); !valid)
            return std::unexpected{valid.error()};
        const auto index = required_json<std::size_t>(disk, "index", source_name);
        const auto logical_name = required_json<std::string>(disk, "logicalName", source_name);
        const auto marker = required_json<std::string>(disk, "memberMarker", source_name);
        const auto path = required_json<std::string>(disk, "path", source_name);
        const auto size = required_json<std::uint64_t>(disk, "sizeBytes", source_name);
        const auto digest = required_json<std::string>(disk, "sha256", source_name);
        const auto symbol_digest = required_json<std::string>(disk, "yamahaSymbolSha256", source_name);
        if (!index || !logical_name || !marker || !path || !size || !digest || !symbol_digest)
            return std::unexpected{set_error(ErrorCode::container_invalid_geometry,
                                             "floppy-set disk entry has an invalid field", source_name)};
        const auto expected_path = std::format("payloads/disk{:02}.ima", offset + 1U);
        const auto &entry = archive->entries[offset + 1U];
        if (*index != offset + 1U || *path != expected_path || entry.path != expected_path ||
            *size != floppy_image_bytes || entry.size != floppy_image_bytes) {
            return std::unexpected{set_error(ErrorCode::container_invalid_geometry,
                                             "floppy-set disk order, path, or size is invalid", source_name)};
        }
        auto slice = std::make_shared<ArchiveSliceReader>(reader, entry.data_offset, entry.size);
        const auto actual_digest = package_internal::sha256_reader(*slice, cancellation);
        if (!actual_digest || package_internal::hex_digest(*actual_digest) != *digest)
            return std::unexpected{set_error(ErrorCode::container_backup_mismatch,
                                             std::format("floppy-set member {} SHA-256 mismatch", offset + 1U),
                                             source_name)};
        auto member = FatImage::open(slice, entry.path, cancellation);
        if (!member)
            return std::unexpected{member.error()};
        const auto parsed_marker = marker_from_name(*marker);
        const auto valid_marker = offset + 1U == *disk_count ? parsed_marker == FloppySetMarker::final
                                                             : parsed_marker == FloppySetMarker::ordinary ||
                                                                   parsed_marker == FloppySetMarker::continuation;
        const auto actual_symbol_digest = catalog_digest(*member, cancellation);
        if (member->disk_identity().label != *logical_name || !valid_marker ||
            member->disk_identity().marker != *parsed_marker || !actual_symbol_digest ||
            *actual_symbol_digest != *symbol_digest) {
            return std::unexpected{set_error(ErrorCode::container_backup_mismatch,
                                             std::format("floppy-set member {} metadata mismatch", offset + 1U),
                                             source_name)};
        }
        members.push_back(std::move(*member));
    }
    return open_members(std::move(members), std::move(source_name), true, cancellation);
}

const std::string &FloppyDiskSet::source_name() const noexcept { return source_name_; }
const std::vector<FatImage> &FloppyDiskSet::members() const noexcept { return members_; }
FloppySetStatus FloppyDiskSet::status() const noexcept { return status_; }
std::optional<std::uint16_t> FloppyDiskSet::next_required_index() const noexcept { return next_required_index_; }
std::span<const MediaValidationIssue> FloppyDiskSet::validation_issues() const noexcept { return validation_issues_; }

Result<std::vector<MediaObject>> FloppyDiskSet::objects(MediaObjectReadMode mode, std::size_t maximum_object_bytes,
                                                        const CancellationToken &cancellation) const {
    std::vector<detail::FloppyPendingObject> pending;
    for (const auto &member : members_) {
        // Set assembly must compare and join the physical segment bytes even for a metadata-only caller.
        auto objects = member.objects(MediaObjectReadMode::complete, maximum_object_bytes, cancellation);
        if (!objects)
            return std::unexpected{objects.error()};
        for (auto &object : *objects) {
            auto item = pending_object(member, std::move(object));
            if (!item)
                return std::unexpected{item.error()};
            pending.push_back(std::move(*item));
        }
    }

    return detail::assemble_floppy_objects(std::move(pending), status_, maximum_object_bytes, source_name_, mode);
}

} // namespace axk
