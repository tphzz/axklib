#include "axklib/writer_internal.hpp"

#include <algorithm>
#include <cctype>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"

#include "relationship_policy.hpp"

namespace axk {
namespace {

constexpr std::uint64_t floppy_image_bytes = 1'474'560U;
constexpr std::size_t maximum_floppy_images = 32U;
constexpr std::string_view generated_raw_group = "46DEF120";

struct SourceVolume {
    std::uint32_t directory_id{};
    std::string name;
    std::vector<std::uint32_t> object_ids;
};

struct ConversionSource {
    std::shared_ptr<const Container> container;
    const Partition *partition{};
    std::vector<SourceVolume> volumes;
    MediaInventory inventory;
    RelationshipGraph graph;
    std::unordered_map<std::uint32_t, const ObjectSnapshot *> objects_by_id;
    std::map<std::string, const ObjectSnapshot *, std::less<>> objects_by_key;
    std::unordered_map<std::uint32_t, const IndexRecord *> records_by_id;
};

class RecordReader final : public RandomAccessReader {
  public:
    RecordReader(std::shared_ptr<const Container> container, PartitionIndex partition, SfsId record, std::uint64_t size,
                 CancellationToken cancellation)
        : container_(std::move(container)), partition_(partition), record_(record), size_(size),
          cancellation_(std::move(cancellation)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        if (offset > size_ || destination.size() > size_ - offset) {
            return std::unexpected{
                make_error(ErrorCode::out_of_bounds, ErrorCategory::io, "media conversion read is out of bounds")};
        }
        auto bytes = container_->read_record_range(partition_, record_, offset, destination.size(), cancellation_);
        if (!bytes)
            return std::unexpected{bytes.error()};
        if (bytes->size() != destination.size()) {
            return std::unexpected{
                make_error(ErrorCode::io_short_read, ErrorCategory::io, "media conversion read was short")};
        }
        std::ranges::copy(*bytes, destination.begin());
        return {};
    }

  private:
    std::shared_ptr<const Container> container_;
    PartitionIndex partition_;
    SfsId record_;
    std::uint64_t size_{};
    CancellationToken cancellation_;
};

bool category_name(std::string_view name) {
    return name == "SMPL" || name == "SBNK" || name == "SBAC" || name == "PROG" || name == "SEQU" || name == "PRF3";
}

bool supported_object(ObjectType type) {
    return type == ObjectType::smpl || type == ObjectType::sbnk || type == ObjectType::sbac ||
           type == ObjectType::prog || type == ObjectType::sequ || type == ObjectType::prf3;
}

bool dependency_relationship(const Relationship &relationship) {
    if (relationship.type == "SBNK_LEFT_MEMBER_TO_SMPL" || relationship.type == "SBNK_RIGHT_MEMBER_TO_SMPL" ||
        relationship.type == "SBAC_SLOT_TO_SBNK") {
        return true;
    }
    return is_effective_program_assignment(relationship);
}

void add_issue(MediaConversionPlanSummary &summary, std::string code, std::string message,
               std::optional<MediaConversionIssueMeasurement> measurement = {}, bool blocking = true) {
    summary.issues.push_back({std::move(code), std::move(message), blocking, measurement});
}

bool retained_unresolved_program_row(const Relationship &relationship,
                                     const std::map<std::string, const ObjectSnapshot *, std::less<>> &objects_by_key) {
    return relationship.assignment_state == AssignmentState::stored_assignment &&
           !relationship.assignment_name.empty() &&
           (relationship.type == "PROG_ASSIGNMENT_TO_SBNK" || relationship.type == "PROG_ASSIGNMENT_TO_SBAC") &&
           relationship.quality != RelationshipQuality::known &&
           !detail::relationship_has_exact_named_program_target(relationship, objects_by_key);
}

Result<std::vector<SourceVolume>> source_volumes(const Partition &partition) {
    std::unordered_map<std::uint32_t, const IndexRecord *> directories;
    for (const auto &record : partition.records) {
        if (record.directory_id)
            directories.emplace(record.directory_id->value, &record);
    }
    const IndexRecord *root{};
    for (const auto &[id, directory] : directories) {
        if (directory->parent_directory_id && directory->parent_directory_id->value == id) {
            root = directory;
            break;
        }
    }
    if (root == nullptr || !root->directory_id) {
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "partition has no unambiguous root directory")};
    }

