#include "writer_internal.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string_view>

#include "axklib/catalog.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/relationship.hpp"
#include "axklib/utf8.hpp"

namespace axk {
namespace {

bool dependency_relationship(const Relationship &relationship) {
    if (relationship.type == "SBNK_LEFT_MEMBER_TO_SMPL" || relationship.type == "SBNK_RIGHT_MEMBER_TO_SMPL" ||
        relationship.type == "SBAC_SLOT_TO_SBNK") {
        return true;
    }
    return (relationship.type == "PROG_ASSIGNMENT_TO_SBNK" || relationship.type == "PROG_ASSIGNMENT_TO_SBAC") &&
           (relationship.assignment_state == AssignmentState::active ||
            relationship.assignment_state == AssignmentState::source_load);
}

Result<std::vector<detail::PreparedMediaObject>> prepare_transfer(const SavedObjectTransferSpec &transfer,
                                                                  const MediaBuildLimits &limits,
                                                                  const CancellationToken &cancellation) {
    auto media = open_media(transfer.source_path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (limits.maximum_object_bytes == 0U || limits.maximum_object_bytes > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "media build object limit is unsupported")};
    }
    auto inventory = build_media_inventory(*media, MediaObjectReadMode::decoded_metadata,
                                           static_cast<std::size_t>(limits.maximum_object_bytes), cancellation);
    if (!inventory)
        return std::unexpected{inventory.error()};
    std::map<std::string, const MediaObjectDescriptor *> by_key;
    for (const auto &object : inventory->objects)
        by_key.emplace(object.key, &object);
    std::set<std::string> selected;
    if (transfer.selection == SavedObjectSelection::all) {
        if (media->kind() != MediaKind::fat12_floppy) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "whole-source transfer requires "
                                              "one Yamaha FAT12 floppy image")};
        }
        for (const auto &object : inventory->catalog.objects) {
            if (object.object.header.type == ObjectType::unknown) {
                return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                                  "whole-source transfer requires every Yamaha floppy object "
                                                  "to decode cleanly")};
            }
            selected.insert(object.key);
        }
    } else {
        const auto graph = build_relationship_graph(inventory->catalog);
        std::queue<std::string> pending;
        for (const auto &key : transfer.root_object_keys) {
            if (!by_key.contains(key)) {
                return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                                  "transfer root object key does not exist in the "
                                                  "source image: " +
                                                      key)};
            }
            if (selected.insert(key).second)
                pending.push(key);
        }
        while (!pending.empty()) {
            if (const auto check = cancellation.check(); !check)
                return std::unexpected{check.error()};
            auto source = std::move(pending.front());
            pending.pop();
            for (const auto &relationship : graph.relationships) {
                if (relationship.source_key != source || !dependency_relationship(relationship))
                    continue;
                if (relationship.quality != RelationshipQuality::known || !relationship.target_key) {
                    return std::unexpected{make_error(relationship.candidate_keys.size() > 1U
                                                          ? ErrorCode::relationship_ambiguous
                                                          : ErrorCode::relationship_unresolved,
                                                      ErrorCategory::relationship,
                                                      "saved-object transfer requires a known "
                                                      "dependency for " +
                                                          relationship.type)};
                }
                if (!by_key.contains(*relationship.target_key)) {
                    return std::unexpected{make_error(ErrorCode::object_missing, ErrorCategory::object,
                                                      "known transfer dependency is absent from the source")};
                }
                if (selected.insert(*relationship.target_key).second)
                    pending.push(*relationship.target_key);
            }
        }
    }

    std::vector<detail::PreparedMediaObject> result;
    std::uint64_t aggregate_bytes{};
    for (const auto &key : selected) {
        const auto &descriptor = *by_key.at(key);
        if (descriptor.size > limits.maximum_object_bytes ||
            descriptor.size > limits.maximum_aggregate_payload_bytes - aggregate_bytes) {
            return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                              "selected media payloads exceed the configured build limits")};
        }
        auto object =
            load_media_object(*media, descriptor, static_cast<std::size_t>(limits.maximum_object_bytes), cancellation);
        if (!object)
            return std::unexpected{object.error()};
        aggregate_bytes += descriptor.size;
        result.push_back({object->decoded.header.type, object->decoded.header.name, std::move(object->raw_payload)});
    }
    std::ranges::sort(result, [](const auto &left, const auto &right) {
        return std::tuple{left.type, left.name, left.payload.size()} <
               std::tuple{right.type, right.name, right.payload.size()};
    });
    return result;
}

