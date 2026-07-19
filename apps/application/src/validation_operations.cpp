#include "axklib/application/validation_operations.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"

namespace {

using Json = nlohmann::json;

struct ValidationRequest {
    std::vector<axk::app::FileRef> sources;
    std::optional<axk::app::DirectoryRef> exports;
    axk::app::DirectoryRef destination;
    std::string policy{"normal"};
    bool overwrite{};
};

struct ValidationSource {
    axk::app::FileRef reference;
    std::filesystem::path path;
    axk::MediaContainer media;
    std::vector<axk::MediaObjectDescriptor> objects;
    axk::ObjectCatalog catalog;
    axk::RelationshipGraph graph;
};

struct DirectoryCleanup {
    std::filesystem::path path;
    bool active{true};

    ~DirectoryCleanup() {
        if (!active)
            return;
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    void release() noexcept { active = false; }
};

axk::app::Error operation_error(std::string code, std::string message,
                                std::optional<std::string> relative_path = std::nullopt) {
    axk::app::ErrorContext context;
    context.relative_path = std::move(relative_path);
    return {std::move(code), std::move(message), std::move(context)};
}

axk::app::Error core_error(const axk::Error &error, const axk::app::FileRef &source) {
    axk::app::ErrorContext context;
    context.partition_index = error.context.partition_index;
    context.volume_name = error.context.volume_name;
    context.object_type = error.context.object_type;
    context.object_name = error.context.object_name;
    context.relative_path = source.relative_path;
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "validation_failed",
            error.message, std::move(context)};
}

axk::app::Result<void> write_reader(const std::filesystem::path &path, const axk::RandomAccessReader &reader) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return std::unexpected(operation_error("validation_input_failed", "could not stage export directory"));
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(operation_error("validation_input_failed", "could not stage export file"));
    std::vector<std::byte> buffer(
        static_cast<std::size_t>(std::max<std::uint64_t>(1U, std::min<std::uint64_t>(1024U * 1024U, reader.size()))));
    for (std::uint64_t offset = 0U; offset < reader.size();) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), reader.size() - offset));
        if (const auto read = reader.read_exact_at(offset, std::span{buffer}.first(count)); !read)
            return std::unexpected(operation_error("validation_input_failed", read.error().message));
        output.write(reinterpret_cast<const char *>(buffer.data()), static_cast<std::streamsize>(count));
        if (!output)
            return std::unexpected(operation_error("validation_input_failed", "could not stage export file"));
        offset += count;
    }
    return {};
}

axk::app::Result<ValidationRequest> parse_request(const Json &input) {
    ValidationRequest result;
    try {
        if (!input.is_object() || !input.contains("destination") || !input.at("destination").is_object())
            return std::unexpected(operation_error("invalid_request", "validation request requires a destination"));
        if (input.contains("sources")) {
            if (!input.at("sources").is_array() || input.at("sources").size() > 1024U)
                return std::unexpected(
                    operation_error("invalid_request", "sources must contain at most 1024 FileRef values"));
            for (const auto &source : input.at("sources")) {
                result.sources.push_back(
                    {source.at("rootId").get<std::string>(), source.at("relativePath").get<std::string>()});
            }
        }
        if (input.contains("exports") && !input.at("exports").is_null()) {
            const auto &exports = input.at("exports");
            result.exports = axk::app::DirectoryRef{exports.at("rootId").get<std::string>(),
                                                    exports.at("relativePath").get<std::string>()};
        }
        if (result.sources.empty() == !result.exports)
            return std::unexpected(operation_error("invalid_request", "provide either sources or exports"));
        const auto &destination = input.at("destination");
        result.destination = {destination.at("rootId").get<std::string>(),
                              destination.at("relativePath").get<std::string>()};
        result.policy = input.value("policy", std::string{"normal"});
        constexpr std::array policies{"normal", "strict", "salvage-aware"};
        if (std::ranges::find(policies, result.policy) == policies.end())
            return std::unexpected(
                operation_error("invalid_request", "policy must be normal, strict, or salvage-aware"));
        result.overwrite = input.value("overwrite", false);
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "validation request does not match its schema"));
    }
    return result;
}

std::string display_path(const axk::app::FileRef &source, const axk::app::OperationContext &context) {
    if (context.display_path) {
        const auto display = context.display_path(source);
        if (!display.empty())
            return display;
    }
    return source.relative_path;
}

axk::app::Result<ValidationSource> load_source(const axk::app::Sandbox &sandbox, const axk::app::FileRef &source,
                                               const axk::app::OperationContext &context) {
    const auto file = sandbox.open_file(source);
    if (!file)
        return std::unexpected(file.error());
    auto media = axk::open_media(file->reader, std::filesystem::path{file->filename}, context.cancellation);
    if (!media)
        return std::unexpected(core_error(media.error(), source));
    auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::complete, 64U * 1024U * 1024U,
                                                context.cancellation);
    if (!inventory)
        return std::unexpected(core_error(inventory.error(), source));
    auto graph = axk::build_relationship_graph(inventory->catalog);
    const auto report_path = axk::text::path_from_utf8(display_path(source, context));
    return ValidationSource{source,
                            report_path ? *report_path : std::filesystem::path{source.relative_path},
                            std::move(*media),
                            std::move(inventory->objects),
                            std::move(inventory->catalog),
                            std::move(graph)};
}

std::string child_reference_path(const axk::app::DirectoryRef &directory, std::string_view child) {
    return directory.relative_path.empty() ? std::string{child} : std::format("{}/{}", directory.relative_path, child);
}

std::uint64_t mismatch_cluster_count(std::span<const axk::AllocationMismatchRange> ranges) {
    std::uint64_t result{};
    for (const auto &range : ranges)
        result += static_cast<std::uint64_t>(range.end_cluster) - range.start_cluster + 1U;
    return result;
}