    std::vector<SourceVolume> result;
    for (const auto &entry : root->directory_entries) {
        const auto found = directories.find(entry.link_id.value);
        if (entry.name == "." || entry.name == ".." || found == directories.end())
            continue;
        const auto &volume = *found->second;
        if (!volume.parent_directory_id || volume.parent_directory_id->value != root->directory_id->value ||
            !volume.directory_id) {
            continue;
        }
        SourceVolume item{volume.sfs_id.value, entry.name, {}};
        for (const auto &category_entry : volume.directory_entries) {
            if (!category_name(category_entry.name))
                continue;
            const auto category_found = directories.find(category_entry.link_id.value);
            if (category_found == directories.end())
                continue;
            const auto &category = *category_found->second;
            if (!category.parent_directory_id || category.parent_directory_id->value != volume.directory_id->value) {
                continue;
            }
            for (const auto &object_entry : category.directory_entries) {
                if (object_entry.name != "." && object_entry.name != "..")
                    item.object_ids.push_back(object_entry.link_id.value);
            }
        }
        result.push_back(std::move(item));
    }
    return result;
}

void validate_iso_layout(detail::PreparedMediaConversion &prepared) {
    auto &summary = prepared.summary;
    const auto &volumes = prepared.image.iso_volumes;
    bool shape_supported = true;
    if (volumes.size() > 998U) {
        add_issue(summary, "MEDIA_CONVERSION_ISO_VOLUME_CAPACITY", "CD-ROM output supports at most 998 source volumes");
        shape_supported = false;
    }
    for (const auto &volume : volumes) {
        std::map<ObjectType, std::size_t> categories;
        for (const auto &object : volume.objects)
            ++categories[object.type];
        for (const auto &[type, count] : categories) {
            static_cast<void>(type);
            if (count > 999U) {
                add_issue(summary, "MEDIA_CONVERSION_ISO_OBJECT_CAPACITY",
                          std::format("Volume '{}' has more than 999 objects in one category", volume.volume_name));
                shape_supported = false;
            }
        }
    }
    if (!shape_supported)
        return;

    const auto layout = detail::plan_iso9660_layout(prepared.image);
    if (!layout) {
        add_issue(summary, "MEDIA_CONVERSION_ISO_LAYOUT_UNSUPPORTED", layout.error().message);
        return;
    }
    summary.projected_output_bytes = layout->output_bytes;
    if (summary.projected_output_bytes > summary.capacity_bytes) {
        add_issue(summary, "MEDIA_CONVERSION_OUTPUT_CAPACITY", "The partition does not fit on a 700 MB CD-ROM image",
                  MediaConversionIssueMeasurement{summary.projected_output_bytes, summary.capacity_bytes,
                                                  MediaConversionIssueUnit::bytes});
    }
}

