#include "axklib/application/file_operations.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/terminology.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"

#include "file_operations_internal.hpp"

namespace axk::app::file_operations_internal {
namespace {

std::string object_format_name(axk::ObjectFormat format) {
    switch (format) {
    case axk::ObjectFormat::current:
        return "current";
    case axk::ObjectFormat::unknown:
        return "unknown";
    }
    return "unknown";
}

bool is_decode_issue(const axk::CatalogIssue &issue) {
    return issue.code == "CATALOG_OBJECT_DECODE_FAILED" || issue.code == "media_object_decode_failed";
}

} // namespace

axk::ReportRow inventory_row(const LoadedSource &source, const axk::ObjectSnapshot &item, std::string display_path) {
    const auto media_object = std::ranges::find(source.inventory.objects, item.key, &axk::MediaObjectDescriptor::key);
    const auto iso = source.media.kind() == axk::MediaKind::iso9660;
    const auto fat = source.media.kind() == axk::MediaKind::fat12_floppy;
    const auto sfs = source.media.kind() == axk::MediaKind::sfs;
    std::string decoded_kind{"UnknownObject"};
    std::string decoded_fields;
    if (item.object.format == axk::ObjectFormat::unknown) {
        decoded_kind = item.object.header.type == axk::ObjectType::sequ ? "OpaqueSequence" : "OpaqueObject";
    } else if (item.object.header.type == axk::ObjectType::smpl) {
        decoded_kind = "DecodedWaveData";
        decoded_fields = "fine_tune;loop_length;loop_mode;loop_start;root_key;sample_rate";
    } else if (item.object.header.type == axk::ObjectType::sbnk) {
        decoded_kind = "DecodedSample";
        decoded_fields = "sample_topology;left_wave_data_name;left_cached_wave_data_reference_value";
    } else if (item.object.header.type == axk::ObjectType::sbac) {
        decoded_kind = "DecodedSampleBank";
        decoded_fields = "active_slot_count;max_slot_count_from_payload";
    } else if (item.object.header.type == axk::ObjectType::prog) {
        decoded_kind = "DecodedProgram";
        decoded_fields = "control_record_count";
    } else if (item.object.header.type == axk::ObjectType::sequ) {
        decoded_kind = "DecodedSequence";
    }
    const auto field_count =
        decoded_fields.empty() ? 0U : static_cast<unsigned int>(std::ranges::count(decoded_fields, ';') + 1);
    std::vector<std::string> decode_issue_codes;
    for (const auto &issue : source.inventory.catalog.issues) {
        if (issue.partition == item.partition && issue.sfs_id == item.sfs_id && is_decode_issue(issue))
            decode_issue_codes.push_back(issue.code);
    }
    std::string serialized_decode_issue_codes;
    for (const auto &code : decode_issue_codes) {
        if (!serialized_decode_issue_codes.empty())
            serialized_decode_issue_codes.push_back(';');
        serialized_decode_issue_codes += code;
    }
    std::uint64_t payload_offset{};
    if (sfs) {
        const auto &container = std::get<axk::Container>(source.media.storage());
        const auto partition = std::ranges::find(container.partitions(), item.partition.value,
                                                 [](const auto &row) { return row.index.value; });
        if (partition != container.partitions().end()) {
            const auto record = std::ranges::find(partition->records, item.sfs_id.value,
                                                  [](const auto &row) { return row.sfs_id.value; });
            if (record != partition->records.end() && !record->extents.empty()) {
                payload_offset = (static_cast<std::uint64_t>(partition->start_sector) +
                                  static_cast<std::uint64_t>(record->extents.front().cluster_offset) *
                                      partition->sectors_per_cluster) *
                                 container.superblock().sector_size_bytes;
            }
        }
    }
    const auto decoded_payload_size =
        static_cast<std::uint64_t>(item.object.header.header_size) + item.object.header.payload_bytes_0x1c;
    const auto payload_size = media_object != source.inventory.objects.end() && media_object->size != 0U
                                  ? media_object->size
                                  : decoded_payload_size;
    const axk::FatFile *fat_metadata{};
    if (fat && media_object != source.inventory.objects.end()) {
        const auto *image = std::get_if<axk::FatImage>(&source.media.storage());
        if (image != nullptr) {
            const auto found = std::ranges::find(image->files(), media_object->logical_path, &axk::FatFile::path);
            if (found != image->files().end())
                fat_metadata = &*found;
        }
    }
    return {
        {"source_path", display_path},
        {"container_kind", info_media_kind_name(source.media.kind())},
        {"detected_format", info_media_kind_name(source.media.kind())},
        {"scope_key", public_scope_key(source, item, display_path)},
        {"object_key", public_object_key(source, item.key)},
        {"partition_index",
         sfs ? axk::ReportValue{static_cast<std::uint64_t>(item.partition.value)} : axk::ReportValue{""}},
        {"sfs_id", sfs ? axk::ReportValue{static_cast<std::uint64_t>(item.sfs_id.value)} : axk::ReportValue{""}},
        {"fat_file", !sfs && media_object != source.inventory.objects.end()
                         ? axk::ReportValue{media_object->logical_path}
                         : axk::ReportValue{""}},
        {"payload_offset", sfs ? axk::ReportValue{payload_offset}
                           : media_object != source.inventory.objects.end()
                               ? axk::ReportValue{media_object->data_offset}
                               : axk::ReportValue{""}},
        {"payload_size", payload_size},
        {"object_type", object_type_name(item.object.header.type)},
        {"object_name", item.object.header.name},
        {"object_format", object_format_name(item.object.format)},
        {"decoded_kind", decoded_kind},
        {"decoded_field_count", static_cast<std::uint64_t>(field_count)},
        {"decoded_fields", decoded_fields},
        {"decode_issue_count", static_cast<std::uint64_t>(decode_issue_codes.size())},
        {"decode_issue_codes", std::move(serialized_decode_issue_codes)},
        {"iso_extent_sector", iso && media_object != source.inventory.objects.end()
                                  ? axk::ReportValue{media_object->data_offset / 2048U}
                                  : axk::ReportValue{""}},
        {"iso_data_offset", iso && media_object != source.inventory.objects.end()
                                ? axk::ReportValue{media_object->data_offset}
                                : axk::ReportValue{""}},
        {"iso_file_size", iso && media_object != source.inventory.objects.end() ? axk::ReportValue{media_object->size}
                                                                                : axk::ReportValue{""}},
        {"iso_recovery_quality", iso ? axk::ReportValue{"clean-iso9660-object"} : axk::ReportValue{""}},
        {"iso_raw_group", iso && media_object != source.inventory.objects.end()
                              ? axk::ReportValue{media_object->raw_group}
                              : axk::ReportValue{""}},
        {"iso_raw_volume", iso && media_object != source.inventory.objects.end()
                               ? axk::ReportValue{media_object->raw_volume}
                               : axk::ReportValue{""}},
        {"iso_group_label", iso && media_object != source.inventory.objects.end()
                                ? axk::ReportValue{media_object->group_label.value}
                                : axk::ReportValue{""}},
        {"iso_volume_label", iso && media_object != source.inventory.objects.end()
                                 ? axk::ReportValue{media_object->volume_label.value}
                                 : axk::ReportValue{""}},
        {"iso_group_label_source", iso && media_object != source.inventory.objects.end() &&
                                           media_object->group_label.status == axk::LabelStatus::confirmed
                                       ? axk::ReportValue{"yamaha-cdrom-menu-label"}
                                       : axk::ReportValue{""}},
        {"iso_volume_label_source", iso && media_object != source.inventory.objects.end() &&
                                            media_object->volume_label.status == axk::LabelStatus::confirmed
                                        ? axk::ReportValue{"yamaha-cdrom-menu-label"}
                                        : axk::ReportValue{""}},
        {"fat_directory_offset",
         fat_metadata != nullptr ? axk::ReportValue{fat_metadata->directory_offset} : axk::ReportValue{""}},
        {"fat_first_cluster", fat_metadata != nullptr
                                  ? axk::ReportValue{static_cast<std::uint64_t>(fat_metadata->first_cluster)}
                                  : axk::ReportValue{""}},
        {"fat_cluster_count", fat_metadata != nullptr
                                  ? axk::ReportValue{static_cast<std::uint64_t>(fat_metadata->clusters.size())}
                                  : axk::ReportValue{""}},
        {"fat_file_size", fat && media_object != source.inventory.objects.end() ? axk::ReportValue{media_object->size}
                                                                                : axk::ReportValue{""}},
        {"fat_object_offset", fat && media_object != source.inventory.objects.end()
                                  ? axk::ReportValue{media_object->data_offset}
                                  : axk::ReportValue{""}},
        {"fat_stored_payload_offset", fat && media_object != source.inventory.objects.end()
                                          ? axk::ReportValue{media_object->data_offset + item.object.header.header_size}
                                          : axk::ReportValue{""}}};
}

std::string sfs_selector_component(const axk::ContentNode &node) {
    if (node.node_type != "partition") {
        auto result = node.display_name;
        std::ranges::replace(result, '/', '_');
        std::ranges::replace(result, '\\', '_');
        return result;
    }

    const auto separator = node.node_id.find(':');
    const auto raw_index = separator == std::string::npos ? std::string{} : node.node_id.substr(separator + 1U);
    auto partition_name = node.display_name;
    const auto prefix = std::format("partition {}: ", raw_index);
    if (partition_name.starts_with(prefix))
        partition_name.erase(0U, prefix.size());
    std::string safe;
    bool prior_space{};
    for (const auto value : partition_name) {
        const auto byte = static_cast<unsigned char>(value);
        if (std::isspace(byte) != 0) {
            if (!safe.empty() && !prior_space)
                safe.push_back('_');
            prior_space = true;
        } else {
            const bool retained = std::isalnum(byte) != 0 || value == '.' || value == '_' || value == '-';
            safe.push_back(retained ? value : '_');
            prior_space = false;
        }
    }
    while (!safe.empty() && (safe.front() == '.' || safe.front() == '_' || safe.front() == '-'))
        safe.erase(safe.begin());
    while (!safe.empty() && (safe.back() == '.' || safe.back() == '_' || safe.back() == '-'))
        safe.pop_back();
    return std::format("partition_{:0>2}_{}", raw_index,
                       safe.empty() ? std::format("partition_{:0>2}", raw_index) : safe);
}

std::string first_media_object_directory(const LoadedSource &source, const axk::ContentNode &node) {
    if (!node.object_key.empty()) {
        const auto object =
            std::ranges::find(source.inventory.objects, node.object_key, &axk::MediaObjectDescriptor::key);
        if (object != source.inventory.objects.end()) {
            auto directory = std::filesystem::path{object->logical_path}.parent_path();
            if (source.media.kind() == axk::MediaKind::iso9660)
                directory = directory.parent_path();
            return axk::text::path_to_utf8(directory);
        }
    }
    for (const auto &child : node.children) {
        auto directory = first_media_object_directory(source, child);
        if (!directory.empty())
            return directory;
    }
    return {};
}

std::string selector_component(const LoadedSource &source, const axk::ContentNode &node) {
    if (node.node_id == axk::sample_structure_category_id)
        return std::string{axk::sample_structure_selector_component};
    if (node.node_id == axk::wave_data_category_id)
        return std::string{axk::wave_data_selector_component};
    if (source.media.kind() == axk::MediaKind::sfs)
        return sfs_selector_component(node);
    if (node.node_type == "partition" || node.node_type == "volume") {
        auto name = node.display_name;
        constexpr std::string_view error_suffix{" (errors detected)"};
        if (node.node_type == "volume" && name.ends_with(error_suffix))
            name.resize(name.size() - error_suffix.size());
        return axk::sanitize_path_component(name, node.node_type);
    }
    auto component = node.display_name;
    std::ranges::replace(component, '/', '_');
    std::ranges::replace(component, '\\', '_');
    const auto first = component.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return node.node_type;
    const auto last = component.find_last_not_of(" \t\r\n");
    return component.substr(first, last - first + 1U);
}

Json info_node_json(const LoadedSource &source, const axk::ContentNode &node, std::string parent_path = {},
                    std::string parent_id = {}, std::string parent_type = {}) {
    const auto component = selector_component(source, node);
    const auto selector = parent_path.empty() ? component : std::format("{}/{}", parent_path, component);
    const auto object_key = node.object_key.empty() ? std::string{} : public_object_key(source, node.object_key);
    auto node_id = node.node_id;
    if (!node.object_key.empty()) {
        node_id = std::format("object:{}", object_key);
    } else if ((source.media.kind() == axk::MediaKind::fat12_floppy ||
                source.media.kind() == axk::MediaKind::standalone_object) &&
               node.node_type == "volume") {
        node_id = std::format("scope:{}", node.display_name);
    } else if (source.media.kind() == axk::MediaKind::iso9660 && node.node_type == "partition") {
        node_id = "partition:None";
    } else if (source.media.kind() == axk::MediaKind::iso9660 && node.node_type == "volume") {
        node_id = std::format("volume:None:{}", first_media_object_directory(source, node));
    } else if (source.media.kind() == axk::MediaKind::iso9660 && node.node_type == "category" &&
               parent_type == "volume") {
        const auto volume_prefix = parent_id.find(':');
        const auto volume_tail =
            volume_prefix == std::string::npos ? std::string{} : parent_id.substr(volume_prefix + 1U);
        node_id = std::format("category:{}:{}", volume_tail, node.display_name);
    } else if (source.media.kind() == axk::MediaKind::sfs && node.node_type == "volume") {
        const auto separator = parent_id.find(':');
        const auto partition_index = separator == std::string::npos ? std::string{} : parent_id.substr(separator + 1U);
        node_id = std::format("volume:{}:{}", partition_index, node.display_name);
    } else if (source.media.kind() == axk::MediaKind::sfs && node.node_type == "category" && parent_type == "volume") {
        const auto volume_prefix = parent_id.find(':');
        const auto volume_tail =
            volume_prefix == std::string::npos ? std::string{} : parent_id.substr(volume_prefix + 1U);
        node_id = std::format("category:{}:{}", volume_tail, node.display_name);
    }

    auto children = Json::array();
    for (const auto &child : node.children)
        children.push_back(info_node_json(source, child, selector, node_id, node.node_type));
    const bool counted = node.node_type == "partition" || node.node_type == "volume" || node.node_type == "category";
    auto notes = node.notes;
    if (parent_type == "sample_bank" && node.object_type == "SBNK" && node.quality == axk::RelationshipQuality::known) {
        notes = "Input consistency: counted SBAC slot name uniquely matches a same-scope SBNK header name. The "
                "companion 32-bit slot word is preserved as raw/opaque.";
    }
    return {{"nodeId", std::move(node_id)},
            {"nodeType", node.node_type},
            {"displayName", node.display_name},
            {"objectKey", object_key},
            {"objectType", node.object_type},
            {"count", counted ? Json(node.children.size()) : Json(nullptr)},
            {"details", node.details},
            {"quality", std::string{axk::relationship_quality_name(node.quality)}},
            {"basis", node.basis},
            {"notes", std::move(notes)},
            {"selectorPath", selector},
            {"children", std::move(children)}};
}

Json info_tree_json(const LoadedSource &source, std::string display_path) {
    auto roots = Json::array();
    for (const auto &root : source.tree.roots)
        roots.push_back(info_node_json(source, root));
    auto issues = Json::array();
    for (const auto &issue : source.tree.issues) {
        issues.push_back(
            {{"code", issue.code},
             {"severity", issue.severity},
             {"message", issue.message},
             {"sourcePath", display_path},
             {"samplerPath", issue.sampler_path},
             {"objectKey", issue.object_key.empty() ? std::string{} : public_object_key(source, issue.object_key)}});
    }
    std::map<std::string, std::size_t> counts;
    for (const auto &object : source.inventory.catalog.objects)
        ++counts[object_type_name(object.object.header.type)];
    Json object_counts = Json::object();
    for (const auto &[type, count] : counts)
        object_counts[type] = count;
    const auto object_count = source.inventory.catalog.objects.size();
    return {{"sourcePath", std::move(display_path)},
            {"containerKind", info_media_kind_name(source.media.kind())},
            {"detectedFormat", info_media_kind_name(source.media.kind())},
            {"objectCount", object_count},
            {"objectCounts", std::move(object_counts)},
            {"recovery", source.media.kind() == axk::MediaKind::iso9660
                             ? Json(std::format("clean-iso9660-object:{}", object_count))
                             : Json(nullptr)},
            {"roots", std::move(roots)},
            {"issues", std::move(issues)}};
}

axk::app::Result<Json> execute_objects(const axk::app::Sandbox &sandbox, const Json &input,
                                       const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    if (request->object_type) {
        constexpr std::array admitted{"SMPL", "SBNK", "SBAC", "PROG", "SEQU", "PRF3"};
        if (std::ranges::find(admitted, *request->object_type) == admitted.end())
            return std::unexpected(operation_error("invalid_request", "objectType is not supported"));
    }
    const auto destination = sandbox.create_staging_directory("axklib-report-objects");
    if (!destination)
        return std::unexpected(destination.error());
    DirectoryCleanup cleanup{*destination};
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }

    std::vector<axk::ReportRow> rows;
    std::size_t failed_count{};
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_source(sandbox, source_ref, request->include_default_programs, context);
        if (!source) {
            if (request->strict)
                return std::unexpected(source.error());
            ++failed_count;
            continue;
        }
        const auto display_path = source_display_path(source_ref, context);
        for (const auto &item : source->inventory.catalog.objects) {
            if (request->object_type && object_type_name(item.object.header.type) != *request->object_type)
                continue;
            rows.push_back(inventory_row(*source, item, display_path));
        }
    }

    if (auto written = axk::write_report_csv(*destination / "objects.csv", rows, {}, request->overwrite); !written) {
        return std::unexpected(
            core_error(written.error(),
                       {request->destination.root_id, child_reference_path(request->destination, "objects.csv")}));
    }
    if (auto written = axk::write_report_json(*destination / "objects.json", rows, request->overwrite); !written) {
        return std::unexpected(
            core_error(written.error(),
                       {request->destination.root_id, child_reference_path(request->destination, "objects.json")}));
    }
    axk::ReportSchemaOptions options;
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    options.semantic_notes = "Filtered object summary rows produced through the canonical inventory view.";
    const auto schema = axk::make_report_schema("objects", rows, std::move(options));
    if (auto written =
            axk::write_report_schema(*destination / "_schemas" / "objects.schema.json", schema, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/objects.schema.json")}));
    }
    const std::array schemas{schema};
    if (auto written = axk::write_report_schema_index(*destination / "_schemas" / "schema_index.json", schemas,
                                                      request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/schema_index.json")}));
    }
    auto artifacts = Json::array();
    for (const auto path :
         {"objects.csv", "objects.json", "_schemas/objects.schema.json", "_schemas/schema_index.json"}) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    if (context.progress != nullptr) {
        context.progress->report(
            {axk::ProgressPhase::writing, request->sources.size(), request->sources.size(), "objects", std::nullopt});
    }
    if (auto published = sandbox.publish_directory(request->destination, request->overwrite, *destination); !published)
        return std::unexpected(published.error());
    return Json{{"operationId", "report.objects"},
                {"sourceCount", request->sources.size()},
                {"loadedCount", request->sources.size() - failed_count},
                {"failedCount", failed_count},
                {"rowCount", rows.size()},
                {"artifacts", std::move(artifacts)}};
}

axk::app::Result<Json> execute_info(const axk::app::Sandbox &sandbox, const Json &input,
                                    const axk::app::OperationContext &context) {
    const auto request = parse_info_request(input);
    if (!request)
        return std::unexpected(request.error());

    auto trees = Json::array();
    auto errors = Json::array();
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_info_source(sandbox, source_ref, request->include_default_programs, context);
        if (!source) {
            if (request->strict)
                return std::unexpected(source.error().error);
            errors.push_back({{"path", source_display_path(source_ref, context)},
                              {"errorCode", source.error().error_code},
                              {"message", source.error().error.message},
                              {"originalException", source.error().original_exception}});
            continue;
        }
        trees.push_back(info_tree_json(*source, source_display_path(source_ref, context)));
    }
    if (context.progress != nullptr) {
        context.progress->report(
            {axk::ProgressPhase::reading, request->sources.size(), request->sources.size(), "info", std::nullopt});
    }
    return Json{{"operationId", "report.info"}, {"sourceCount", request->sources.size()},
                {"loadedCount", trees.size()},  {"failedCount", errors.size()},
                {"trees", std::move(trees)},    {"loadErrors", std::move(errors)}};
}

} // namespace axk::app::file_operations_internal