std::vector<axk::ReportRow> allocation_summary_rows(const std::filesystem::path &path,
                                                    const axk::Container &container) {
    std::vector<axk::ReportRow> rows;
    for (const auto &partition : container.partitions()) {
        const auto cluster_size =
            static_cast<std::uint64_t>(partition.sectors_per_cluster) * container.superblock().sector_size_bytes;
        std::uint64_t direct_records{};
        std::uint64_t continuation_records{};
        std::uint64_t extent_count{};
        std::uint64_t continuation_clusters{};
        std::uint64_t first_payload = partition.cluster_count;
        std::uint64_t first_object = partition.cluster_count;
        for (const auto &record : partition.records) {
            if (record.continuation_clusters.empty())
                ++direct_records;
            else
                ++continuation_records;
            extent_count += record.extents.size();
            continuation_clusters += record.continuation_clusters.size();
            for (const auto &extent : record.extents)
                first_payload = std::min(first_payload, static_cast<std::uint64_t>(extent.cluster_offset));
            if (record.payload_kind == axk::PayloadKind::object ||
                record.payload_kind == axk::PayloadKind::alternating_byte_object) {
                for (const auto &extent : record.extents)
                    first_object = std::min(first_object, static_cast<std::uint64_t>(extent.cluster_offset));
            }
        }
        std::string warnings;
        const auto &allocation = partition.allocation;
        const auto free = allocation.free_space;
        rows.push_back({
            {"source_image", axk::text::path_to_utf8(path)},
            {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
            {"partition_name", partition.name},
            {"start_sector", static_cast<std::uint64_t>(partition.start_sector)},
            {"sectors_per_cluster", static_cast<std::uint64_t>(partition.sectors_per_cluster)},
            {"cluster_count", static_cast<std::uint64_t>(partition.cluster_count)},
            {"bitmap_offset", (static_cast<std::uint64_t>(partition.start_sector) +
                               static_cast<std::uint64_t>(partition.bitmap_cluster) * partition.sectors_per_cluster) *
                                  container.superblock().sector_size_bytes},
            {"index_offset",
             (static_cast<std::uint64_t>(partition.start_sector) +
              static_cast<std::uint64_t>(partition.directory_index_cluster) * partition.sectors_per_cluster) *
                 container.superblock().sector_size_bytes},
            {"scanned_index_bytes", (first_object - partition.directory_index_cluster) * cluster_size},
            {"valid_index_record_count", static_cast<std::uint64_t>(partition.records.size())},
            {"invalid_extent_record_count", static_cast<std::uint64_t>(allocation.invalid_extent_record_count)},
            {"direct_extent_record_count", direct_records},
            {"continuation_extent_record_count", continuation_records},
            {"data_extent_count", extent_count},
            {"continuation_list_cluster_count", continuation_clusters},
            {"stored_used_cluster_count", static_cast<std::uint64_t>(allocation.stored_used_cluster_count)},
            {"reconstructed_used_cluster_count",
             static_cast<std::uint64_t>(allocation.reconstructed_used_cluster_count)},
            {"first_payload_cluster", first_payload},
            {"reserved_cluster_count", free ? static_cast<std::uint64_t>(free->reserved_cluster_count) : first_payload},
            {"sampler_free_cluster_count",
             free ? static_cast<std::uint64_t>(free->free_cluster_count) : std::uint64_t{0}},
            {"sampler_free_bytes", free ? free->free_bytes : std::uint64_t{0}},
            {"sampler_visible_free_kib", free ? free->sampler_visible_free_kib : std::uint64_t{0}},
            {"stored_used_not_reconstructed_count", mismatch_cluster_count(allocation.stored_not_reconstructed)},
            {"reconstructed_used_not_stored_count", mismatch_cluster_count(allocation.reconstructed_not_stored)},
            {"extent_total_mismatch_count", static_cast<std::uint64_t>(allocation.extent_total_mismatch_count)},
            {"warning_count", std::uint64_t{0}},
            {"warnings", warnings},
        });
    }
    return rows;
}

std::vector<axk::ReportRow> allocation_extent_rows(const std::filesystem::path &path, const axk::Container &container) {
    std::vector<axk::ReportRow> rows;
    for (const auto &partition : container.partitions()) {
        for (const auto &record : partition.records) {
            for (std::size_t index = 0; index < record.extents.size(); ++index) {
                const auto &extent = record.extents[index];
                rows.push_back({{"source_image", axk::text::path_to_utf8(path)},
                                {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
                                {"sfs_id", static_cast<std::uint64_t>(record.sfs_id.value)},
                                {"record_offset", record.record_offset.value},
                                {"extent_kind", "data"},
                                {"extent_index", static_cast<std::uint64_t>(index)},
                                {"cluster_offset", static_cast<std::uint64_t>(extent.cluster_offset)},
                                {"cluster_count", static_cast<std::uint64_t>(extent.cluster_count)},
                                {"byte_count", static_cast<std::uint64_t>(extent.byte_count)},
                                {"continuation_cluster", nullptr}});
            }
        }
    }
    return rows;
}

std::vector<axk::ReportRow> allocation_mismatch_rows(const std::filesystem::path &path,
                                                     std::span<const axk::Partition> partitions) {
    std::vector<axk::ReportRow> rows;
    const auto append = [&](const axk::Partition &partition, std::string direction,
                            std::span<const axk::AllocationMismatchRange> ranges) {
        for (const auto &range : ranges) {
            rows.push_back(
                {{"source_image", axk::text::path_to_utf8(path)},
                 {"partition_index", static_cast<std::uint64_t>(partition.index.value)},
                 {"partition_name", partition.name},
                 {"direction", direction},
                 {"start_cluster", static_cast<std::uint64_t>(range.start_cluster)},
                 {"end_cluster", static_cast<std::uint64_t>(range.end_cluster)},
                 {"cluster_count", static_cast<std::uint64_t>(range.end_cluster) - range.start_cluster + 1U}});
        }
    };
    for (const auto &partition : partitions) {
        append(partition, "stored-used-without-index-extent", partition.allocation.stored_not_reconstructed);
        append(partition, "index-extent-references-free-cluster", partition.allocation.reconstructed_not_stored);
    }
    return rows;
}

std::vector<axk::ReportRow> volume_validation_rows(const std::filesystem::path &path, const axk::Container &container,
                                                   const axk::ObjectCatalog &catalog,
                                                   std::vector<axk::ReportRow> &detail_issues,
                                                   std::vector<axk::ReportRow> &validation_issues) {
    using VolumeKey = std::tuple<std::uint8_t, std::uint32_t, std::string, std::string>;
    std::map<VolumeKey, std::vector<const axk::ObjectSnapshot *>> volume_objects;
    for (const auto &partition : container.partitions()) {
        std::map<std::uint32_t, const axk::IndexRecord *> directories;
        for (const auto &record : partition.records) {
            if (record.directory_id)
                directories.emplace(record.directory_id->value, &record);
        }
        const axk::IndexRecord *root{};
        for (const auto &[id, directory] : directories) {
            if (directory->parent_directory_id && directory->parent_directory_id->value == id) {
                root = directory;
                break;
            }
        }
        if (root == nullptr || !root->directory_id)
            continue;
        for (const auto &entry : root->directory_entries) {
            const auto found = directories.find(entry.link_id.value);
            if (entry.name == "." || entry.name == ".." || found == directories.end())
                continue;
            const auto *volume = found->second;
            if (!volume->parent_directory_id || volume->parent_directory_id->value != root->directory_id->value)
                continue;
            volume_objects.try_emplace(
                VolumeKey{partition.index.value, volume->sfs_id.value, partition.name, entry.name});
        }
    }
    for (const auto &item : catalog.objects) {
        if (item.placement) {
            volume_objects[{item.partition.value, item.placement->volume_directory.value,
                            item.placement->partition_name, item.placement->volume_name}]
                .push_back(&item);
        }
    }
    std::vector<axk::ReportRow> rows;
    for (const auto &[key, objects] : volume_objects) {
        static_cast<void>(objects);
        const auto &[partition_index, directory_id, partition_name, volume_name] = key;
        const auto partition = std::ranges::find(container.partitions(), partition_index,
                                                 [](const auto &item) { return item.index.value; });
        const axk::IndexRecord *volume_record{};
        if (partition != container.partitions().end()) {
            const auto found = std::ranges::find_if(
                partition->records, [&](const auto &record) { return record.sfs_id.value == directory_id; });
            if (found != partition->records.end())
                volume_record = &*found;
        }
        std::uint64_t category_count{};
        std::uint64_t object_entry_count{};
        std::uint64_t matched_object_count{};
        std::uint64_t category_directory_count{};
        std::uint64_t checked_entry_count{};
        std::uint64_t valid_entry_count{};
        std::uint64_t current_object_count{};
        std::map<axk::ObjectType, std::uint64_t> artifact_counts;
        if (partition != container.partitions().end() && volume_record != nullptr) {
            const auto category_type = [](std::string_view name) {
                if (name == "SMPL")
                    return axk::ObjectType::smpl;
                if (name == "SBNK")
                    return axk::ObjectType::sbnk;
                if (name == "SBAC")
                    return axk::ObjectType::sbac;
                if (name == "PROG")
                    return axk::ObjectType::prog;
                if (name == "SEQU")
                    return axk::ObjectType::sequ;
                return axk::ObjectType::unknown;
            };
            for (const auto &category_entry : volume_record->directory_entries) {
                if (category_entry.name == "." || category_entry.name == "..")
                    continue;
                ++category_count;
                const auto type = category_type(category_entry.name);
                if (type == axk::ObjectType::unknown)
                    continue;
                const auto category = std::ranges::find(partition->records, category_entry.link_id.value,
                                                        [](const auto &record) { return record.sfs_id.value; });
                if (category == partition->records.end())
                    continue;
                ++category_directory_count;
                for (const auto &entry : category->directory_entries) {
                    if (entry.name == "." || entry.name == "..")
                        continue;
                    ++object_entry_count;
                    ++checked_entry_count;
                    const auto target = std::ranges::find(partition->records, entry.link_id.value,
                                                          [](const auto &record) { return record.sfs_id.value; });
                    if (target == partition->records.end() ||
                        (target->payload_kind != axk::PayloadKind::object &&
                         target->payload_kind != axk::PayloadKind::alternating_byte_object))
                        continue;
                    ++matched_object_count;
                    ++valid_entry_count;
                    if (target->payload_kind == axk::PayloadKind::alternating_byte_object)
                        ++artifact_counts[type];
                    else
                        ++current_object_count;
                }
            }
        }
        const auto allocation_issues =
            partition == container.partitions().end()
                ? 1U
                : partition->allocation.invalid_extent_record_count + partition->allocation.extent_total_mismatch_count;
        std::uint64_t artifact_count{};
        for (const auto &[type, count] : artifact_counts) {
            static_cast<void>(type);
            artifact_count += count;
        }
        const auto artifact_smpl_count = artifact_counts[axk::ObjectType::smpl];
        const auto warning_count = artifact_count == 0U ? 0U : 1U;
        const auto details =
            std::format("visible alternating-byte compatibility artifact object entries: "
                        "total={}, SMPL={}, "
                        "SBNK={}, SBAC={}, PROG={}; filesystem tree/allocation validation "
                        "does not prove sampler "
                        "loadability for this physical alternating-byte artifact family",
                        artifact_count, artifact_smpl_count, artifact_counts[axk::ObjectType::sbnk],
                        artifact_counts[axk::ObjectType::sbac], artifact_counts[axk::ObjectType::prog]);
        if (artifact_count != 0U) {
            detail_issues.push_back({
                {"source_image", axk::text::path_to_utf8(path)},
                {"partition_index", static_cast<std::uint64_t>(partition_index)},
                {"partition_name", partition_name},
                {"volume_name", volume_name},
                {"volume_path", "/" + volume_name},
                {"severity", "warning"},
                {"issue_type", "visible-alternating-byte-compatibility-artifact-objects"},
                {"category_code", ""},
                {"category_name", ""},
                {"category_directory_id", ""},
                {"category_directory_path", ""},
                {"entry_offset", ""},
                {"entry_name", ""},
                {"link_id", ""},
                {"target_kind", "object"},
                {"target_sfs_id", ""},
                {"target_payload_kind", "alternating-byte-compatibility-object"},
                {"match_quality", "Likely"},
                {"unmatched_reason", ""},
                {"details", details},
            });
            validation_issues.push_back({
                {"severity", "warning"},
                {"code", "SFS_VOLUME_VISIBLE_ALTERNATING_BYTE_ARTIFACT"},
                {"message", details},
                {"scope", "volume"},
                {"source_path", axk::text::path_to_utf8(path)},
                {"sampler_path", "/" + volume_name},
                {"object_key", ""},
                {"quality", "Likely"},
                {"basis", "axklib.validation.volume"},
                {"recommended_next_check", ""},
            });
        }
        const auto validation_status = allocation_issues != 0U ? "Fail" : warning_count != 0U ? "Warn" : "Pass";
        const auto classification = allocation_issues != 0U ? "volume-likely-corrupt"
                                    : warning_count != 0U   ? "valid-visible-tree-with-warnings"
                                                            : "valid-visible-tree-hidden-unreferenced-not-an-error";
        rows.push_back({
            {"source_image", axk::text::path_to_utf8(path)},
            {"partition_index", static_cast<std::uint64_t>(partition_index)},
            {"partition_name", partition_name},
            {"volume_name", volume_name},
            {"volume_path", "/" + volume_name},
            {"directory_id", static_cast<std::uint64_t>(directory_id)},
            {"category_count", category_count},
            {"object_entry_count", object_entry_count},
            {"matched_object_count", matched_object_count},
            {"category_directory_count", category_directory_count},
            {"checked_category_entry_count", checked_entry_count},
            {"valid_category_entry_count", valid_entry_count},
            {"malformed_category_entry_count", std::uint64_t{0}},
            {"category_count_mismatch_count", std::uint64_t{0}},
            {"current_object_entry_count", current_object_count},
            {"compatibility_artifact_object_entry_count", artifact_count},
            {"compatibility_artifact_smpl_entry_count", artifact_smpl_count},
            {"fatal_issue_count", std::uint64_t{0}},
            {"warning_issue_count", static_cast<std::uint64_t>(warning_count)},
            {"allocation_status", allocation_issues == 0U ? "Pass" : "Fail"},
            {"allocation_issue_count", static_cast<std::uint64_t>(allocation_issues)},
            {"validation_status", validation_status},
            {"volume_classification", classification},
            {"quality_summary", warning_count != 0U ? details
                                : allocation_issues == 0U
                                    ? "category directory entries and optional allocation check passed"
                                    : "allocation check failed"},
        });
    }
    return rows;
}

const axk::ObjectSnapshot *catalog_object(const ValidationSource &source, std::string_view key) {
    const auto found = std::ranges::find(source.catalog.objects, key, &axk::ObjectSnapshot::key);
    return found == source.catalog.objects.end() ? nullptr : &*found;
}

const axk::MediaObjectDescriptor *media_object(const ValidationSource &source, std::string_view key) {
    const auto found = std::ranges::find(source.objects, key, &axk::MediaObjectDescriptor::key);
    return found == source.objects.end() ? nullptr : &*found;
}

std::string public_object_key(const ValidationSource &source, std::string_view native_key) {
    if (source.media.kind() == axk::MediaKind::sfs)
        return std::string{native_key};
    const auto *object = media_object(source, native_key);
    if (object == nullptr)
        return std::string{native_key};
    const auto filename = axk::text::path_to_utf8(source.path.filename());
    if (source.media.kind() == axk::MediaKind::fat12_floppy)
        return std::format("{}:{}", filename, object->logical_path);
    if (source.media.kind() == axk::MediaKind::iso9660)
        return std::format("{}:iso9660:{}", filename, object->logical_path);
    return std::format("{}:standalone-object", filename);
}

axk::ReportRow media_validation_issue(const ValidationSource &source, std::string severity, std::string code,
                                      std::string message, std::string scope, std::string sampler_path,
                                      std::string object_key, std::string quality, std::string basis,
                                      std::string recommended_next_check = {}) {
    return {{"severity", std::move(severity)},
            {"code", std::move(code)},
            {"message", std::move(message)},
            {"scope", std::move(scope)},
            {"source_path", axk::text::path_to_utf8(source.path)},
            {"sampler_path", std::move(sampler_path)},
            {"object_key", std::move(object_key)},
            {"quality", std::move(quality)},
            {"basis", std::move(basis)},
            {"recommended_next_check", std::move(recommended_next_check)}};
}

std::string media_object_report_path(const ValidationSource &source, std::string_view object_key) {
    if (const auto *item = catalog_object(source, object_key); item != nullptr && item->placement) {
        const auto &placement = *item->placement;
        const auto category = [&]() -> std::string_view {
            if (placement.category_name == "SMPL")
                return "Wave Data";
            if (placement.category_name == "SBNK")
                return "Samples";
            if (placement.category_name == "SBAC")
                return "Sample Banks";
            if (placement.category_name == "SEQU")
                return "Sequences";
            if (placement.category_name == "PROG")
                return "Programs";
            return placement.category_name;
        }();
        std::string path = std::format("partition {}", placement.partition.value);
        for (const auto &component :
             {std::string_view{placement.volume_name}, category, std::string_view{placement.entry_name}}) {
            if (!component.empty())
                path += std::format("/{}", component);
        }
        return path;
    }
    const auto *object = media_object(source, object_key);
    return object == nullptr ? public_object_key(source, object_key) : object->logical_path;
}

std::string media_object_group_path(const ValidationSource &source, std::string_view object_key) {
    auto path = media_object_report_path(source, object_key);
    std::ranges::replace(path, '\\', '/');
    const auto filename_separator = path.rfind('/');
    if (filename_separator == std::string::npos)
        return path;
    const auto category_separator = path.rfind('/', filename_separator - 1U);
    if (category_separator == std::string::npos)
        return path;
    const auto category = path.substr(category_separator + 1U, filename_separator - category_separator - 1U);
    static constexpr std::array object_categories{"PROG", "SBAC", "SBNK", "SMPL", "SEQU", "PRF3"};
    if (std::ranges::find(object_categories, category) == object_categories.end())
        return path;
    return path.substr(0U, category_separator);
}

std::string active_program_assignment_label(const ValidationSource &source, const axk::Relationship &row) {
    const auto assignment_name =
        !row.assignment_name.empty() ? row.assignment_name : row.target_key.value_or("unnamed assignment");
    if (!row.assignment_index)
        return std::format("{}: {}", media_object_report_path(source, row.source_key), assignment_name);
    return std::format("{}: assignment {} {}", media_object_report_path(source, row.source_key),
                       *row.assignment_index + 1U, assignment_name);
}

std::string relationship_issue_path(const ValidationSource &source, const axk::Relationship &row) {
    if (row.type.starts_with("PROG_ASSIGNMENT_"))
        return active_program_assignment_label(source, row);
    return media_object_report_path(source, row.source_key);
}

std::pair<std::string, std::string> ambiguous_relationship_message(const axk::Relationship &row) {
    if (row.basis == "assignment-visible-off-same-volume-sbac-diagnostic" ||
        row.basis == "assignment-visible-off-same-volume-sbnk-diagnostic") {
        const auto target =
            row.basis == "assignment-visible-off-same-volume-sbac-diagnostic" ? "Sample Bank (SBAC)" : "Sample (SBNK)";
        return {std::format("Visible/off Program assignment row names a {} with one same-volume diagnostic candidate "
                            "plus other duplicate-name candidates; this is decoded Program inventory, not active "
                            "Program content loss.",
                            target),
                "Use relationships.csv candidate fields when auditing off rows; the same-volume candidate is "
                "diagnostic only and must not create an active Program child."};
    }
    if (row.assignment_state == axk::AssignmentState::visible_off)
        return {"Visible/off Program assignment row has multiple possible local targets; this is decoded Program "
                "inventory, not active Program content loss.",
                "Use relationships.csv candidate fields only when auditing off rows; do not treat this warning as a "
                "missing active Program child."};
    if (row.type == "PROG_ASSIGNMENT_TO_SBAC")
        return {"Program assignment to a Sample Bank (SBAC) has multiple possible targets.",
                "Verify the sampler-visible Program assignment and Sample Bank target before promotion."};
    if (row.type == "PROG_ASSIGNMENT_TO_SBNK")
        return {"Direct Program assignment has multiple possible Sample (SBNK) targets.",
                "Verify the sampler-visible Program assignment target before promotion."};
    if (row.type == "SBAC_SLOT_TO_SBNK")
        return {"Sample Bank (SBAC) slot has multiple possible Sample (SBNK) targets.",
                "Inspect duplicate same-name Sample candidates before using this slot as authoritative."};
    if (row.basis == "sbnk-member-link-id-only-iso-cross-folder-name-mismatch")
        return {"Sample (SBNK) link ID points to one Wave Data (SMPL) object in another ISO object folder, but the "
                "Sample name does not confirm it.",
                "Inspect the Wave Data candidate before treating this link as exact."};
    if (row.basis == "sbnk-member-link-id-only-name-mismatch")
        return {"Sample (SBNK) link ID points to one Wave Data (SMPL) object, but the Sample name does not confirm it.",
                "Inspect the Wave Data candidate before treating this link as exact."};
    if (row.type.starts_with("SBNK_") && row.type.ends_with("_TO_SMPL"))
        return {"Sample (SBNK) link has multiple possible Wave Data (SMPL) targets.",
                "Inspect candidate Wave Data objects before treating this Sample link as exact."};
    if (row.basis.starts_with("sbnk-program-link-bitmap-")) {
        std::string message;
        if (row.basis.find("disambiguates-ambiguous-direct-assignment") != std::string::npos)
            message =
                "Sample (SBNK) Program-link bitmap points to one Program from an ambiguous direct-assignment set.";
        else if (row.basis.find("known-direct-assignment-missing-bitmap") != std::string::npos)
            message = "Known direct Program assignment is missing from the Sample (SBNK) Program-link bitmap.";
        else if (row.basis.find("nondefault-flag-direct-assignment-without-bitmap") != std::string::npos)
            message = "Nondefault direct Program assignment is missing from the Sample (SBNK) Program-link bitmap.";
        else
            message = "Sample (SBNK) Program-link bitmap differs from resolved direct Program assignments.";
        return {std::move(message), "Use this as bitmap consistency data only; do not treat it as Program content loss "
                                    "unless another public rule proves the bitmap is authoritative."};
    }
    if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG")
        return {"Sample (SBNK) Program-link bitmap maps to multiple possible Program slots.",
                "Use this as bitmap consistency data only until the Program target is disambiguated."};
    return {"Relationship has ambiguous candidate targets.",
            "Inspect candidate set before using for authoritative placement."};
}

std::string tentative_relationship_code(const axk::Relationship &row) {
    if (row.assignment_state == axk::AssignmentState::visible_off)
        return "REL_VISIBLE_OFF_ASSIGNMENT_DIAGNOSTIC";
    if (row.basis.starts_with("sbnk-program-link-bitmap-"))
        return "REL_PROGRAM_LINK_BITMAP_DIAGNOSTIC";
    if (row.basis.starts_with("sbnk-member-link-id-only"))
        return "REL_SBNK_MEMBER_LINK_DIAGNOSTIC";
    return "REL_AMBIGUOUS_TARGET";
}

std::pair<std::string, std::string> missing_relationship_message(const axk::Relationship &row) {
    if (row.assignment_state == axk::AssignmentState::active)
        return {"Active Program assignment references a missing local target.",
                "Inspect the Program assignment and source object group; user-facing info may show an unresolved "
                "placeholder instead of a normal Program child."};
    if (row.assignment_state == axk::AssignmentState::visible_off) {
        const auto expected = row.type == "PROG_ASSIGNMENT_TO_SBAC" ? "Sample Bank (SBAC)" : "Sample (SBNK)";
        return {std::format("Visible/off Program assignment row names a missing local {} target; this is decoded "
                            "Program inventory, not active Program content loss.",
                            expected),
                "Keep this row as diagnostic/off-row data unless sampler-visible checks prove it should become an "
                "active assignment."};
    }
    if (row.assignment_state == axk::AssignmentState::source_load)
        return {"Source-load Program assignment row has no resolved local target.",
                "Keep the selector as diagnostic source data until sampler-loaded placement or another public rule "
                "proves a target."};
    if (row.type.starts_with("SBNK_") && row.type.ends_with("_TO_SMPL"))
        return {"Sample (SBNK) link does not resolve to a Wave Data (SMPL) target.",
                "Inspect the object group before treating this Sample as complete."};
    return {"Relationship target could not be resolved.",
            "Inspect the relationship row and decoded source object before treating the target as present."};
}

std::string missing_relationship_code(const axk::Relationship &row) {
    if (row.assignment_state == axk::AssignmentState::visible_off)
        return "REL_VISIBLE_OFF_ASSIGNMENT_DIAGNOSTIC";
    if (row.assignment_state == axk::AssignmentState::active)
        return "REL_ACTIVE_ASSIGNMENT_MISSING_TARGET";
    return "REL_MISSING_TARGET";
}

std::vector<axk::ReportRow> validate_media_details(const ValidationSource &source, bool include_object_checks = true) {
    std::vector<axk::ReportRow> issues;
    if (include_object_checks) {
        for (const auto &issue : source.media.validation_issues()) {
            issues.push_back(media_validation_issue(source, "error", issue.code, issue.message, "container",
                                                    issue.sampler_path, {}, "Confirmed", issue.basis,
                                                    issue.recommended_next_check));
        }
        for (const auto &object : source.catalog.objects) {
            const auto *descriptor = media_object(source, object.key);
            const auto required =
                static_cast<std::uint64_t>(object.object.header.header_size) + object.object.header.payload_bytes_0x1c;
            if (descriptor == nullptr || required <= descriptor->size)
                continue;
            issues.push_back(media_validation_issue(
                source, "error", "OBJECT_PAYLOAD_TRUNCATED",
                std::format("Object header requires {} bytes but payload has {} bytes.", required, descriptor->size),
                "object", {}, public_object_key(source, object.key), "Known", "validation"));
        }
    }

    std::map<std::string, std::vector<std::string>> group_members;
    for (const auto &row : source.graph.relationships) {
        if (row.type == "SBAC_SLOT_TO_SBNK" && row.target_key &&
            (row.quality == axk::RelationshipQuality::known || row.quality == axk::RelationshipQuality::likely)) {
            group_members[row.source_key].push_back(*row.target_key);
        }
    }
    std::map<std::string, std::vector<const axk::Relationship *>> reachable;
    for (const auto &row : source.graph.relationships) {
        if (!row.target_key ||
            (row.assignment_state != axk::AssignmentState::active &&
             row.assignment_state != axk::AssignmentState::source_load) ||
            (row.quality != axk::RelationshipQuality::known && row.quality != axk::RelationshipQuality::likely)) {
            continue;
        }
        if (row.type == "PROG_ASSIGNMENT_TO_SBNK") {
            reachable[*row.target_key].push_back(&row);
        } else if (row.type == "PROG_ASSIGNMENT_TO_SBAC") {
            if (const auto members = group_members.find(*row.target_key); members != group_members.end()) {
                for (const auto &member : members->second)
                    reachable[member].push_back(&row);
            }
        }
    }
    using MemberGroup = std::pair<std::string, bool>;
    std::map<MemberGroup, std::vector<const axk::Relationship *>> grouped_members;
    std::map<MemberGroup, std::set<std::string>> grouped_active_labels;
    std::set<std::string> covered_relationships;
    for (const auto &row : source.graph.relationships) {
        if ((row.type != "SBNK_LEFT_MEMBER_TO_SMPL" && row.type != "SBNK_RIGHT_MEMBER_TO_SMPL") ||
            row.quality != axk::RelationshipQuality::unknown)
            continue;
        const auto active = reachable.find(row.source_key);
        const MemberGroup group{media_object_group_path(source, row.source_key), active != reachable.end()};
        grouped_members[group].push_back(&row);
        if (active != reachable.end()) {
            for (const auto *program_row : active->second)
                grouped_active_labels[group].insert(active_program_assignment_label(source, *program_row));
        }
        covered_relationships.insert(row.key);
    }
    for (const auto &[group, rows] : grouped_members) {
        std::set<std::string> source_keys;
        for (const auto *row : rows)
            source_keys.insert(public_object_key(source, row->source_key));
        const auto member_count = rows.size();
        const auto bank_count = source_keys.size();
        if (group.second) {
            std::string active_summary;
            const auto &labels = grouped_active_labels[group];
            std::size_t index{};
            for (const auto &label : labels) {
                if (index == 4U)
                    break;
                if (!active_summary.empty())
                    active_summary += "; ";
                active_summary += label;
                ++index;
            }
            if (labels.size() > 4U)
                active_summary += std::format("; +{} more", labels.size() - 4U);
            issues.push_back(media_validation_issue(
                source, "error", "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING",
                std::format("{} Sample-to-Wave-Data link(s) across {} Sample(s) do not resolve to Wave Data objects "
                            "and are reachable from active Program assignments.",
                            member_count, bank_count),
                "relationship", std::format("{} | {}", group.first, active_summary), *source_keys.begin(), "Unknown",
                "SBNK member target aggregation",
                "Treat the affected Program/Sample path as incomplete until the missing Wave Data objects are found "
                "or the source is confirmed partially loadable."));
        } else {
            issues.push_back(media_validation_issue(
                source, "warning", "REL_SBNK_MEMBER_TARGET_MISSING",
                std::format("{} Sample-to-Wave-Data link(s) across {} Sample(s) do not resolve to Wave Data objects.",
                            member_count, bank_count),
                "relationship", group.first, *source_keys.begin(), "Unknown", "SBNK member target aggregation",
                "Inspect the Sample-to-Wave-Data links before treating this object set as complete."));
        }
    }
    for (const auto &row : source.graph.relationships) {
        if (covered_relationships.contains(row.key))
            continue;
        if (row.quality == axk::RelationshipQuality::tentative) {
            auto [message, next_check] = ambiguous_relationship_message(row);
            issues.push_back(media_validation_issue(
                source, "warning", tentative_relationship_code(row), std::move(message), "relationship",
                relationship_issue_path(source, row), public_object_key(source, row.source_key), "Tentative", row.basis,
                std::move(next_check)));
        } else if (row.quality == axk::RelationshipQuality::unknown) {
            auto [message, next_check] = missing_relationship_message(row);
            issues.push_back(media_validation_issue(
                source, "warning", missing_relationship_code(row), std::move(message), "relationship",
                relationship_issue_path(source, row), public_object_key(source, row.source_key), "Unknown", row.basis,
                std::move(next_check)));
        }
    }
    std::ranges::sort(issues, {}, [](const axk::ReportRow &row) {
        const auto value = [&](std::string_view key) -> std::string {
            const auto found = std::ranges::find(row, key, &axk::ReportRow::value_type::first);
            return found == row.end() ? std::string{} : std::get<std::string>(found->second.value);
        };
        return std::tuple{value("code"), value("object_key"), value("message")};
    });
    return issues;
}

axk::ReportRow export_validation_issue(std::string severity, std::string code, std::string message, std::string scope,
                                       const std::filesystem::path &source, std::string object_key = {}) {
    return {{"severity", std::move(severity)},
            {"code", std::move(code)},
            {"message", std::move(message)},
            {"scope", std::move(scope)},
            {"source_path", axk::text::path_to_utf8(source)},
            {"sampler_path", ""},
            {"object_key", std::move(object_key)},
            {"quality", "Known"},
            {"basis", "validation"},
            {"recommended_next_check", ""}};
}

std::optional<std::uint32_t> little_u32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset + 4U > bytes.size())
        return std::nullopt;
    return std::to_integer<std::uint8_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 2U])) << 16U) |
           (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + 3U])) << 24U);
}