Result<void> validate_floppy_layout(detail::PreparedMediaConversion &prepared, const CancellationToken &cancellation) {
    auto &summary = prepared.summary;
    const auto volume_name = summary.volumes.empty() ? std::string_view{"AXKLIB"} : summary.volumes.front().name;
    auto plan = detail::plan_floppy_disk_set(prepared.image, volume_name, cancellation);
    if (!plan) {
        if (plan.error().code == ErrorCode::operation_cancelled)
            return std::unexpected{plan.error()};
        add_issue(summary, "MEDIA_CONVERSION_FLOPPY_LAYOUT_UNSUPPORTED", plan.error().message);
        return {};
    }
    summary.floppy_image_count = plan->disks.size();
    if (plan->disks.size() == 1U) {
        summary.artifact_kind = MediaConversionArtifactKind::image;
        summary.output_extension = ".ima";
        summary.projected_output_bytes = floppy_image_bytes;
        summary.capacity_bytes = floppy_image_bytes;
        return {};
    }

    summary.artifact_kind = MediaConversionArtifactKind::floppy_disk_set;
    summary.output_extension = ".zip";
    summary.projected_output_bytes = plan->projected_archive_bytes;
    summary.capacity_bytes = maximum_floppy_images * floppy_image_bytes;
    if (plan->disks.size() > maximum_floppy_images) {
        add_issue(summary, "MEDIA_CONVERSION_FLOPPY_DISK_SET_CAPACITY",
                  "The volume requires more than 32 Yamaha floppy images",
                  MediaConversionIssueMeasurement{plan->disks.size(), maximum_floppy_images,
                                                  MediaConversionIssueUnit::floppy_images});
        return {};
    }
    if (summary.projected_output_bytes > prepared.image.limits.maximum_output_bytes) {
        add_issue(
            summary, "MEDIA_CONVERSION_OUTPUT_CAPACITY", "The multi-floppy archive exceeds the configured output limit",
            MediaConversionIssueMeasurement{summary.projected_output_bytes, prepared.image.limits.maximum_output_bytes,
                                            MediaConversionIssueUnit::bytes});
        return {};
    }
    add_issue(summary, "MEDIA_CONVERSION_MULTI_FLOPPY_SAVE_RELOAD_VALIDATION_PENDING",
              "Multi-floppy load and audition are verified on sampler hardware; sampler save/reload validation is "
              "pending",
              MediaConversionIssueMeasurement{plan->disks.size(), maximum_floppy_images,
                                              MediaConversionIssueUnit::floppy_images},
              false);
    return {};
}

std::string sanitized_iso_id(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return std::isalnum(character) != 0 ? static_cast<char>(std::toupper(character)) : '_';
    });
    value.resize(std::min<std::size_t>(value.size(), 32U));
    return value.empty() ? "AXKLIB" : value;
}

bool valid_limits(const MediaBuildLimits &limits) {
    return limits.maximum_object_bytes != 0U &&
           limits.maximum_object_bytes <= std::numeric_limits<std::size_t>::max() &&
           limits.maximum_aggregate_payload_bytes != 0U && limits.maximum_output_bytes != 0U;
}

Result<std::unique_ptr<ConversionSource>>
open_conversion_source(std::shared_ptr<const RandomAccessReader> source_reader,
                       const std::filesystem::path &source_path, std::uint32_t partition_index,
                       const MediaBuildLimits &limits, const CancellationToken &cancellation) {
    if (source_reader == nullptr || !valid_limits(limits)) {
        return std::unexpected{
            make_error(ErrorCode::invalid_argument, ErrorCategory::io, "media conversion request is invalid")};
    }
    auto media = open_media(std::move(source_reader), source_path, cancellation);
    if (!media)
        return std::unexpected{media.error()};
    if (media->kind() != MediaKind::sfs) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          "media conversion requires an open SFS HDA/HDS source image")};
    }
    auto result = std::make_unique<ConversionSource>();
    result->container = std::make_shared<Container>(std::get<Container>(media->storage()));
    const auto partition = std::ranges::find(result->container->partitions(), partition_index,
                                             [](const Partition &item) { return item.index.value; });
    if (partition == result->container->partitions().end()) {
        return std::unexpected{
            make_error(ErrorCode::object_missing, ErrorCategory::object, "source partition does not exist")};
    }
    result->partition = &*partition;
    auto volumes = source_volumes(*partition);
    if (!volumes)
        return std::unexpected{volumes.error()};
    result->volumes = std::move(*volumes);
    auto inventory = build_media_inventory(*media, MediaObjectReadMode::decoded_metadata,
                                           static_cast<std::size_t>(limits.maximum_object_bytes), cancellation);
    if (!inventory)
        return std::unexpected{inventory.error()};
    result->inventory = std::move(*inventory);
    result->graph = build_relationship_graph(result->inventory.catalog);
    for (const auto &object : result->inventory.catalog.objects) {
        result->objects_by_key.emplace(object.key, &object);
        if (object.partition.value == partition_index)
            result->objects_by_id.emplace(object.sfs_id.value, &object);
    }
    for (const auto &record : partition->records)
        result->records_by_id.emplace(record.sfs_id.value, &record);
    return result;
}