Result<std::vector<detail::PreparedMediaObject>>
prepare_authored(const VolumeSpec &volume, const MediaBuildLimits &limits, const CancellationToken &cancellation) {
    PartitionSpec partition{"MEDIA", {volume}};
    constexpr PartitionGeometry staging_geometry{0, 3, 2'000'000, 1'999'999, 999'999, 2, 1, 3, 512};
    auto records = detail::prepare_partition_records(partition, staging_geometry, 1, cancellation);
    if (!records)
        return std::unexpected{records.error()};
    std::vector<detail::PreparedMediaObject> result;
    std::uint64_t aggregate_bytes{};
    for (auto &record : *records) {
        if (record.kind != detail::RecordKind::object)
            continue;
        auto header = decode_object_header(record.payload);
        if (!header)
            return std::unexpected{header.error()};
        if (record.payload.size() > limits.maximum_object_bytes ||
            record.payload.size() > limits.maximum_aggregate_payload_bytes - aggregate_bytes) {
            return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                              "authored media payloads exceed the configured build limits")};
        }
        aggregate_bytes += record.payload.size();
        result.push_back({header->type, std::move(header->name), std::move(record.payload)});
    }
    std::ranges::sort(result, [](const auto &left, const auto &right) {
        return std::tuple{left.type, left.name, left.payload.size()} <
               std::tuple{right.type, right.name, right.payload.size()};
    });
    return result;
}

class TemporaryFileCleanup {
  public:
    explicit TemporaryFileCleanup(std::filesystem::path path) : path_{std::move(path)} {}
    ~TemporaryFileCleanup() {
        if (active_)
            detail::discard_temporary_file(path_);
    }
    TemporaryFileCleanup(const TemporaryFileCleanup &) = delete;
    TemporaryFileCleanup &operator=(const TemporaryFileCleanup &) = delete;
    void release() noexcept { active_ = false; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

class MediaRangeReader final : public RandomAccessReader {
  public:
    using Read = std::function<Result<std::vector<std::byte>>(std::uint64_t, std::size_t)>;

    MediaRangeReader(std::uint64_t size, Read read) : size_(size), read_(std::move(read)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        if (offset > size_ || destination.size() > size_ - offset) {
            return std::unexpected{
                make_error(ErrorCode::out_of_bounds, ErrorCategory::io, "media validation read is out of bounds")};
        }
        auto bytes = read_(offset, destination.size());
        if (!bytes)
            return std::unexpected{bytes.error()};
        if (bytes->size() != destination.size()) {
            return std::unexpected{
                make_error(ErrorCode::io_short_read, ErrorCategory::io, "media validation read was short")};
        }
        std::ranges::copy(*bytes, destination.begin());
        return {};
    }

  private:
    std::uint64_t size_{};
    Read read_;
};

Result<void> validate_prepared_limits(const detail::PreparedMediaImage &prepared) {
    std::uint64_t aggregate{};
    const auto account = [&](std::size_t size, bool object) -> Result<void> {
        if ((object && size > prepared.limits.maximum_object_bytes) ||
            size > prepared.limits.maximum_aggregate_payload_bytes - aggregate) {
            return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                              "prepared media payloads exceed the configured build limits")};
        }
        aggregate += size;
        return {};
    };
    for (const auto &file : prepared.retained_files) {
        if (auto admitted = account(file.payload.size(), false); !admitted)
            return admitted;
    }
    if (prepared.iso_volumes.empty()) {
        for (const auto &object : prepared.objects) {
            if (auto admitted = account(object.payload.size(), true); !admitted)
                return admitted;
        }
    } else {
        for (const auto &volume : prepared.iso_volumes) {
            for (const auto &object : volume.objects) {
                if (auto admitted = account(object.payload.size(), true); !admitted)
                    return admitted;
            }
        }
    }
    return {};
}