std::optional<std::uint16_t> little_u16(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset + 2U > bytes.size())
        return std::nullopt;
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1U])) << 8U));
}

std::vector<axk::ReportRow> validate_export_directory(const std::filesystem::path &root) {
    std::vector<axk::ReportRow> issues;
    if (!std::filesystem::exists(root)) {
        issues.push_back(export_validation_issue("error", "EXPORT_DIR_NOT_FOUND", "Export directory does not exist.",
                                                 "export", root));
        return issues;
    }
    std::error_code iteration_error;
    for (std::filesystem::recursive_directory_iterator iterator{root, iteration_error}, end;
         iterator != end && !iteration_error; iterator.increment(iteration_error)) {
        if (!iterator->is_regular_file() || iterator->path().extension() != ".json" ||
            iterator->path().filename() == "schema_index.json" ||
            std::ranges::find(iterator->path(), "_schemas") != iterator->path().end())
            continue;
        Json record;
        try {
            std::ifstream input{iterator->path()};
            input >> record;
        } catch (const std::exception &error) {
            issues.push_back(export_validation_issue("error", "EXPORT_SIDECAR_BAD_JSON",
                                                     std::string{"Sidecar JSON could not be parsed: "} + error.what(),
                                                     "sidecar", iterator->path()));
            continue;
        }
        if (!record.is_object())
            continue;
        const auto schema = record.value("schema", std::string{});
        if (schema == "axklib.volume_graph.v1") {
            const auto inspect_path = [&](const Json &value, std::string_view object_key) {
                if (!value.is_string())
                    return;
                const std::filesystem::path path{value.get<std::string>()};
                const auto resolved = (iterator->path().parent_path() / path).lexically_normal();
                const auto relative_to_root = resolved.lexically_relative(root.lexically_normal());
                const auto escapes_root = relative_to_root.empty() || relative_to_root.is_absolute() ||
                                          std::ranges::find(relative_to_root, "..") != relative_to_root.end();
                if (path.is_absolute() || escapes_root) {
                    issues.push_back(export_validation_issue("error", "EXPORT_VOLUME_GRAPH_PATH_ESCAPE",
                                                             "Volume graph WAV path must be relative and stay inside "
                                                             "the export root.",
                                                             "sidecar", iterator->path(), std::string{object_key}));
                }
            };
            if (record.contains("objects") && record["objects"].is_object() && record["objects"].contains("smpl") &&
                record["objects"]["smpl"].is_array()) {
                for (const auto &sample : record["objects"]["smpl"])
                    inspect_path(sample.value("wav_path", Json{}), sample.value("object_key", std::string{}));
            }
            continue;
        }
        auto object_key = record.value("object_key", std::string{});
        Json header = record;
        std::filesystem::path wav_path;
        if (schema == "axklib.wave_sidecar.v2") {
            static constexpr std::array sections{"identity",   "audio",      "playback", "relationships",
                                                 "parameters", "conversion", "origin"};
            std::vector<std::string> missing;
            for (const auto section : sections) {
                if (!record.contains(section))
                    missing.emplace_back(section);
            }
            if (record.contains("identity") && record["identity"].is_object())
                object_key = record["identity"].value("object_key", object_key);
            if (!missing.empty()) {
                std::string section_names;
                for (const auto &section : missing) {
                    if (!section_names.empty())
                        section_names += ", ";
                    section_names += section;
                }
                issues.push_back(export_validation_issue("error", "EXPORT_SIDECAR_MISSING_FIELD",
                                                         "Sidecar missing required sections: " + section_names,
                                                         "sidecar", iterator->path(), object_key));
            }
            if (!record.contains("audio") || !record["audio"].is_object())
                continue;
            header = record["audio"];
            wav_path = header.value("wav_path", std::string{});
            if (wav_path.is_absolute() || std::ranges::find(wav_path, "..") != wav_path.end()) {
                issues.push_back(export_validation_issue("error", "EXPORT_SIDECAR_PATH_ESCAPE",
                                                         "v2 sidecar audio.wav_path must be relative and stay inside "
                                                         "the export root.",
                                                         "sidecar", iterator->path(), object_key));
                continue;
            }
            wav_path = root / wav_path;
        } else {
            if (!record.contains("wav_path"))
                continue;
            static constexpr std::array required{
                "source_container",   "object_key",         "wav_path",     "sample_rate",
                "channels",           "sample_width_bytes", "frames",       "stored_payload_size",
                "extraction_quality", "extraction_basis",   "field_quality"};
            std::vector<std::string> missing;
            for (const auto field : required) {
                if (!record.contains(field))
                    missing.emplace_back(field);
            }
            std::ranges::sort(missing);
            if (!missing.empty()) {
                std::string fields;
                for (const auto &field : missing) {
                    if (!fields.empty())
                        fields += ", ";
                    fields += field;
                }
                issues.push_back(export_validation_issue("error", "EXPORT_SIDECAR_MISSING_FIELD",
                                                         "Sidecar missing required fields: " + fields, "sidecar",
                                                         iterator->path(), object_key));
            }
            wav_path = record.value("wav_path", std::string{});
            if (!wav_path.is_absolute()) {
                if (!std::filesystem::exists(wav_path))
                    wav_path = iterator->path().parent_path() / wav_path;
            }
        }
        if (!std::filesystem::exists(wav_path)) {
            issues.push_back(export_validation_issue(
                "error", "EXPORT_WAV_MISSING", "Referenced WAV does not exist: " + axk::text::path_to_utf8(wav_path),
                "export", iterator->path(), object_key));
            continue;
        }
        std::ifstream wav{wav_path, std::ios::binary};
        std::array<std::byte, 44> bytes{};
        wav.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (wav.gcount() != static_cast<std::streamsize>(bytes.size()) ||
            std::string_view{reinterpret_cast<const char *>(bytes.data()), 4U} != "RIFF" ||
            std::string_view{reinterpret_cast<const char *>(bytes.data() + 8U), 4U} != "WAVE") {
            issues.push_back(export_validation_issue("error", "EXPORT_WAV_BAD_HEADER",
                                                     "Referenced WAV could not be opened: invalid WAVE header",
                                                     "export", wav_path, object_key));
            continue;
        }
        const std::array observed{
            std::pair{"sample_rate", static_cast<std::uint64_t>(*little_u32(bytes, 24U))},
            std::pair{"channels", static_cast<std::uint64_t>(*little_u16(bytes, 22U))},
            std::pair{"sample_width_bytes", static_cast<std::uint64_t>(*little_u16(bytes, 34U) / 8U)},
            std::pair{"frames", static_cast<std::uint64_t>(*little_u32(bytes, 40U) /
                                                           (*little_u16(bytes, 22U) * (*little_u16(bytes, 34U) / 8U)))},
        };
        for (const auto &[name, value] : observed) {
            if (header.contains(name) && header[name].is_number_integer() &&
                header[name].get<std::uint64_t>() != value) {
                issues.push_back(export_validation_issue(
                    "error", "EXPORT_WAV_HEADER_MISMATCH",
                    std::format("{} sidecar={} wav={}", name, header[name].get<std::uint64_t>(), value), "export",
                    wav_path, object_key));
            }
        }
    }
    return issues;
}