Result<detail::PreparedMediaConversion> prepare_media_conversion_from_source(const ConversionSource &source,
                                                                             const MediaConversionRequest &request,
                                                                             const MediaBuildLimits &limits,
                                                                             const CancellationToken &cancellation) {
    if ((request.format == MediaImageFormat::iso9660 && request.scope != MediaConversionScope::partition) ||
        (request.format == MediaImageFormat::fat12_floppy && request.scope != MediaConversionScope::volume) ||
        (request.scope == MediaConversionScope::volume) != request.volume_directory_id.has_value()) {
        return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::io,
                                          "CD-ROM conversion requires a partition and floppy conversion "
                                          "requires one volume directory")};
    }
    if (source.partition == nullptr || source.partition->index.value != request.partition_index) {
        return std::unexpected{
            make_error(ErrorCode::object_missing, ErrorCategory::object, "source partition does not exist")};
    }
    if (request.scope == MediaConversionScope::volume &&
        std::ranges::find(source.volumes, *request.volume_directory_id, &SourceVolume::directory_id) ==
            source.volumes.end()) {
        return std::unexpected{
            make_error(ErrorCode::object_missing, ErrorCategory::object, "source volume directory does not exist")};
    }

    const auto &partition = *source.partition;

    detail::PreparedMediaConversion prepared;
    prepared.summary.format = request.format;
    prepared.summary.scope = request.scope;
    prepared.summary.partition_index = request.partition_index;
    prepared.summary.partition_name = partition.name;
    prepared.summary.capacity_bytes =
        request.format == MediaImageFormat::fat12_floppy ? floppy_image_bytes : limits.maximum_output_bytes;
    prepared.image.manifest.schema_version = std::string{build_manifest_schema_version};
    prepared.image.manifest.format = request.format;
    prepared.image.manifest.iso_volume_id = sanitized_iso_id(request.iso_volume_id);
    prepared.image.manifest.raw_group = std::string{generated_raw_group};
    prepared.image.manifest.group_name = partition.name;
    prepared.image.limits = limits;
    prepared.summary.output_extension = request.format == MediaImageFormat::iso9660 ? ".iso" : ".ima";
    prepared.summary.floppy_image_count = request.format == MediaImageFormat::fat12_floppy ? 1U : 0U;

    std::set<std::string> selected_keys;
    std::unordered_map<std::string, std::uint32_t> volume_by_key;
    std::map<std::string, std::size_t, std::less<>> floppy_object_index_by_key;
    std::size_t output_volume_index{};
    for (const auto &volume : source.volumes) {
        if (request.scope == MediaConversionScope::volume && volume.directory_id != *request.volume_directory_id)
            continue;
        ++output_volume_index;
        detail::PreparedIsoVolume prepared_volume{std::string{generated_raw_group},
                                                  partition.name,
                                                  std::format("F{:03}", output_volume_index),
                                                  volume.name,
                                                  {}};
        MediaConversionVolumeSummary volume_summary{volume.directory_id, volume.name, prepared_volume.raw_volume};
        std::set<std::uint32_t> seen_ids;
        for (const auto object_id : volume.object_ids) {
            if (!seen_ids.insert(object_id).second) {
                add_issue(prepared.summary, "MEDIA_CONVERSION_OBJECT_PLACEMENT_DUPLICATE",
                          std::format("Volume '{}' contains a duplicate object directory link", volume.name));
                continue;
            }
            const auto object_found = source.objects_by_id.find(object_id);
            const auto record_found = source.records_by_id.find(object_id);
            if (object_found == source.objects_by_id.end() || record_found == source.records_by_id.end()) {
                add_issue(prepared.summary, "MEDIA_CONVERSION_OBJECT_UNREADABLE",
                          std::format("Volume '{}' contains an unreadable object", volume.name));
                continue;
            }
            const auto &object = *object_found->second;
            if (!object.placement || object.placement_resolution != PlacementResolution::exact ||
                object.placement->volume_directory.value != volume.directory_id) {
                add_issue(prepared.summary, "MEDIA_CONVERSION_OBJECT_PLACEMENT_UNPROVEN",
                          std::format("Object '{}' has no exact placement in volume '{}'", object.object.header.name,
                                      volume.name));
                continue;
            }
            if (!supported_object(object.object.header.type)) {
                add_issue(prepared.summary, "MEDIA_CONVERSION_OBJECT_TYPE_UNSUPPORTED",
                          std::format("Object '{}' has an unsupported object type", object.object.header.name));
                continue;
            }
            const auto size = static_cast<std::uint64_t>(record_found->second->data_size);
            if (size > limits.maximum_object_bytes ||
                size > limits.maximum_aggregate_payload_bytes - prepared.summary.payload_bytes) {
                add_issue(prepared.summary, "MEDIA_CONVERSION_PAYLOAD_LIMIT",
                          "Selected object payloads exceed the configured conversion limit");
                continue;
            }
            auto reader =
                std::make_shared<RecordReader>(source.container, partition.index, object.sfs_id, size, cancellation);
            if (request.format == MediaImageFormat::fat12_floppy)
                floppy_object_index_by_key.emplace(object.key, prepared_volume.objects.size());
            prepared_volume.objects.emplace_back(object.object.header.type, object.object.header.name,
                                                 std::move(reader));
            selected_keys.insert(object.key);
            volume_by_key.emplace(object.key, volume.directory_id);
            prepared.summary.payload_bytes += size;
            volume_summary.payload_bytes += size;
            ++prepared.summary.object_count;
            ++volume_summary.object_count;
        }
        prepared.summary.volumes.push_back(std::move(volume_summary));
        if (request.format == MediaImageFormat::iso9660) {
            prepared.image.iso_volumes.push_back(std::move(prepared_volume));
        } else {
            prepared.image.objects = std::move(prepared_volume.objects);
            prepared.image.manifest.raw_volume = prepared_volume.raw_volume;
            prepared.image.manifest.volume_name = prepared_volume.volume_name;
        }
    }

    if (request.scope == MediaConversionScope::partition) {
        for (const auto &[id, object] : source.objects_by_id) {
            static_cast<void>(id);
            if (!selected_keys.contains(object->key)) {
                add_issue(prepared.summary, "MEDIA_CONVERSION_PARTITION_OBJECT_UNPLACED",
                          std::format("Partition object '{}' is not exactly placed in a source volume",
                                      object->object.header.name));
            }
        }
        for (const auto &issue : source.inventory.catalog.issues) {
            if (issue.partition.value == request.partition_index) {
                add_issue(prepared.summary, issue.code,
                          std::format("Partition catalog issue prevents exact conversion: {}", issue.message));
            }
        }
    }

    std::size_t retained_program_row_count{};
    std::vector<std::string> retained_program_row_names;
    for (const auto &relationship : source.graph.relationships) {
        if (!selected_keys.contains(relationship.source_key))
            continue;
        if (retained_unresolved_program_row(relationship, source.objects_by_key)) {
            ++retained_program_row_count;
            if (retained_program_row_names.size() < 5U &&
                !std::ranges::contains(retained_program_row_names, relationship.assignment_name)) {
                retained_program_row_names.push_back(relationship.assignment_name);
            }
            continue;
        }
        if (!dependency_relationship(relationship))
            continue;
        if (relationship.quality != RelationshipQuality::known || !relationship.target_key) {
            add_issue(prepared.summary, "MEDIA_CONVERSION_RELATIONSHIP_UNCONFIRMED",
                      std::format("{} dependency is not Known", relationship.type));
            continue;
        }
        if (!selected_keys.contains(*relationship.target_key)) {
            add_issue(prepared.summary, "MEDIA_CONVERSION_DEPENDENCY_OUTSIDE_SCOPE",
                      std::format("{} dependency is outside the selected source scope", relationship.type));
            continue;
        }
        if (volume_by_key.at(relationship.source_key) != volume_by_key.at(*relationship.target_key)) {
            add_issue(prepared.summary, "MEDIA_CONVERSION_CROSS_VOLUME_DEPENDENCY",
                      std::format("{} crosses source volume boundaries", relationship.type));
            continue;
        }
        if (request.format == MediaImageFormat::fat12_floppy &&
            (relationship.type == "SBNK_LEFT_MEMBER_TO_SMPL" || relationship.type == "SBNK_RIGHT_MEMBER_TO_SMPL")) {
            const auto source_object = floppy_object_index_by_key.find(relationship.source_key);
            const auto target = floppy_object_index_by_key.find(*relationship.target_key);
            if (source_object == floppy_object_index_by_key.end() || target == floppy_object_index_by_key.end()) {
                add_issue(prepared.summary, "MEDIA_CONVERSION_RELATIONSHIP_UNCONFIRMED",
                          std::format("{} dependency has no prepared floppy object", relationship.type));
                continue;
            }
            const detail::PreparedMediaImage::SampleWaveDependency dependency{source_object->second, target->second};
            if (!std::ranges::contains(prepared.image.sample_wave_dependencies, dependency))
                prepared.image.sample_wave_dependencies.push_back(dependency);
        }
    }
    if (retained_program_row_count != 0U) {
        std::string names;
        for (const auto &name : retained_program_row_names) {
            if (!names.empty())
                names += ", ";
            names += name;
        }
        add_issue(prepared.summary, "MEDIA_CONVERSION_RETAINED_DISABLED_PROGRAM_ROWS",
                  std::format("Retained {} disabled Program assignment row{} without inventing target objects{}{}",
                              retained_program_row_count, retained_program_row_count == 1U ? "" : "s",
                              names.empty() ? "" : ": ", names),
                  {}, false);
    }

    if (prepared.summary.volumes.empty()) {
        add_issue(prepared.summary, "MEDIA_CONVERSION_SCOPE_EMPTY", "The selected source scope has no volume");
    }
    if (request.format == MediaImageFormat::iso9660) {
        validate_iso_layout(prepared);
    } else {
        if (prepared.summary.object_count == 0U)
            add_issue(prepared.summary, "MEDIA_CONVERSION_SCOPE_EMPTY", "A floppy image must contain an object");
        if (auto validated = validate_floppy_layout(prepared, cancellation); !validated)
            return std::unexpected{validated.error()};
    }
    prepared.summary.can_export = std::ranges::none_of(prepared.summary.issues, &MediaConversionIssue::blocking);
    return prepared;
}

} // namespace

