#include "writer_internal.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <queue>
#include <set>
#include <string_view>

#include "axklib/catalog.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
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
                                                                  const CancellationToken &cancellation) {
    auto media = open_media(transfer.source_path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    auto media_objects = media->objects(64U * 1024U * 1024U, cancellation);
    if (!media_objects)
        return std::unexpected{media_objects.error()};
    std::map<std::string, const MediaObject *> by_key;
    for (const auto &object : *media_objects)
        by_key.emplace(object.key, &object);

    std::set<std::string> selected;
    if (transfer.selection == SavedObjectSelection::all) {
        if (media->kind() != MediaKind::fat12_floppy) {
            return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                              "whole-source transfer requires "
                                              "one Yamaha FAT12 floppy image")};
        }
        for (const auto &object : *media_objects) {
            if (object.decoded.header.type == ObjectType::unknown || object.decode_issue) {
                return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                                  "whole-source transfer requires every Yamaha floppy object "
                                                  "to decode cleanly")};
            }
            selected.insert(object.key);
        }
    } else {
        auto catalog = build_object_catalog(*media, 64U * 1024U * 1024U, cancellation);
        if (!catalog)
            return std::unexpected{catalog.error()};
        const auto graph = build_relationship_graph(*catalog);
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
    for (const auto &key : selected) {
        const auto &object = *by_key.at(key);
        result.push_back({object.decoded.header.type, object.decoded.header.name, object.raw_payload});
    }
    std::ranges::sort(result, [](const auto &left, const auto &right) {
        return std::tuple{left.type, left.name, left.payload.size()} <
               std::tuple{right.type, right.name, right.payload.size()};
    });
    return result;
}

Result<std::vector<detail::PreparedMediaObject>> prepare_authored(const VolumeSpec &volume,
                                                                  const CancellationToken &cancellation) {
    PartitionSpec partition{"MEDIA", {volume}};
    constexpr PartitionGeometry staging_geometry{0, 3, 2'000'000, 1'999'999, 999'999, 2, 1, 3, 512};
    auto records = detail::prepare_partition_records(partition, staging_geometry, 1, cancellation);
    if (!records)
        return std::unexpected{records.error()};
    std::vector<detail::PreparedMediaObject> result;
    for (auto &record : *records) {
        if (record.kind != detail::RecordKind::object)
            continue;
        auto header = decode_object_header(record.payload);
        if (!header)
            return std::unexpected{header.error()};
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
        if (!active_)
            return;
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    TemporaryFileCleanup(const TemporaryFileCleanup &) = delete;
    TemporaryFileCleanup &operator=(const TemporaryFileCleanup &) = delete;
    void release() noexcept { active_ = false; }

  private:
    std::filesystem::path path_;
    bool active_{true};
};

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
    auto objects = media->objects(64U * 1024U * 1024U, cancellation);
    if (!objects)
        return std::unexpected{objects.error()};
    const auto payload_less = [](const std::vector<std::byte> &left, const std::vector<std::byte> &right) {
        if (left.size() != right.size())
            return left.size() < right.size();
        for (std::size_t index = 0; index < left.size(); ++index) {
            if (left[index] != right[index])
                return std::to_integer<unsigned int>(left[index]) < std::to_integer<unsigned int>(right[index]);
        }
        return false;
    };
    std::multiset<std::vector<std::byte>, decltype(payload_less)> expected{payload_less};
    std::multiset<std::vector<std::byte>, decltype(payload_less)> actual{payload_less};
    for (const auto &object : prepared.objects)
        expected.insert(object.payload);
    for (const auto &object : *objects)
        actual.insert(object.raw_payload);
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
            const auto payload = fat.read_file(*found, cancellation);
            if (!payload)
                return std::unexpected{payload.error()};
            if (*payload != retained.payload) {
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
            const auto payload = iso.read_file(*found, cancellation);
            if (!payload)
                return std::unexpected{payload.error()};
            if (*payload != retained.payload) {
                return std::unexpected{make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                                                  "retained ISO9660 file changed during rebuild")};
            }
        }
    }
    return {};
}

} // namespace

Result<detail::PreparedMediaImage> detail::prepare_media_image(const MediaBuildManifest &manifest,
                                                               const CancellationToken &cancellation) {
    if (manifest.schema_version != "1.0" || manifest.transfer.has_value() == manifest.authored_volume.has_value()) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "invalid media build manifest content mode")};
    }
    auto objects = manifest.transfer ? prepare_transfer(*manifest.transfer, cancellation)
                                     : prepare_authored(*manifest.authored_volume, cancellation);
    if (!objects)
        return std::unexpected{objects.error()};
    if (objects->empty() && manifest.format != MediaImageFormat::iso9660) {
        return std::unexpected{make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                                          "media image must contain at least one Yamaha object")};
    }
    return PreparedMediaImage{manifest, std::move(*objects), {}, {}};
}

Result<WrittenMediaImage>
detail::write_prepared_media_image(const PreparedMediaImage &prepared, const std::filesystem::path &output_path,
                                   bool overwrite, const CancellationToken &cancellation,
                                   const std::function<Result<void>(const std::filesystem::path &)> &validator) {
    if (const auto check = cancellation.check(); !check)
        return std::unexpected{check.error()};
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
    if (auto flushed = detail::flush_file_to_disk(*temporary); !flushed)
        return std::unexpected{flushed.error()};
    if (auto published = detail::publish_temporary_file(*temporary, output_path, overwrite); !published)
        return std::unexpected{published.error()};
    cleanup.release();
    return WrittenMediaImage{output_path, prepared.manifest.format, size, prepared.objects.size()};
}

Result<WrittenMediaImage> write_media_image(const MediaBuildManifest &manifest,
                                            const std::filesystem::path &output_path, bool overwrite,
                                            const CancellationToken &cancellation) {
    auto prepared = detail::prepare_media_image(manifest, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    return detail::write_prepared_media_image(*prepared, output_path, overwrite, cancellation);
}

Result<MediaBuildPlanSummary> plan_media_build(const MediaBuildManifest &manifest,
                                               const CancellationToken &cancellation) {
    auto prepared = detail::prepare_media_image(manifest, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    return MediaBuildPlanSummary{manifest.format, prepared->objects.size()};
}

} // namespace axk