axk::app::Result<axk::ReportSchemaManifest> write_report_set(const std::filesystem::path &destination,
                                                             const axk::app::DirectoryRef &destination_ref,
                                                             std::string name, std::span<const axk::ReportRow> rows,
                                                             bool overwrite) {
    const auto csv_name = name + ".csv";
    if (auto written = axk::write_report_csv(destination / csv_name, rows, {}, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, csv_name)}));
    }
    const auto json_name = name + ".json";
    if (auto written = axk::write_report_json(destination / json_name, rows, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, json_name)}));
    }
    axk::ReportSchemaOptions options;
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    if (name == "validation_issues")
        options.semantic_notes = "Validation issues use stable issue codes intended for regression and CI gates.";
    auto schema = axk::make_report_schema(name, rows, std::move(options));
    const auto schema_name = std::format("_schemas/{}.schema.json", name);
    if (auto written = axk::write_report_schema(destination / schema_name, schema, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, schema_name)}));
    }
    return schema;
}

axk::app::Result<Json> execute_validation(const axk::app::Sandbox &sandbox, const Json &input,
                                          const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.create_staging_directory("axklib-validation");
    if (!destination)
        return std::unexpected(destination.error());
    DirectoryCleanup cleanup{*destination};
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }

    std::vector<axk::ReportRow> issues;
    std::vector<axk::ReportRow> allocation_summaries;
    std::vector<axk::ReportRow> allocation_extents;
    std::vector<axk::ReportRow> allocation_mismatches;
    std::vector<axk::ReportRow> volumes;
    std::vector<axk::ReportRow> volume_issues;
    std::map<std::string, std::uint64_t> issue_counts;
    bool failed{};
    bool has_sfs_input{};
    std::vector<ValidationSource> loaded;

    if (request->exports) {
        constexpr std::size_t maximum_export_files = 100'000U;
        constexpr std::uint64_t maximum_export_bytes = 16ULL * 1024ULL * 1024ULL * 1024ULL;
        auto export_files = sandbox.open_tree_files(*request->exports, maximum_export_files, maximum_export_bytes);
        if (!export_files)
            return std::unexpected(export_files.error());
        auto export_staging = sandbox.create_staging_directory("axklib-validation-input");
        if (!export_staging)
            return std::unexpected(export_staging.error());
        DirectoryCleanup export_cleanup{*export_staging};
        for (const auto &file : *export_files) {
            if (auto staged = write_reader(*export_staging / file.relative_path, *file.reader); !staged)
                return std::unexpected(staged.error());
        }
        issues = validate_export_directory(*export_staging);
        for (const auto &issue : issues) {
            const auto code = std::ranges::find(issue, "code", &std::pair<std::string, axk::ReportValue>::first);
            if (code != issue.end())
                ++issue_counts[std::get<std::string>(code->second.value)];
        }
        failed = !issues.empty();
    } else {
        loaded.reserve(request->sources.size());
        for (std::size_t index = 0; index < request->sources.size(); ++index) {
            if (context.progress != nullptr) {
                context.progress->report({axk::ProgressPhase::reading, index, request->sources.size(),
                                          request->sources[index].relative_path, std::nullopt});
            }
            auto source = load_source(sandbox, request->sources[index], context);
            if (!source) {
                return std::unexpected(operation_error("validation_input_failed",
                                                       "one or more validation inputs could not be opened",
                                                       request->sources[index].relative_path));
            }
            loaded.push_back(std::move(*source));
        }
    }

    for (const auto &source : loaded) {
        if (source.media.kind() != axk::MediaKind::sfs) {
            auto source_issues = validate_media_details(source);
            for (auto &issue : source_issues) {
                const auto code = std::ranges::find(issue, "code", &std::pair<std::string, axk::ReportValue>::first);
                if (code != issue.end())
                    ++issue_counts[std::get<std::string>(code->second.value)];
                const auto severity =
                    std::ranges::find(issue, "severity", &std::pair<std::string, axk::ReportValue>::first);
                if (severity != issue.end()) {
                    const auto &value = std::get<std::string>(severity->second.value);
                    if (value == "error" || value == "fatal" || (request->policy == "strict" && value == "warning"))
                        failed = true;
                }
                issues.push_back(std::move(issue));
            }
            continue;
        }
        has_sfs_input = true;
        const auto &container = std::get<axk::Container>(source.media.storage());
        const auto validation = axk::validate_semantics(container, source.catalog, source.graph);
        for (const auto &issue : validation.issues) {
            if (issue.code.starts_with("REL_"))
                continue;
            const auto severity = issue.severity == axk::ValidationSeverity::error     ? "error"
                                  : issue.severity == axk::ValidationSeverity::warning ? "warning"
                                                                                       : "info";
            ++issue_counts[issue.code];
            if (issue.severity == axk::ValidationSeverity::error ||
                (request->policy == "strict" && issue.severity == axk::ValidationSeverity::warning))
                failed = true;
            issues.push_back({{"severity", severity},
                              {"code", issue.code},
                              {"message", issue.message},
                              {"scope", "relationship"},
                              {"source_path", axk::text::path_to_utf8(source.path)},
                              {"sampler_path", issue.sampler_path},
                              {"object_key", issue.object_key},
                              {"quality", "Known"},
                              {"basis", "validation"},
                              {"recommended_next_check", ""}});
        }
        auto relationship_issues = validate_media_details(source, false);
        for (auto &issue : relationship_issues) {
            const auto code = std::ranges::find(issue, "code", &std::pair<std::string, axk::ReportValue>::first);
            if (code != issue.end())
                ++issue_counts[std::get<std::string>(code->second.value)];
            const auto severity =
                std::ranges::find(issue, "severity", &std::pair<std::string, axk::ReportValue>::first);
            if (severity != issue.end()) {
                const auto &value = std::get<std::string>(severity->second.value);
                if (value == "error" || value == "fatal" || (request->policy == "strict" && value == "warning"))
                    failed = true;
            }
            issues.push_back(std::move(issue));
        }
        auto source_summaries = allocation_summary_rows(source.path, container);
        std::ranges::move(source_summaries, std::back_inserter(allocation_summaries));
        auto source_extents = allocation_extent_rows(source.path, container);
        std::ranges::move(source_extents, std::back_inserter(allocation_extents));
        auto source_mismatches = allocation_mismatch_rows(source.path, container.partitions());
        std::ranges::move(source_mismatches, std::back_inserter(allocation_mismatches));
        const auto prior_issue_count = issues.size();
        auto source_volumes = volume_validation_rows(source.path, container, source.catalog, volume_issues, issues);
        for (auto index = prior_issue_count; index < issues.size(); ++index) {
            const auto code =
                std::ranges::find(issues[index], "code", &std::pair<std::string, axk::ReportValue>::first);
            if (code != issues[index].end())
                ++issue_counts[std::get<std::string>(code->second.value)];
            if (request->policy == "strict")
                failed = true;
        }
        std::ranges::move(source_volumes, std::back_inserter(volumes));
    }

    axk::ReportValue::Object summary_counts;
    for (const auto &[name, count] : issue_counts)
        summary_counts.emplace_back(name, count);
    axk::ReportRow validation_summary{{"policy", request->policy},
                                      {"failed", failed},
                                      {"issue_count", static_cast<std::uint64_t>(issues.size())},
                                      {"summary_counts", std::move(summary_counts)}};
    std::uint64_t pass_count{};
    std::uint64_t warn_count{};
    std::uint64_t fail_count{};
    std::uint64_t fatal_issue_count{};
    std::uint64_t warning_issue_count{};
    std::uint64_t malformed_category_entry_count{};
    std::uint64_t allocation_issue_count{};
    for (const auto &row : volumes) {
        const auto text = [&](std::string_view key) -> std::string {
            const auto found = std::ranges::find(row, key, &std::pair<std::string, axk::ReportValue>::first);
            return found == row.end() ? std::string{} : std::get<std::string>(found->second.value);
        };
        const auto number = [&](std::string_view key) -> std::uint64_t {
            const auto found = std::ranges::find(row, key, &std::pair<std::string, axk::ReportValue>::first);
            return found == row.end() ? 0U : std::get<std::uint64_t>(found->second.value);
        };
        if (text("validation_status") == "Pass")
            ++pass_count;
        else if (text("validation_status") == "Warn")
            ++warn_count;
        else
            ++fail_count;
        fatal_issue_count += number("fatal_issue_count");
        warning_issue_count += number("warning_issue_count");
        malformed_category_entry_count += number("malformed_category_entry_count");
        allocation_issue_count += number("allocation_issue_count");
    }
    axk::ReportRow volume_summary{
        {"source_image", loaded.size() == 1U ? axk::text::path_to_utf8(loaded.front().path) : ""},
        {"volume_count", static_cast<std::uint64_t>(volumes.size())},
        {"pass_count", pass_count},
        {"warn_count", warn_count},
        {"fail_count", fail_count},
        {"fatal_issue_count", fatal_issue_count},
        {"warning_issue_count", warning_issue_count},
        {"malformed_category_entry_count", malformed_category_entry_count},
        {"allocation_issue_count", allocation_issue_count},
    };

    std::vector<axk::ReportSchemaManifest> schemas;
    auto validation_issue_schema =
        write_report_set(*destination, request->destination, "validation_issues", issues, request->overwrite);
    if (!validation_issue_schema)
        return std::unexpected(validation_issue_schema.error());
    schemas.push_back(std::move(*validation_issue_schema));
    if (auto written =
            axk::write_report_object(*destination / "validation_summary.json", validation_summary, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "validation_summary.json")}));
    }
    axk::ReportSchemaOptions schema_options;
    schema_options.source_command = "axklib";
    schema_options.library_version = std::string{axk::version()};
    auto validation_summary_schema =
        axk::make_report_schema("validation_summary", std::span{&validation_summary, 1U}, schema_options);
    if (auto written = axk::write_report_schema(*destination / "_schemas" / "validation_summary.schema.json",
                                                validation_summary_schema, request->overwrite);
        !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id,
                              child_reference_path(request->destination, "_schemas/validation_summary.schema.json")}));
    }
    schemas.push_back(validation_summary_schema);
    if (!request->exports && has_sfs_input) {
        for (const auto &[name, rows] :
             std::initializer_list<std::pair<std::string_view, const std::vector<axk::ReportRow> &>>{
                 {"allocation_summary", allocation_summaries},
                 {"allocation_extents", allocation_extents},
                 {"allocation_mismatches", allocation_mismatches},
                 {"volume_validation", volumes},
                 {"volume_validation_issues", volume_issues}}) {
            auto schema =
                write_report_set(*destination, request->destination, std::string{name}, rows, request->overwrite);
            if (!schema)
                return std::unexpected(schema.error());
            schemas.push_back(std::move(*schema));
        }
        const std::vector<axk::ReportRow> volume_summaries{volume_summary};
        auto volume_schema = write_report_set(*destination, request->destination, "volume_validation_summary",
                                              volume_summaries, request->overwrite);
        if (!volume_schema)
            return std::unexpected(volume_schema.error());
        schemas.push_back(std::move(*volume_schema));
    }
    if (auto written = axk::write_report_schema_index(*destination / "_schemas" / "schema_index.json", schemas,
                                                      request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/schema_index.json")}));
    }

    auto artifacts = Json::array();
    for (const auto &schema : schemas) {
        if (schema.report_name == "validation_summary") {
            artifacts.push_back(
                {{"rootId", request->destination.root_id},
                 {"relativePath", child_reference_path(request->destination, "validation_summary.json")}});
        } else {
            artifacts.push_back(
                {{"rootId", request->destination.root_id},
                 {"relativePath", child_reference_path(request->destination, schema.report_name + ".csv")}});
            artifacts.push_back(
                {{"rootId", request->destination.root_id},
                 {"relativePath", child_reference_path(request->destination, schema.report_name + ".json")}});
        }
        artifacts.push_back(
            {{"rootId", request->destination.root_id},
             {"relativePath",
              child_reference_path(request->destination, std::format("_schemas/{}.schema.json", schema.report_name))}});
    }
    artifacts.push_back({{"rootId", request->destination.root_id},
                         {"relativePath", child_reference_path(request->destination, "_schemas/schema_index.json")}});
    if (context.progress != nullptr) {
        context.progress->report({axk::ProgressPhase::writing, request->sources.size(), request->sources.size(),
                                  "validation", std::nullopt});
    }
    if (auto published = sandbox.publish_directory(request->destination, request->overwrite, *destination); !published)
        return std::unexpected(published.error());
    cleanup.release();
    return Json{{"operationId", "report.validate"}, {"sourceCount", request->sources.size()},
                {"issueCount", issues.size()},      {"failed", failed},
                {"policy", request->policy},        {"artifacts", std::move(artifacts)}};
}

} // namespace

axk::app::Result<void> axk::app::bind_validation_operations(OperationRegistry &registry, const Sandbox &sandbox) {
    if (registry.is_implemented("report.validate"))
        return {};
    return registry.bind("report.validate",
                         [&sandbox](const nlohmann::json &request, const OperationContext &context) -> Result<Json> {
                             return execute_validation(sandbox, request, context);
                         });
}