Result<detail::PreparedMediaConversion>
detail::prepare_media_conversion(std::shared_ptr<const RandomAccessReader> source_reader,
                                 const std::filesystem::path &source_path, const MediaConversionRequest &request,
                                 const MediaBuildLimits &limits, const CancellationToken &cancellation) {
    auto source =
        open_conversion_source(std::move(source_reader), source_path, request.partition_index, limits, cancellation);
    if (!source)
        return std::unexpected{source.error()};
    return prepare_media_conversion_from_source(**source, request, limits, cancellation);
}

Result<detail::PreparedVolumeFloppyExport>
detail::prepare_volume_floppy_export(std::shared_ptr<const RandomAccessReader> source_reader,
                                     const std::filesystem::path &source_path, const VolumeFloppyExportRequest &request,
                                     const MediaBuildLimits &limits, const CancellationToken &cancellation) {
    auto source =
        open_conversion_source(std::move(source_reader), source_path, request.partition_index, limits, cancellation);
    if (!source)
        return std::unexpected{source.error()};

    PreparedVolumeFloppyExport result;
    result.summary.partition_index = request.partition_index;
    result.summary.partition_name = (*source)->partition->name;
    result.volumes.reserve((*source)->volumes.size());
    result.summary.volumes.reserve((*source)->volumes.size());
    for (const auto &volume : (*source)->volumes) {
        if (const auto check = cancellation.check(); !check)
            return std::unexpected{check.error()};
        MediaConversionRequest conversion;
        conversion.format = MediaImageFormat::fat12_floppy;
        conversion.scope = MediaConversionScope::volume;
        conversion.partition_index = request.partition_index;
        conversion.volume_directory_id = volume.directory_id;
        auto prepared = prepare_media_conversion_from_source(**source, conversion, limits, cancellation);
        if (!prepared)
            return std::unexpected{prepared.error()};
        result.summary.volumes.push_back(
            {volume.directory_id, volume.name, prepared->summary.can_export, prepared->summary.object_count,
             prepared->summary.payload_bytes, prepared->summary.floppy_image_count,
             prepared->summary.floppy_image_count * floppy_image_bytes, prepared->summary.issues});
        result.volumes.push_back(std::move(*prepared));
    }
    return result;
}