std::size_t prepared_object_count(const detail::PreparedMediaImage &prepared) {
    if (prepared.iso_volumes.empty())
        return prepared.objects.size();
    std::size_t result{};
    for (const auto &volume : prepared.iso_volumes)
        result += volume.objects.size();
    return result;
}

Result<void> validate_written_image(const detail::PreparedMediaImage &prepared, const std::filesystem::path &path,
                                    const CancellationToken &cancellation) {
    auto media = open_media(path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    const auto expected_kind =
        prepared.manifest.format == MediaImageFormat::fat12_floppy ? MediaKind::fat12_floppy : MediaKind::iso9660;
    if (media->kind() != expected_kind) {
        return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                          "written media reopened as the wrong container type")};
    }
    auto inventory =
        build_media_inventory(*media, MediaObjectReadMode::decoded_metadata,
                              static_cast<std::size_t>(prepared.limits.maximum_object_bytes), cancellation);
    if (!inventory)
        return std::unexpected{inventory.error()};
    using PayloadIdentity = std::pair<std::uint64_t, package_internal::Sha256Digest>;
    std::multiset<PayloadIdentity> expected;
    std::multiset<PayloadIdentity> actual;
    if (prepared.iso_volumes.empty()) {
        for (const auto &object : prepared.objects)
            expected.emplace(object.payload.size(), package_internal::sha256(object.payload));
    } else {
        for (const auto &volume : prepared.iso_volumes) {
            for (const auto &object : volume.objects)
                expected.emplace(object.payload.size(), package_internal::sha256(object.payload));
        }
    }
    for (const auto &descriptor : inventory->objects) {
        auto object = load_media_object(*media, descriptor,
                                        static_cast<std::size_t>(prepared.limits.maximum_object_bytes), cancellation);
        if (!object)
            return std::unexpected{object.error()};
        actual.emplace(object->raw_payload.size(), package_internal::sha256(object->raw_payload));
    }
    if (expected != actual) {
        return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                          "written media object payloads failed reopen validation")};
    }
    if (prepared.manifest.format == MediaImageFormat::fat12_floppy) {
        const auto &fat = std::get<FatImage>(media->storage());
        for (const auto &retained : prepared.retained_files) {
            const auto found = std::ranges::find(fat.files(), retained.path, &FatFile::path);
            if (found == fat.files().end()) {
                return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                  "retained FAT12 file is absent after rebuild")};
            }
            MediaRangeReader reader{found->size, [&fat, found, &cancellation](std::uint64_t offset, std::size_t size) {
                                        return fat.read_file_range(*found, offset, size, cancellation);
                                    }};
            const auto digest = package_internal::sha256_reader(reader, cancellation);
            if (!digest)
                return std::unexpected{digest.error()};
            if (*digest != package_internal::sha256(retained.payload)) {
                return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                  "retained FAT12 file changed during rebuild")};
            }
        }
    } else {
        const auto &iso = std::get<IsoImage>(media->storage());
        for (const auto &retained : prepared.retained_files) {
            const auto found = std::ranges::find(iso.files(), retained.path, &IsoFile::path);
            if (found == iso.files().end() || found->is_directory) {
                return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                  "retained ISO9660 file is absent after rebuild")};
            }
            MediaRangeReader reader{found->size, [&iso, found, &cancellation](std::uint64_t offset, std::size_t size) {
                                        return iso.read_file_range(*found, offset, size, cancellation);
                                    }};
            const auto digest = package_internal::sha256_reader(reader, cancellation);
            if (!digest)
                return std::unexpected{digest.error()};
            if (*digest != package_internal::sha256(retained.payload)) {
                return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                  "retained ISO9660 file changed during rebuild")};
            }
        }
    }
    return {};
}

} // namespace