Result<MediaConversionPlanSummary> plan_media_conversion(std::shared_ptr<const RandomAccessReader> source_reader,
                                                         const std::filesystem::path &source_path,
                                                         const MediaConversionRequest &request,
                                                         const MediaBuildLimits &limits,
                                                         const CancellationToken &cancellation) {
    auto prepared =
        detail::prepare_media_conversion(std::move(source_reader), source_path, request, limits, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    return std::move(prepared->summary);
}

Result<VolumeFloppyExportPlanSummary> plan_volume_floppy_export(std::shared_ptr<const RandomAccessReader> source_reader,
                                                                const std::filesystem::path &source_path,
                                                                const VolumeFloppyExportRequest &request,
                                                                const MediaBuildLimits &limits,
                                                                const CancellationToken &cancellation) {
    auto prepared =
        detail::prepare_volume_floppy_export(std::move(source_reader), source_path, request, limits, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    return std::move(prepared->summary);
}

Result<WrittenMediaImage> write_media_conversion(std::shared_ptr<const RandomAccessReader> source_reader,
                                                 const std::filesystem::path &source_path,
                                                 const MediaConversionRequest &request,
                                                 const std::filesystem::path &output_path, bool overwrite,
                                                 const MediaBuildLimits &limits,
                                                 const CancellationToken &cancellation) {
    auto prepared =
        detail::prepare_media_conversion(std::move(source_reader), source_path, request, limits, cancellation);
    if (!prepared)
        return std::unexpected{prepared.error()};
    if (!prepared->summary.can_export) {
        return std::unexpected{make_error(ErrorCode::unsupported_profile, ErrorCategory::unsupported,
                                          prepared->summary.issues.front().message)};
    }
    if (request.format == MediaImageFormat::fat12_floppy) {
        const auto volume_name =
            prepared->summary.volumes.empty() ? std::string_view{"AXKLIB"} : prepared->summary.volumes.front().name;
        auto plan = detail::plan_floppy_disk_set(prepared->image, volume_name, cancellation);
        if (!plan)
            return std::unexpected{plan.error()};
        if (plan->disks.size() > 1U)
            return detail::write_floppy_disk_set(prepared->image, *plan, output_path, overwrite, cancellation);
    }
    return detail::write_prepared_media_image(prepared->image, output_path, overwrite, cancellation);
}

} // namespace axk