Result<detail::PreparedMediaImage> detail::prepare_media_image(const MediaBuildManifest &manifest,
                                                               const MediaBuildLimits &limits,
                                                               const CancellationToken &cancellation) {
    if (limits.maximum_object_bytes == 0U || limits.maximum_aggregate_payload_bytes == 0U ||
        limits.maximum_output_bytes == 0U || limits.maximum_object_bytes > limits.maximum_aggregate_payload_bytes) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "media build limits are invalid")};
    }
    if ((manifest.schema_version != "1.0" && manifest.schema_version != "1.1") ||
        manifest.transfer.has_value() == manifest.authored_volume.has_value()) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "invalid media build manifest content mode")};
    }
    auto objects = manifest.transfer ? prepare_transfer(*manifest.transfer, limits, cancellation)
                                     : prepare_authored(*manifest.authored_volume, limits, cancellation);
    if (!objects)
        return std::unexpected{objects.error()};
    if (objects->empty() && manifest.format != MediaImageFormat::iso9660) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "media image must contain at least one Yamaha object")};
    }
    return PreparedMediaImage{manifest, limits, std::move(*objects), {}, {}};
}

Result<WrittenMediaImage>
detail::write_prepared_media_image(const PreparedMediaImage &prepared, const std::filesystem::path &output_path,
                                   bool overwrite, const CancellationToken &cancellation,
                                   const std::function<Result<void>(const std::filesystem::path &)> &validator) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
    if (auto admitted = validate_prepared_limits(prepared); !admitted)
        return std::unexpected{admitted.error()};
    if (!overwrite && std::filesystem::exists(output_path)) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "fresh media output already exists")};
    }
    std::error_code filesystem_error;
    if (!output_path.parent_path().empty())
        std::filesystem::create_directories(output_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create media output directory")};
    }
    auto temporary = detail::reserve_temporary_file(output_path);
    if (!temporary)
        return std::unexpected{temporary.error()};
    TemporaryFileCleanup cleanup{*temporary};
    const auto written = prepared.manifest.format == MediaImageFormat::fat12_floppy
                             ? write_fat12_image(prepared, *temporary, cancellation)
                             : write_iso9660_image(prepared, *temporary, cancellation);
    if (!written)
        return std::unexpected{written.error()};
    if (auto validated = validate_written_image(prepared, *temporary, cancellation); !validated)
        return std::unexpected{validated.error()};
    if (validator) {
        if (auto validated = validator(*temporary); !validated)
            return std::unexpected{validated.error()};
    }
    const auto size = std::filesystem::file_size(*temporary, filesystem_error);
    if (filesystem_error) {
        return std::unexpected{
            make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not determine written media image size")};
    }
    if (size > prepared.limits.maximum_output_bytes) {
        return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                          "written media exceeds the configured output limit")};
    }
    if (auto flushed = detail::flush_file_to_disk(*temporary); !flushed)
        return std::unexpected{flushed.error()};
    if (auto published = detail::publish_temporary_file(*temporary, output_path, overwrite); !published)
        return std::unexpected{published.error()};
    cleanup.release();
    return WrittenMediaImage{output_path, prepared.manifest.format, size, prepared_object_count(prepared)};
}

Result<WrittenMediaImage> write_media_image(const MediaBuildManifest &manifest,
                                            const std::filesystem::path &output_path, bool overwrite,
                                            const CancellationToken &cancellation) {
    return write_media_image(manifest, output_path, overwrite, MediaBuildLimits{}, cancellation);
}

Result<WrittenMediaImage> write_media_image(const MediaBuildManifest &manifest,
                                            const std::filesystem::path &output_path, bool overwrite,
                                            const MediaBuildLimits &limits, const CancellationToken &cancellation) {
    auto prepared = detail::prepare_media_image(manifest, limits, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    return detail::write_prepared_media_image(*prepared, output_path, overwrite, cancellation);
}

Result<MediaBuildPlanSummary> plan_media_build(const MediaBuildManifest &manifest,
                                               const CancellationToken &cancellation) {
    return plan_media_build(manifest, MediaBuildLimits{}, cancellation);
}

Result<MediaBuildPlanSummary> plan_media_build(const MediaBuildManifest &manifest, const MediaBuildLimits &limits,
                                               const CancellationToken &cancellation) {
    auto prepared = detail::prepare_media_image(manifest, limits, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    return MediaBuildPlanSummary{manifest.format, prepared->objects.size()};
}

} // namespace axk
