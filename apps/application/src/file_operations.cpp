#include "axklib/application/file_operations.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/report.hpp"
#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"

namespace {

using Json = nlohmann::json;

struct ReportRequest {
    std::vector<axk::app::FileRef> sources;
    axk::app::DirectoryRef destination;
    bool overwrite{};
    bool strict{};
    bool include_default_programs{};
    std::optional<std::string> object_type;
};

struct InfoRequest {
    std::vector<axk::app::FileRef> sources;
    bool strict{};
    bool include_default_programs{};
};

struct CorpusAuditRequest {
    std::vector<axk::app::FileRef> sources;
    axk::app::DirectoryRef destination;
    std::string policy{"normal"};
    std::size_t wave_smoke_limit{10U};
    bool skip_wave_smoke{};
    bool overwrite{};
};

struct InfoLoadFailure {
    axk::app::Error error;
    std::uint64_t error_code{static_cast<std::uint64_t>(axk::ErrorCode::io_open_failed)};
    std::string original_exception{"axk::Error"};
};

struct LoadedSource {
    axk::app::FileRef source;
    axk::MediaContainer media;
    axk::MediaInventory inventory;
    axk::RelationshipGraph graph;
    axk::ContentTree tree;
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
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "report_source_failed",
            error.message, std::move(context)};
}

axk::app::Result<ReportRequest> parse_request(const Json &input) {
    ReportRequest result;
    try {
        if (!input.is_object() || !input.contains("sources") || !input.at("sources").is_array() ||
            input.at("sources").empty() || input.at("sources").size() > 1024U) {
            return std::unexpected(operation_error("invalid_request", "sources must contain 1 to 1024 FileRef values"));
        }
        for (const auto &source : input.at("sources")) {
            result.sources.push_back(
                {source.at("rootId").get<std::string>(), source.at("relativePath").get<std::string>()});
        }
        const auto &destination = input.at("destination");
        result.destination = {destination.at("rootId").get<std::string>(),
                              destination.at("relativePath").get<std::string>()};
        result.overwrite = input.value("overwrite", false);
        result.strict = input.value("strict", false);
        result.include_default_programs = input.value("includeDefaultPrograms", false);
        if (input.contains("objectType") && !input.at("objectType").is_null())
            result.object_type = input.at("objectType").get<std::string>();
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "report request does not match its schema"));
    }
    return result;
}

axk::app::Result<InfoRequest> parse_info_request(const Json &input) {
    InfoRequest result;
    try {
        if (!input.is_object() || !input.contains("sources") || !input.at("sources").is_array() ||
            input.at("sources").empty() || input.at("sources").size() > 1024U) {
            return std::unexpected(operation_error("invalid_request", "sources must contain 1 to 1024 FileRef values"));
        }
        for (const auto &source : input.at("sources")) {
            result.sources.push_back(
                {source.at("rootId").get<std::string>(), source.at("relativePath").get<std::string>()});
        }
        result.strict = input.value("strict", false);
        result.include_default_programs = input.value("includeDefaultPrograms", false);
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "info request does not match its schema"));
    }
    return result;
}

axk::app::Result<CorpusAuditRequest> parse_corpus_audit_request(const Json &input) {
    CorpusAuditRequest result;
    try {
        if (!input.is_object() || !input.contains("sources") || !input.at("sources").is_array() ||
            input.at("sources").empty() || input.at("sources").size() > 1024U) {
            return std::unexpected(operation_error("invalid_request", "sources must contain 1 to 1024 FileRef values"));
        }
        for (const auto &source : input.at("sources")) {
            result.sources.push_back(
                {source.at("rootId").get<std::string>(), source.at("relativePath").get<std::string>()});
        }
        const auto &destination = input.at("destination");
        result.destination = {destination.at("rootId").get<std::string>(),
                              destination.at("relativePath").get<std::string>()};
        result.policy = input.value("policy", std::string{"normal"});
        constexpr std::array policies{"normal", "strict", "salvage-aware"};
        if (std::ranges::find(policies, result.policy) == policies.end())
            return std::unexpected(
                operation_error("invalid_request", "policy must be normal, strict, or salvage-aware"));
        result.wave_smoke_limit = input.value("waveSmokeLimit", 10U);
        if (result.wave_smoke_limit > 1'000'000U)
            return std::unexpected(operation_error("invalid_request", "waveSmokeLimit is too large"));
        result.skip_wave_smoke = input.value("skipWaveSmoke", false);
        result.overwrite = input.value("overwrite", false);
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "corpus audit request does not match its schema"));
    }
    return result;
}

std::string info_media_kind_name(axk::MediaKind kind) {
    switch (kind) {
    case axk::MediaKind::sfs:
        return "sfs";
    case axk::MediaKind::fat12_floppy:
        return "fat12_floppy";
    case axk::MediaKind::iso9660:
        return "iso";
    case axk::MediaKind::standalone_object:
        return "standalone_object";
    }
    return "unknown";
}

std::string object_type_name(axk::ObjectType type) {
    switch (type) {
    case axk::ObjectType::smpl:
        return "SMPL";
    case axk::ObjectType::sbnk:
        return "SBNK";
    case axk::ObjectType::sbac:
        return "SBAC";
    case axk::ObjectType::prog:
        return "PROG";
    case axk::ObjectType::sequ:
        return "SEQU";
    case axk::ObjectType::prf3:
        return "PRF3";
    case axk::ObjectType::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

axk::app::Result<LoadedSource>
load_source(const axk::app::Sandbox &sandbox, const axk::app::FileRef &source, bool include_default_programs,
            const axk::app::OperationContext &context,
            axk::MediaObjectReadMode read_mode = axk::MediaObjectReadMode::decoded_metadata) {
    const auto path = sandbox.resolve_file(source);
    if (!path)
        return std::unexpected(path.error());
    auto media = axk::open_media(*path, context.cancellation);
    if (!media)
        return std::unexpected(core_error(media.error(), source));
    auto inventory = axk::build_media_inventory(*media, read_mode, 64U * 1024U * 1024U, context.cancellation);
    if (!inventory)
        return std::unexpected(core_error(inventory.error(), source));
    auto graph = axk::build_relationship_graph(inventory->catalog);
    auto tree = axk::build_content_tree(*media, inventory->catalog, graph, include_default_programs);
    return LoadedSource{source, std::move(*media), std::move(*inventory), std::move(graph), std::move(tree)};
}

std::expected<LoadedSource, InfoLoadFailure> load_info_source(const axk::app::Sandbox &sandbox,
                                                              const axk::app::FileRef &source,
                                                              bool include_default_programs,
                                                              const axk::app::OperationContext &context) {
    const auto path = sandbox.resolve_file(source);
    if (!path) {
        return std::unexpected(InfoLoadFailure{.error = path.error(),
                                               .error_code = static_cast<std::uint64_t>(axk::ErrorCode::io_open_failed),
                                               .original_exception = "axk::Error"});
    }
    auto media = axk::open_media(*path, context.cancellation);
    if (!media) {
        return std::unexpected(InfoLoadFailure{.error = core_error(media.error(), source),
                                               .error_code = static_cast<std::uint64_t>(media.error().code),
                                               .original_exception = "axk::Error"});
    }
    auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata, 64U * 1024U * 1024U,
                                                context.cancellation);
    if (!inventory) {
        return std::unexpected(InfoLoadFailure{.error = core_error(inventory.error(), source),
                                               .error_code = static_cast<std::uint64_t>(inventory.error().code),
                                               .original_exception = "axk::Error"});
    }
    auto graph = axk::build_relationship_graph(inventory->catalog);
    auto tree = axk::build_content_tree(*media, inventory->catalog, graph, include_default_programs);
    return LoadedSource{source, std::move(*media), std::move(*inventory), std::move(graph), std::move(tree)};
}

std::string source_display_path(const axk::app::FileRef &source, const axk::app::OperationContext &context) {
    if (context.display_path) {
        const auto display = context.display_path(source);
        if (!display.empty())
            return display;
    }
    return source.relative_path;
}

std::string source_filename(const LoadedSource &source) {
    const auto path = axk::text::path_from_utf8(source.source.relative_path);
    return path ? axk::text::path_to_utf8(path->filename()) : source.source.relative_path;
}

std::string public_object_key(const LoadedSource &source, std::string_view native_key) {
    if (source.media.kind() == axk::MediaKind::sfs)
        return std::string{native_key};
    const auto object = std::ranges::find(source.inventory.objects, native_key, &axk::MediaObjectDescriptor::key);
    if (object == source.inventory.objects.end())
        return std::string{native_key};
    const auto filename = source_filename(source);
    if (source.media.kind() == axk::MediaKind::fat12_floppy)
        return std::format("{}:{}", filename, object->logical_path);
    if (source.media.kind() == axk::MediaKind::iso9660)
        return std::format("{}:iso9660:{}", filename, object->logical_path);
    return std::format("{}:standalone-object", filename);
}

std::string public_scope_key(const LoadedSource &source, const axk::ObjectSnapshot &item,
                             std::string_view display_path) {
    if (source.media.kind() == axk::MediaKind::sfs)
        return std::format("{}:partition:{}", display_path, item.partition.value);
    if (source.media.kind() == axk::MediaKind::fat12_floppy)
        return std::format("{}:fat-root", display_path);
    if (source.media.kind() == axk::MediaKind::standalone_object)
        return std::format("{}:standalone-object", display_path);
    const auto object = std::ranges::find(source.inventory.objects, item.key, &axk::MediaObjectDescriptor::key);
    return object == source.inventory.objects.end() ? std::format("{}:iso", display_path)
                                                    : std::format("{}:{}", display_path, object->scope_key);
}

axk::ReportRow inventory_row(const LoadedSource &source, const axk::ObjectSnapshot &item, std::string display_path) {
    const auto media_object = std::ranges::find(source.inventory.objects, item.key, &axk::MediaObjectDescriptor::key);
    const auto iso = source.media.kind() == axk::MediaKind::iso9660;
    const auto fat = source.media.kind() == axk::MediaKind::fat12_floppy;
    const auto sfs = source.media.kind() == axk::MediaKind::sfs;
    std::string decoded_kind{"UnknownObject"};
    std::string decoded_fields;
    if (item.object.header.type == axk::ObjectType::smpl) {
        decoded_kind = "DecodedSample";
        decoded_fields = "fine_tune;loop_length;loop_mode;loop_start;root_key;sample_rate";
    } else if (item.object.header.type == axk::ObjectType::sbnk) {
        decoded_kind = "DecodedSampleBank";
        decoded_fields = "bank_topology;left_sample_name;left_smpl_link_id";
    } else if (item.object.header.type == axk::ObjectType::sbac) {
        decoded_kind = "DecodedSampleBankAccessory";
        decoded_fields = "active_slot_count;max_slot_count_from_payload";
    } else if (item.object.header.type == axk::ObjectType::prog) {
        decoded_kind = "DecodedProgram";
        decoded_fields = "control_record_count";
    } else if (item.object.header.type == axk::ObjectType::sequ) {
        decoded_kind = "DecodedSequence";
    }
    const auto field_count =
        decoded_fields.empty() ? 0U : static_cast<unsigned int>(std::ranges::count(decoded_fields, ';') + 1);
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
        {"object_format", "normal-fsfsdev3splx"},
        {"decoded_kind", decoded_kind},
        {"decoded_field_count", static_cast<std::uint64_t>(field_count)},
        {"decoded_fields", decoded_fields},
        {"decode_issue_count", std::uint64_t{0}},
        {"decode_issue_codes", ""},
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

std::string child_reference_path(const axk::app::DirectoryRef &directory, std::string_view child) {
    return directory.relative_path.empty() ? std::string{child} : std::format("{}/{}", directory.relative_path, child);
}

axk::app::Result<axk::ReportSchemaManifest> write_report_set(const std::filesystem::path &destination,
                                                             const axk::app::DirectoryRef &destination_ref,
                                                             std::string name, std::span<const axk::ReportRow> rows,
                                                             std::string semantic_notes, bool overwrite) {
    const auto csv_name = name + ".csv";
    if (auto written = axk::write_report_csv(destination / csv_name, rows, {}, overwrite); !written)
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, csv_name)}));
    const auto json_name = name + ".json";
    if (auto written = axk::write_report_json(destination / json_name, rows, overwrite); !written)
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, json_name)}));
    axk::ReportSchemaOptions options;
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    options.semantic_notes = std::move(semantic_notes);
    auto schema = axk::make_report_schema(name, rows, std::move(options));
    const auto schema_name = std::format("_schemas/{}.schema.json", name);
    if (auto written = axk::write_report_schema(destination / schema_name, schema, overwrite); !written)
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, schema_name)}));
    return schema;
}

axk::app::Result<axk::ReportSchemaManifest> write_csv_schema(const std::filesystem::path &destination,
                                                             const axk::app::DirectoryRef &destination_ref,
                                                             std::string name, std::span<const axk::ReportRow> rows,
                                                             bool overwrite) {
    const auto csv_name = name + ".csv";
    if (auto written = axk::write_report_csv(destination / csv_name, rows, {}, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, csv_name)}));
    }
    axk::ReportSchemaOptions options;
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    auto schema = axk::make_report_schema(name, rows, options);
    const auto schema_name = std::format("_schemas/{}.schema.json", name);
    if (auto written = axk::write_report_schema(destination / schema_name, schema, overwrite); !written) {
        return std::unexpected(
            core_error(written.error(), {destination_ref.root_id, child_reference_path(destination_ref, schema_name)}));
    }
    return schema;
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
    const auto destination = sandbox.resolve_output_directory(request->destination, request->overwrite);
    if (!destination)
        return std::unexpected(destination.error());
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

axk::app::Result<Json> execute_inventory(const axk::app::Sandbox &sandbox, const Json &input,
                                         const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.resolve_output_directory(request->destination, request->overwrite);
    if (!destination)
        return std::unexpected(destination.error());
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }

    std::vector<axk::ReportRow> objects;
    std::vector<axk::ReportRow> issues;
    std::vector<axk::ReportRow> load_errors;
    std::map<std::string, std::uint64_t> counts;
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
            load_errors.push_back({{"path", source_display_path(source_ref, context)},
                                   {"error_code", source.error().error_code},
                                   {"message", source.error().error.message},
                                   {"recoverable", true},
                                   {"original_exception", source.error().original_exception}});
            continue;
        }
        const auto display_path = source_display_path(source_ref, context);
        for (const auto &item : source->inventory.catalog.objects) {
            objects.push_back(inventory_row(*source, item, display_path));
            ++counts[object_type_name(item.object.header.type)];
        }
        for (const auto &issue : source->inventory.catalog.issues) {
            issues.push_back(
                {{"source_path", display_path},
                 {"container_kind", info_media_kind_name(source->media.kind())},
                 {"object_key",
                  issue.sfs_id ? std::format("p{}:sfs{}", issue.partition.value, issue.sfs_id->value) : std::string{}},
                 {"object_type", ""},
                 {"object_name", ""},
                 {"code", issue.code},
                 {"severity", "error"},
                 {"message", issue.message},
                 {"byte_start", nullptr},
                 {"byte_end", nullptr},
                 {"quality", "Unknown"},
                 {"basis", "native catalog decode"}});
        }
    }

    auto object_schema =
        write_report_set(*destination, request->destination, "inventory_objects", objects,
                         "Decoded object inventory rows produced through axklib.objects.decoded.", request->overwrite);
    if (!object_schema)
        return std::unexpected(object_schema.error());
    auto issue_schema = write_report_set(*destination, request->destination, "decode_issues", issues,
                                         "Decode issues use stable code/severity/quality fields.", request->overwrite);
    if (!issue_schema)
        return std::unexpected(issue_schema.error());

    axk::ReportValue::Object type_counts;
    for (const auto &[name, count] : counts)
        type_counts.emplace_back(name, count);
    axk::ReportValue::Array serialized_load_errors;
    for (const auto &row : load_errors)
        serialized_load_errors.emplace_back(axk::ReportValue::Object{row.begin(), row.end()});
    axk::ReportRow summary{{"input_count", static_cast<std::uint64_t>(request->sources.size())},
                           {"object_count", static_cast<std::uint64_t>(objects.size())},
                           {"decode_issue_count", static_cast<std::uint64_t>(issues.size())},
                           {"load_error_count", static_cast<std::uint64_t>(load_errors.size())},
                           {"object_type_counts", std::move(type_counts)},
                           {"load_errors", std::move(serialized_load_errors)}};
    const auto summary_name = std::string{"inventory_summary.json"};
    if (auto written = axk::write_report_object(*destination / summary_name, summary, request->overwrite); !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id, child_reference_path(request->destination, summary_name)}));
    }
    axk::ReportSchemaOptions summary_options;
    summary_options.source_command = "axklib";
    summary_options.library_version = std::string{axk::version()};
    const auto summary_schema = axk::make_report_schema("inventory_summary", std::span{&summary, 1U}, summary_options);
    const auto summary_schema_name = std::string{"_schemas/inventory_summary.schema.json"};
    if (auto written = axk::write_report_schema(*destination / summary_schema_name, summary_schema, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, summary_schema_name)}));
    }
    const std::array schemas{*object_schema, *issue_schema, summary_schema};
    const auto index_name = std::string{"_schemas/schema_index.json"};
    if (auto written = axk::write_report_schema_index(*destination / index_name, schemas, request->overwrite);
        !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id, child_reference_path(request->destination, index_name)}));
    }

    auto artifacts = Json::array();
    for (const auto path :
         {"inventory_objects.csv", "inventory_objects.json", "decode_issues.csv", "decode_issues.json",
          "inventory_summary.json", "_schemas/inventory_objects.schema.json", "_schemas/decode_issues.schema.json",
          "_schemas/inventory_summary.schema.json", "_schemas/schema_index.json"}) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    if (context.progress != nullptr) {
        context.progress->report(
            {axk::ProgressPhase::writing, request->sources.size(), request->sources.size(), "inventory", std::nullopt});
    }
    return Json{{"operationId", "report.inventory"},
                {"sourceCount", request->sources.size()},
                {"loadedCount", request->sources.size() - load_errors.size()},
                {"failedCount", load_errors.size()},
                {"rowCount", objects.size()},
                {"decodeIssueCount", issues.size()},
                {"artifacts", std::move(artifacts)}};
}

std::string joined_strings(const std::vector<std::string> &items) {
    std::string result;
    for (const auto &item : items) {
        if (!result.empty())
            result += '|';
        result += item;
    }
    return result;
}

axk::ReportRow relationship_report_row(const LoadedSource &source, const axk::Relationship &row,
                                       std::string_view display_path) {
    std::string target;
    if (row.target_key) {
        target = public_object_key(source, *row.target_key);
    } else {
        for (const auto &candidate : row.candidate_keys) {
            if (!target.empty())
                target += '|';
            target += public_object_key(source, candidate);
        }
    }
    const auto source_key = public_object_key(source, row.source_key);
    std::string raw_fields;
    std::string notes = row.notes;
    const auto source_object =
        std::ranges::find(source.inventory.catalog.objects, row.source_key, &axk::ObjectSnapshot::key);
    if (source_object != source.inventory.catalog.objects.end() &&
        (row.type == "SBNK_LEFT_MEMBER_TO_SMPL" || row.type == "SBNK_RIGHT_MEMBER_TO_SMPL")) {
        if (const auto *bank = std::get_if<axk::CurrentSbnk>(&source_object->object.payload)) {
            const bool right = row.type == "SBNK_RIGHT_MEMBER_TO_SMPL";
            const auto *member = right && bank->right ? &*bank->right : &bank->left;
            raw_fields = std::format("SBNK+{} member {}; name={}; link_id=0x{:08x}", right ? "right" : "left",
                                     right ? "right" : "left", member->sample_name, member->smpl_link_id);
            if (row.basis == "sbnk-member-link+name") {
                notes = "Current SBNK member name and member link ID match exactly one same-scope SMPL object.";
            }
        }
    } else if (source_object != source.inventory.catalog.objects.end() && row.type == "SBAC_SLOT_TO_SBNK") {
        if (const auto *group = std::get_if<axk::CurrentSbac>(&source_object->object.payload)) {
            std::size_t index{};
            if (row.target_key) {
                const auto target_object =
                    std::ranges::find(source.inventory.catalog.objects, *row.target_key, &axk::ObjectSnapshot::key);
                if (target_object != source.inventory.catalog.objects.end()) {
                    const auto found =
                        std::ranges::find(group->slots, target_object->object.header.name, &axk::SbacSlot::name);
                    if (found != group->slots.end())
                        index = static_cast<std::size_t>(std::distance(group->slots.begin(), found));
                }
            }
            const auto offset = index < group->slots.size() ? group->slots[index].offset : 0x14cU;
            raw_fields = std::format("SBAC slot {} at 0x{:03x}", index, offset);
            if (row.basis == "active-sbac-slot-name") {
                notes = "Input consistency: counted SBAC slot name uniquely matches a same-scope SBNK header name. "
                        "The companion 32-bit slot word is preserved as raw/opaque.";
            }
        }
    } else if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG") {
        raw_fields = "SBNK+0x0c0..0x0cf";
        notes = "Validated standalone assignment rows support SBNK+0x0c0..0x0cf as four big-endian program-link "
                "bitmap words for direct PROG->SBNK/sample assignments. PROG->SBAC assignments are reported "
                "separately as indirection and are not expected to set child SBNK bits.";
    } else if (row.assignment_index) {
        raw_fields = std::format("PROG assignment {} at 0x{:03x}", *row.assignment_index,
                                 0x120U + static_cast<unsigned int>(*row.assignment_index) * 0x38U);
    }
    std::string diagnostic;
    if (row.assignment_state == axk::AssignmentState::visible_off)
        diagnostic = "visible-off-assignment";
    else if (row.basis == "assignment-active-missing-local-target")
        diagnostic = "active-assignment-missing-target";
    else if (row.basis.starts_with("sbnk-program-link-bitmap-"))
        diagnostic = "program-link-bitmap";
    else if (row.quality == axk::RelationshipQuality::tentative)
        diagnostic = row.basis.starts_with("sbnk-member-link-id-only") ? "sbnk-member-link" : "ambiguous-target";
    else if (row.quality == axk::RelationshipQuality::unknown)
        diagnostic = "missing-target";
    return {{"key", std::format("{}|{}|{}|{}", source_key, row.type, target.empty() ? "missing" : target, row.basis)},
            {"source_key", source_key},
            {"target_key", target},
            {"relationship_type", row.type},
            {"quality", std::string{axk::relationship_quality_name(row.quality)}},
            {"basis", row.basis},
            {"raw_fields", raw_fields},
            {"ambiguity_notes", notes},
            {"source_image", std::string{display_path}},
            {"scope_key", std::format("{}:{}", display_path, row.scope_key)},
            {"assignment_index", row.assignment_index
                                     ? axk::ReportValue{static_cast<std::uint64_t>(*row.assignment_index)}
                                     : axk::ReportValue{nullptr}},
            {"assignment_name", row.assignment_name},
            {"assignment_row_state", row.assignment_index ? "decoded-row" : ""},
            {"active_assignment_state",
             row.assignment_index ? std::string{axk::assignment_state_name(row.assignment_state)} : std::string{}},
            {"assignment_rch_assign_display", row.receive_channel_display},
            {"diagnostic_category", diagnostic}};
}

std::size_t program_ignored_count(const LoadedSource &source) {
    std::size_t result{};
    for (const auto &item : source.inventory.catalog.objects) {
        const auto *program = std::get_if<axk::CurrentProg>(&item.object.payload);
        if (program == nullptr)
            continue;
        std::set<std::size_t> represented;
        for (const auto &relation : source.graph.relationships) {
            if (relation.source_key == item.key && relation.type.starts_with("PROG_ASSIGNMENT_TO_") &&
                relation.assignment_index) {
                represented.insert(*relation.assignment_index);
            }
        }
        for (std::size_t index = 0; index < program->assignments.size(); ++index) {
            const auto &assignment = program->assignments[index];
            if (assignment.name.empty() || represented.contains(index))
                continue;
            const bool known_kind = assignment.kind == 0x10U || assignment.kind == 0x11U;
            const bool name_match = std::ranges::any_of(source.inventory.catalog.objects, [&](const auto &target) {
                return target.scope_key == item.scope_key && target.object.header.name == assignment.name;
            });
            if ((!known_kind && !name_match) || assignment.raw_handle == 0U)
                ++result;
        }
    }
    return result;
}

const axk::ObjectSnapshot *catalog_object(const LoadedSource &source, std::string_view key) {
    const auto found = std::ranges::find(source.inventory.catalog.objects, key, &axk::ObjectSnapshot::key);
    return found == source.inventory.catalog.objects.end() ? nullptr : &*found;
}

const axk::MediaObjectDescriptor *media_object(const LoadedSource &source, std::string_view key) {
    const auto found = std::ranges::find(source.inventory.objects, key, &axk::MediaObjectDescriptor::key);
    return found == source.inventory.objects.end() ? nullptr : &*found;
}

const axk::FatFile *fat_file_metadata(const LoadedSource &source, const axk::MediaObjectDescriptor *object) {
    if (object == nullptr)
        return nullptr;
    const auto *fat = std::get_if<axk::FatImage>(&source.media.storage());
    if (fat == nullptr)
        return nullptr;
    const auto found = std::ranges::find(fat->files(), object->logical_path, &axk::FatFile::path);
    return found == fat->files().end() ? nullptr : &*found;
}

std::uint64_t sfs_payload_offset(const LoadedSource &source, const axk::ObjectSnapshot &item) {
    if (source.media.kind() != axk::MediaKind::sfs)
        return 0U;
    const auto &container = std::get<axk::Container>(source.media.storage());
    const auto partition = std::ranges::find(container.partitions(), item.partition.value,
                                             [](const auto &row) { return row.index.value; });
    if (partition == container.partitions().end())
        return 0U;
    const auto record =
        std::ranges::find(partition->records, item.sfs_id.value, [](const auto &row) { return row.sfs_id.value; });
    if (record == partition->records.end() || record->extents.empty())
        return 0U;
    return (static_cast<std::uint64_t>(partition->start_sector) +
            static_cast<std::uint64_t>(record->extents.front().cluster_offset) * partition->sectors_per_cluster) *
           container.superblock().sector_size_bytes;
}

std::string joined_programs(const std::vector<std::uint8_t> &items) {
    std::string result;
    for (const auto item : items) {
        if (!result.empty())
            result += '|';
        result += std::format("{:03}", item);
    }
    return result;
}

axk::ReportValue optional_unsigned(bool present, std::uint64_t value) {
    return present ? axk::ReportValue{value} : axk::ReportValue{nullptr};
}

std::vector<axk::ReportRow> sbac_detail_rows(std::span<const LoadedSource> sources,
                                             const axk::app::OperationContext &context) {
    std::vector<axk::ReportRow> rows;
    for (const auto &source : sources) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &relation : source.graph.relationships) {
            if (relation.type != "SBAC_SLOT_TO_SBNK")
                continue;
            const auto *sbac_item = catalog_object(source, relation.source_key);
            if (sbac_item == nullptr)
                continue;
            const auto *sbac = std::get_if<axk::CurrentSbac>(&sbac_item->object.payload);
            if (sbac == nullptr)
                continue;
            const auto matched = relation.target_key ? catalog_object(source, *relation.target_key) : nullptr;
            std::size_t slot_index{};
            const axk::SbacSlot *slot{};
            for (std::size_t index = 0; index < sbac->slots.size(); ++index) {
                if (sbac->slots[index].name == relation.assignment_name ||
                    (relation.assignment_name.empty() && matched != nullptr &&
                     sbac->slots[index].name == matched->object.header.name)) {
                    slot_index = index;
                    slot = &sbac->slots[index];
                    break;
                }
            }
            if (slot == nullptr) {
                const auto named =
                    std::ranges::find_if(sbac->slots, [](const auto &item) { return !item.name.empty(); });
                if (named == sbac->slots.end())
                    continue;
                slot_index = static_cast<std::size_t>(std::distance(sbac->slots.begin(), named));
                slot = &*named;
            }
            std::vector<std::string> candidate_keys;
            std::vector<std::string> candidate_files;
            std::vector<std::string> candidate_names;
            for (const auto &key : relation.candidate_keys) {
                candidate_keys.push_back(public_object_key(source, key));
                if (const auto *candidate = catalog_object(source, key))
                    candidate_names.push_back(candidate->object.header.name);
                if (const auto *object = media_object(source, key))
                    candidate_files.push_back(object->logical_path);
            }
            const auto *sbac_media = media_object(source, sbac_item->key);
            const auto *matched_media = matched == nullptr ? nullptr : media_object(source, matched->key);
            const auto *sbac_fat = fat_file_metadata(source, sbac_media);
            const auto *matched_fat = fat_file_metadata(source, matched_media);
            const bool sfs = source.media.kind() == axk::MediaKind::sfs;
            const bool iso = source.media.kind() == axk::MediaKind::iso9660;
            const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
            const auto match_notes = relation.quality == axk::RelationshipQuality::known
                                         ? "Input consistency: counted SBAC slot name uniquely matches a same-scope "
                                           "SBNK header name. The companion 32-bit slot word is preserved as "
                                           "raw/opaque."
                                         : relation.notes;
            rows.push_back({
                {"image", display_path},
                {"container_kind", info_media_kind_name(source.media.kind())},
                {"scope_key", public_scope_key(source, *sbac_item, display_path)},
                {"sbac_object_key", public_object_key(source, sbac_item->key)},
                {"sbac_partition_index", optional_unsigned(sfs, sbac_item->partition.value)},
                {"sbac_sfs_id", optional_unsigned(sfs, sbac_item->sfs_id.value)},
                {"sbac_fat_file", fat && sbac_media != nullptr ? sbac_media->logical_path : ""},
                {"sbac_payload_offset",
                 optional_unsigned(sfs || sbac_media != nullptr,
                                   sfs ? sfs_payload_offset(source, *sbac_item) : sbac_media->data_offset)},
                {"sbac_name", sbac_item->object.header.name},
                {"sbac_payload_size", sbac_media != nullptr ? sbac_media->size : std::uint64_t{0}},
                {"sbac_slot_count_0x144", static_cast<std::uint64_t>(sbac->active_slot_count)},
                {"slot_index", static_cast<std::uint64_t>(slot_index)},
                {"slot_offset", static_cast<std::uint64_t>(slot->offset)},
                {"slot_sbnk_name", slot->name},
                {"slot_raw_handle_0x10", static_cast<std::uint64_t>(slot->raw_handle)},
                {"match_method", relation.basis},
                {"match_quality", std::string{axk::relationship_quality_name(relation.quality)}},
                {"match_notes", match_notes},
                {"candidate_count", static_cast<std::uint64_t>(relation.candidate_keys.size())},
                {"candidate_object_keys", joined_strings(candidate_keys)},
                {"candidate_fat_files", fat ? joined_strings(candidate_files) : ""},
                {"candidate_names", joined_strings(candidate_names)},
                {"matched_sbnk_object_key", matched == nullptr ? "" : public_object_key(source, matched->key)},
                {"matched_sbnk_partition_index",
                 optional_unsigned(sfs && matched != nullptr, matched == nullptr ? 0U : matched->partition.value)},
                {"matched_sbnk_sfs_id",
                 optional_unsigned(sfs && matched != nullptr, matched == nullptr ? 0U : matched->sfs_id.value)},
                {"matched_sbnk_fat_file", fat && matched_media != nullptr ? matched_media->logical_path : ""},
                {"matched_sbnk_payload_offset",
                 optional_unsigned(matched != nullptr && (sfs || matched_media != nullptr),
                                   matched == nullptr ? 0U
                                   : sfs              ? sfs_payload_offset(source, *matched)
                                                      : matched_media->data_offset)},
                {"matched_sbnk_name", matched == nullptr ? "" : matched->object.header.name},
                {"notes", ""},
                {"sbac_iso_extent_sector",
                 optional_unsigned(iso && sbac_media != nullptr,
                                   sbac_media == nullptr ? 0U : sbac_media->data_offset / 2048U)},
                {"sbac_iso_data_offset",
                 optional_unsigned(iso && sbac_media != nullptr, sbac_media == nullptr ? 0U : sbac_media->data_offset)},
                {"sbac_iso_file_size",
                 optional_unsigned(iso && sbac_media != nullptr, sbac_media == nullptr ? 0U : sbac_media->size)},
                {"sbac_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
                {"sbac_fat_directory_offset",
                 optional_unsigned(sbac_fat != nullptr, sbac_fat == nullptr ? 0U : sbac_fat->directory_offset)},
                {"sbac_fat_first_cluster",
                 optional_unsigned(sbac_fat != nullptr, sbac_fat == nullptr ? 0U : sbac_fat->first_cluster)},
                {"sbac_fat_cluster_count",
                 optional_unsigned(sbac_fat != nullptr, sbac_fat == nullptr ? 0U : sbac_fat->clusters.size())},
                {"sbac_fat_file_size",
                 optional_unsigned(fat && sbac_media != nullptr, sbac_media == nullptr ? 0U : sbac_media->size)},
                {"sbac_fat_object_offset",
                 optional_unsigned(fat && sbac_media != nullptr, sbac_media == nullptr ? 0U : sbac_media->data_offset)},
                {"sbac_fat_stored_payload_offset",
                 optional_unsigned(sbac_fat != nullptr, sbac_fat == nullptr ? 0U : sbac_fat->first_data_offset)},
                {"matched_sbnk_iso_extent_sector",
                 optional_unsigned(iso && matched_media != nullptr,
                                   matched_media == nullptr ? 0U : matched_media->data_offset / 2048U)},
                {"matched_sbnk_iso_data_offset",
                 optional_unsigned(iso && matched_media != nullptr,
                                   matched_media == nullptr ? 0U : matched_media->data_offset)},
                {"matched_sbnk_iso_file_size", optional_unsigned(iso && matched_media != nullptr,
                                                                 matched_media == nullptr ? 0U : matched_media->size)},
                {"matched_sbnk_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
                {"matched_sbnk_fat_directory_offset",
                 optional_unsigned(matched_fat != nullptr,
                                   matched_fat == nullptr ? 0U : matched_fat->directory_offset)},
                {"matched_sbnk_fat_first_cluster",
                 optional_unsigned(matched_fat != nullptr, matched_fat == nullptr ? 0U : matched_fat->first_cluster)},
                {"matched_sbnk_fat_cluster_count",
                 optional_unsigned(matched_fat != nullptr, matched_fat == nullptr ? 0U : matched_fat->clusters.size())},
                {"matched_sbnk_fat_file_size", optional_unsigned(fat && matched_media != nullptr,
                                                                 matched_media == nullptr ? 0U : matched_media->size)},
                {"matched_sbnk_fat_object_offset",
                 optional_unsigned(fat && matched_media != nullptr,
                                   matched_media == nullptr ? 0U : matched_media->data_offset)},
                {"matched_sbnk_fat_stored_payload_offset",
                 optional_unsigned(matched_fat != nullptr,
                                   matched_fat == nullptr ? 0U : matched_fat->first_data_offset)},
            });
        }
    }
    return rows;
}

std::vector<axk::ReportRow> bitmap_detail_rows(std::span<const LoadedSource> sources,
                                               const axk::app::OperationContext &context) {
    static constexpr std::string_view notes =
        "Validated standalone assignment rows support SBNK+0x0c0..0x0cf as four big-endian program-link bitmap "
        "words for direct PROG->SBNK/sample assignments. PROG->SBAC assignments are reported separately as "
        "indirection and are not expected to set child SBNK bits.";
    std::vector<axk::ReportRow> rows;
    for (const auto &source : sources) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &comparison : source.graph.bitmap_comparisons) {
            const auto *item = catalog_object(source, comparison.sbnk_key);
            if (item == nullptr)
                continue;
            const auto *bank = std::get_if<axk::CurrentSbnk>(&item->object.payload);
            if (bank == nullptr)
                continue;
            const auto *object = media_object(source, item->key);
            const bool sfs = source.media.kind() == axk::MediaKind::sfs;
            const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
            std::vector<std::string> direct_details;
            std::vector<std::string> ambiguous_programs;
            std::vector<std::string> ambiguous_details;
            for (const auto &relation : source.graph.relationships) {
                if (relation.type != "PROG_ASSIGNMENT_TO_SBNK" || !relation.assignment_index)
                    continue;
                const auto direct = relation.target_key && *relation.target_key == item->key;
                const auto ambiguous =
                    relation.quality == axk::RelationshipQuality::tentative &&
                    std::ranges::find(relation.candidate_keys, item->key) != relation.candidate_keys.end();
                if (!direct && !ambiguous)
                    continue;
                const auto *program_item = catalog_object(source, relation.source_key);
                if (program_item == nullptr)
                    continue;
                const auto *program = std::get_if<axk::CurrentProg>(&program_item->object.payload);
                if (program == nullptr || *relation.assignment_index >= program->assignments.size())
                    continue;
                const auto &assignment = program->assignments[*relation.assignment_index];
                const auto detail = std::format("{}@slot{}:kind0x{:02x}:flag0x{:02x}", program_item->object.header.name,
                                                *relation.assignment_index, assignment.kind, assignment.flags);
                if (direct) {
                    direct_details.push_back(detail);
                } else {
                    ambiguous_programs.push_back(program_item->object.header.name);
                    ambiguous_details.push_back(detail);
                }
            }
            std::ranges::sort(direct_details);
            std::ranges::sort(ambiguous_programs);
            std::ranges::sort(ambiguous_details);
            direct_details.erase(std::unique(direct_details.begin(), direct_details.end()), direct_details.end());
            ambiguous_programs.erase(std::unique(ambiguous_programs.begin(), ambiguous_programs.end()),
                                     ambiguous_programs.end());
            ambiguous_details.erase(std::unique(ambiguous_details.begin(), ambiguous_details.end()),
                                    ambiguous_details.end());
            rows.push_back({
                {"image", display_path},
                {"container_kind", info_media_kind_name(source.media.kind())},
                {"scope_key", public_scope_key(source, *item, display_path)},
                {"sbnk_object_key", public_object_key(source, item->key)},
                {"sbnk_partition_index", optional_unsigned(sfs, item->partition.value)},
                {"sbnk_sfs_id", optional_unsigned(sfs, item->sfs_id.value)},
                {"sbnk_fat_file", fat && object != nullptr ? object->logical_path : ""},
                {"sbnk_payload_offset",
                 optional_unsigned(sfs || object != nullptr,
                                   sfs ? sfs_payload_offset(source, *item) : object->data_offset)},
                {"sbnk_name", item->object.header.name},
                {"linked_programs_001_032_bitmap_0x0c0",
                 static_cast<std::uint64_t>(bank->linked_program_bitmap_words[0])},
                {"linked_programs_033_064_bitmap_0x0c4",
                 static_cast<std::uint64_t>(bank->linked_program_bitmap_words[1])},
                {"linked_programs_065_096_bitmap_0x0c8",
                 static_cast<std::uint64_t>(bank->linked_program_bitmap_words[2])},
                {"linked_programs_097_128_bitmap_0x0cc",
                 static_cast<std::uint64_t>(bank->linked_program_bitmap_words[3])},
                {"bitmap_programs", joined_programs(comparison.bitmap_programs)},
                {"direct_prog_assignment_programs", joined_programs(comparison.direct_assignment_programs)},
                {"direct_prog_assignment_details", joined_strings(direct_details)},
                {"ambiguous_direct_assignment_programs", joined_strings(ambiguous_programs)},
                {"ambiguous_direct_assignment_details", joined_strings(ambiguous_details)},
                {"sbac_indirect_assignment_programs", joined_programs(comparison.indirect_assignment_programs)},
                {"bitmap_without_direct_assignment_programs", joined_programs(comparison.bitmap_without_direct)},
                {"direct_assignment_without_bitmap_programs", joined_programs(comparison.direct_without_bitmap)},
                {"mismatch_class", comparison.mismatch_class},
                {"match_status", comparison.status},
                {"quality", comparison.status == "match" ? "Known" : "Tentative"},
                {"notes", std::string{notes}},
            });
        }
    }
    return rows;
}

std::vector<axk::ReportRow> program_detail_rows(std::span<const LoadedSource> sources,
                                                const axk::app::OperationContext &context) {
    std::vector<axk::ReportRow> rows;
    for (const auto &source : sources) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &relation : source.graph.relationships) {
            if (!relation.type.starts_with("PROG_ASSIGNMENT_TO_") || !relation.assignment_index)
                continue;
            const auto *program_item = catalog_object(source, relation.source_key);
            if (program_item == nullptr)
                continue;
            const auto *program = std::get_if<axk::CurrentProg>(&program_item->object.payload);
            if (program == nullptr || *relation.assignment_index >= program->assignments.size())
                continue;
            const auto &assignment = program->assignments[*relation.assignment_index];
            const auto *target = relation.target_key ? catalog_object(source, *relation.target_key) : nullptr;
            const auto *program_media = media_object(source, program_item->key);
            const auto *target_media = target == nullptr ? nullptr : media_object(source, target->key);
            const auto *program_fat = fat_file_metadata(source, program_media);
            const auto *target_fat = fat_file_metadata(source, target_media);
            const bool sfs = source.media.kind() == axk::MediaKind::sfs;
            const bool iso = source.media.kind() == axk::MediaKind::iso9660;
            const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
            std::vector<std::string> candidate_keys;
            std::vector<std::string> candidate_files;
            std::vector<std::string> candidate_names;
            std::vector<std::string> candidate_categories;
            for (const auto &key : relation.candidate_keys) {
                candidate_keys.push_back(public_object_key(source, key));
                if (const auto *candidate = catalog_object(source, key)) {
                    candidate_names.push_back(candidate->object.header.name);
                    candidate_categories.push_back(object_type_name(candidate->object.header.type));
                }
                if (const auto *object = media_object(source, key))
                    candidate_files.push_back(object->logical_path);
            }
            std::ranges::sort(candidate_categories);
            candidate_categories.erase(std::unique(candidate_categories.begin(), candidate_categories.end()),
                                       candidate_categories.end());
            axk::ReportValue child_count{nullptr};
            if (target != nullptr && target->object.header.type == axk::ObjectType::sbac) {
                child_count =
                    static_cast<std::uint64_t>(std::ranges::count_if(source.graph.relationships, [&](const auto &row) {
                        return row.source_key == target->key && row.type == "SBAC_SLOT_TO_SBNK" &&
                               row.target_key.has_value();
                    }));
            }
            const auto expected = assignment.kind == 0x11U ? "SBAC" : assignment.kind == 0x10U ? "SBNK" : "";
            rows.push_back({
                {"image", display_path},
                {"container_kind", info_media_kind_name(source.media.kind())},
                {"scope_key", public_scope_key(source, *program_item, display_path)},
                {"prog_object_key", public_object_key(source, program_item->key)},
                {"prog_partition_index", optional_unsigned(sfs, program_item->partition.value)},
                {"prog_sfs_id", optional_unsigned(sfs, program_item->sfs_id.value)},
                {"prog_fat_file", fat && program_media != nullptr ? program_media->logical_path : ""},
                {"prog_payload_offset",
                 optional_unsigned(sfs || program_media != nullptr,
                                   sfs ? sfs_payload_offset(source, *program_item) : program_media->data_offset)},
                {"prog_name", program_item->object.header.name},
                {"prog_payload_size", program_media != nullptr ? program_media->size : std::uint64_t{0}},
                {"assignment_index", static_cast<std::uint64_t>(*relation.assignment_index)},
                {"assignment_offset", static_cast<std::uint64_t>(0x120U + *relation.assignment_index * 0x38U)},
                {"assignment_name", assignment.name},
                {"assignment_raw_handle_0x10", static_cast<std::uint64_t>(assignment.raw_handle)},
                {"assignment_kind_byte_0x14", static_cast<std::uint64_t>(assignment.kind)},
                {"assignment_flag_byte_0x15", static_cast<std::uint64_t>(assignment.flags)},
                {"assignment_output1_byte_0x1d",
                 static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(assignment.raw_row[0x1d]))},
                {"assignment_rch_assign_gate_byte_0x28",
                 static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(assignment.raw_row[0x28]))},
                {"assignment_rch_assign_display", relation.receive_channel_display},
                {"selector_expected_category", expected},
                {"assignment_row_state", "decoded-row"},
                {"active_assignment_state", std::string{axk::assignment_state_name(relation.assignment_state)}},
                {"match_method", relation.basis},
                {"match_quality", std::string{axk::relationship_quality_name(relation.quality)}},
                {"match_notes", relation.notes},
                {"candidate_count", static_cast<std::uint64_t>(relation.candidate_keys.size())},
                {"candidate_categories", joined_strings(candidate_categories)},
                {"candidate_object_keys", joined_strings(candidate_keys)},
                {"candidate_fat_files", fat ? joined_strings(candidate_files) : ""},
                {"candidate_names", joined_strings(candidate_names)},
                {"matched_target_type", target == nullptr ? "" : object_type_name(target->object.header.type)},
                {"matched_target_object_key", target == nullptr ? "" : public_object_key(source, target->key)},
                {"matched_target_partition_index",
                 optional_unsigned(sfs && target != nullptr, target == nullptr ? 0U : target->partition.value)},
                {"matched_target_sfs_id",
                 optional_unsigned(sfs && target != nullptr, target == nullptr ? 0U : target->sfs_id.value)},
                {"matched_target_fat_file", fat && target_media != nullptr ? target_media->logical_path : ""},
                {"matched_target_payload_offset",
                 optional_unsigned(target != nullptr && (sfs || target_media != nullptr),
                                   target == nullptr ? 0U
                                   : sfs             ? sfs_payload_offset(source, *target)
                                                     : target_media->data_offset)},
                {"matched_target_name", target == nullptr ? "" : target->object.header.name},
                {"matched_sbac_child_sbnk_count", child_count},
                {"notes", ""},
                {"prog_iso_extent_sector",
                 optional_unsigned(iso && program_media != nullptr,
                                   program_media == nullptr ? 0U : program_media->data_offset / 2048U)},
                {"prog_iso_data_offset", optional_unsigned(iso && program_media != nullptr,
                                                           program_media == nullptr ? 0U : program_media->data_offset)},
                {"prog_iso_file_size", optional_unsigned(iso && program_media != nullptr,
                                                         program_media == nullptr ? 0U : program_media->size)},
                {"prog_iso_recovery_quality", iso ? "clean-iso9660-object" : ""},
                {"prog_fat_directory_offset",
                 optional_unsigned(program_fat != nullptr,
                                   program_fat == nullptr ? 0U : program_fat->directory_offset)},
                {"prog_fat_first_cluster",
                 optional_unsigned(program_fat != nullptr, program_fat == nullptr ? 0U : program_fat->first_cluster)},
                {"prog_fat_cluster_count",
                 optional_unsigned(program_fat != nullptr, program_fat == nullptr ? 0U : program_fat->clusters.size())},
                {"prog_fat_file_size", optional_unsigned(fat && program_media != nullptr,
                                                         program_media == nullptr ? 0U : program_media->size)},
                {"prog_fat_object_offset",
                 optional_unsigned(fat && program_media != nullptr,
                                   program_media == nullptr ? 0U : program_media->data_offset)},
                {"prog_fat_stored_payload_offset",
                 optional_unsigned(program_fat != nullptr,
                                   program_fat == nullptr ? 0U : program_fat->first_data_offset)},
                {"matched_target_iso_extent_sector",
                 optional_unsigned(iso && target_media != nullptr,
                                   target_media == nullptr ? 0U : target_media->data_offset / 2048U)},
                {"matched_target_iso_data_offset",
                 optional_unsigned(iso && target_media != nullptr,
                                   target_media == nullptr ? 0U : target_media->data_offset)},
                {"matched_target_iso_file_size",
                 optional_unsigned(iso && target_media != nullptr, target_media == nullptr ? 0U : target_media->size)},
                {"matched_target_iso_recovery_quality", iso && target_media != nullptr ? "clean-iso9660-object" : ""},
                {"matched_target_fat_directory_offset",
                 optional_unsigned(target_fat != nullptr, target_fat == nullptr ? 0U : target_fat->directory_offset)},
                {"matched_target_fat_first_cluster",
                 optional_unsigned(target_fat != nullptr, target_fat == nullptr ? 0U : target_fat->first_cluster)},
                {"matched_target_fat_cluster_count",
                 optional_unsigned(target_fat != nullptr, target_fat == nullptr ? 0U : target_fat->clusters.size())},
                {"matched_target_fat_file_size",
                 optional_unsigned(fat && target_media != nullptr, target_media == nullptr ? 0U : target_media->size)},
                {"matched_target_fat_object_offset",
                 optional_unsigned(fat && target_media != nullptr,
                                   target_media == nullptr ? 0U : target_media->data_offset)},
                {"matched_target_fat_stored_payload_offset",
                 optional_unsigned(target_fat != nullptr, target_fat == nullptr ? 0U : target_fat->first_data_offset)},
            });
        }
    }
    return rows;
}

std::vector<axk::ReportRow> program_ignored_detail_rows(std::span<const LoadedSource> sources,
                                                        const axk::app::OperationContext &context) {
    std::vector<axk::ReportRow> rows;
    for (const auto &source : sources) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &item : source.inventory.catalog.objects) {
            const auto *program = std::get_if<axk::CurrentProg>(&item.object.payload);
            if (program == nullptr)
                continue;
            std::set<std::size_t> represented;
            for (const auto &relation : source.graph.relationships) {
                if (relation.source_key == item.key && relation.type.starts_with("PROG_ASSIGNMENT_TO_") &&
                    relation.assignment_index) {
                    represented.insert(*relation.assignment_index);
                }
            }
            const auto *program_media = media_object(source, item.key);
            const bool sfs = source.media.kind() == axk::MediaKind::sfs;
            const bool fat = source.media.kind() == axk::MediaKind::fat12_floppy;
            for (std::size_t index = 0; index < program->assignments.size(); ++index) {
                const auto &assignment = program->assignments[index];
                if (assignment.name.empty() || represented.contains(index))
                    continue;
                const bool known_kind = assignment.kind == 0x10U || assignment.kind == 0x11U;
                const bool name_match = std::ranges::any_of(source.inventory.catalog.objects, [&](const auto &target) {
                    return target.scope_key == item.scope_key && target.object.header.name == assignment.name;
                });
                std::string reason;
                if (!known_kind && !name_match) {
                    reason = "ignored-reserved-or-tail-slot-no-known-kind-and-no-name-match";
                } else if (assignment.raw_handle == 0U) {
                    reason = "ignored-null-handle-unmatched-assignment";
                } else {
                    continue;
                }
                rows.push_back({
                    {"image", display_path},
                    {"container_kind", info_media_kind_name(source.media.kind())},
                    {"scope_key", public_scope_key(source, item, display_path)},
                    {"prog_object_key", public_object_key(source, item.key)},
                    {"prog_partition_index", optional_unsigned(sfs, item.partition.value)},
                    {"prog_sfs_id", optional_unsigned(sfs, item.sfs_id.value)},
                    {"prog_fat_file", fat && program_media != nullptr ? program_media->logical_path : ""},
                    {"prog_payload_offset", optional_unsigned(sfs || program_media != nullptr,
                                                              sfs ? sfs_payload_offset(source, item)
                                                              : program_media == nullptr ? 0U
                                                                                         : program_media->data_offset)},
                    {"prog_name", item.object.header.name},
                    {"prog_payload_size", program_media == nullptr ? std::uint64_t{0} : program_media->size},
                    {"assignment_index", static_cast<std::uint64_t>(index)},
                    {"assignment_offset", static_cast<std::uint64_t>(0x120U + index * 0x38U)},
                    {"raw_name_guess", assignment.name},
                    {"assignment_raw_handle_0x10", static_cast<std::uint64_t>(assignment.raw_handle)},
                    {"assignment_kind_byte_0x14", static_cast<std::uint64_t>(assignment.kind)},
                    {"assignment_flag_byte_0x15", static_cast<std::uint64_t>(assignment.flags)},
                    {"reason", std::move(reason)},
                });
            }
        }
    }
    return rows;
}

axk::ReportRow coverage_summary(const std::vector<LoadedSource> &sources, std::span<const axk::ReportRow> relationships,
                                std::size_t load_error_count) {
    std::map<std::string, std::uint64_t> qualities;
    std::map<std::string, std::uint64_t> types;
    std::uint64_t sbac{};
    std::uint64_t program{};
    std::uint64_t bitmaps{};
    std::uint64_t ignored{};
    for (const auto &source : sources) {
        for (const auto &row : source.graph.relationships) {
            ++qualities[std::string{axk::relationship_quality_name(row.quality)}];
            ++types[row.type];
            if (row.type == "SBAC_SLOT_TO_SBNK")
                ++sbac;
            if (row.type.starts_with("PROG_ASSIGNMENT_TO_"))
                ++program;
        }
        bitmaps += source.graph.bitmap_comparisons.size();
        ignored += program_ignored_count(source);
    }
    const auto joined = [](const auto &counts) {
        std::string result;
        for (const auto &[name, count] : counts) {
            if (count == 0U)
                continue;
            if (!result.empty())
                result += ';';
            result += std::format("{}:{}", name, count);
        }
        return result;
    };
    return {{"relationship_count", static_cast<std::uint64_t>(relationships.size())},
            {"known_relationship_count", qualities["Known"]},
            {"likely_relationship_count", qualities["Likely"]},
            {"tentative_relationship_count", qualities["Tentative"]},
            {"unknown_relationship_count", qualities["Unknown"]},
            {"ambiguous_relationship_count", qualities["Tentative"]},
            {"sbac_sbnk_row_count", sbac},
            {"prog_assignment_row_count", program},
            {"prog_ignored_row_count", ignored},
            {"sbnk_bitmap_row_count", bitmaps},
            {"relationship_type_counts", joined(types)},
            {"quality_counts", joined(qualities)},
            {"load_error_count", static_cast<std::uint64_t>(load_error_count)}};
}

axk::app::Result<Json> execute_coverage(const axk::app::Sandbox &sandbox, const Json &input,
                                        const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.resolve_output_directory(request->destination, request->overwrite);
    if (!destination)
        return std::unexpected(destination.error());
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }
    std::vector<LoadedSource> loaded;
    std::vector<axk::ReportRow> load_errors;
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_info_source(sandbox, source_ref, request->include_default_programs, context);
        if (!source) {
            load_errors.push_back({{"path", source_display_path(source_ref, context)},
                                   {"error_code", source.error().error_code},
                                   {"message", source.error().error.message},
                                   {"recoverable", true},
                                   {"original_exception", source.error().original_exception}});
            continue;
        }
        loaded.push_back(std::move(*source));
    }
    std::vector<axk::ReportRow> relationships;
    for (const auto &source : loaded) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &row : source.graph.relationships)
            relationships.push_back(relationship_report_row(source, row, display_path));
    }
    auto summary = coverage_summary(loaded, relationships, load_errors.size());
    const std::array summary_rows{summary};
    if (auto written =
            axk::write_report_csv(*destination / "coverage_summary.csv", summary_rows, {}, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "coverage_summary.csv")}));
    }
    if (auto written = axk::write_report_object(*destination / "coverage_summary.json", summary, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "coverage_summary.json")}));
    }
    axk::ReportSchemaOptions summary_options;
    summary_options.source_command = "axklib";
    summary_options.library_version = std::string{axk::version()};
    auto summary_schema = axk::make_report_schema("coverage_summary", summary_rows, std::move(summary_options));
    if (auto written = axk::write_report_schema(*destination / "_schemas" / "coverage_summary.schema.json",
                                                summary_schema, request->overwrite);
        !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id,
                              child_reference_path(request->destination, "_schemas/coverage_summary.schema.json")}));
    }
    auto relation_schema =
        write_report_set(*destination, request->destination, "relationships", relationships, {}, request->overwrite);
    if (!relation_schema)
        return std::unexpected(relation_schema.error());
    auto error_schema =
        write_report_set(*destination, request->destination, "load_errors", load_errors, {}, request->overwrite);
    if (!error_schema)
        return std::unexpected(error_schema.error());
    const std::array schemas{summary_schema, *relation_schema, *error_schema};
    if (auto written = axk::write_report_schema_index(*destination / "_schemas" / "schema_index.json", schemas,
                                                      request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/schema_index.json")}));
    }
    auto artifacts = Json::array();
    for (const auto path :
         {"coverage_summary.csv", "coverage_summary.json", "relationships.csv", "relationships.json", "load_errors.csv",
          "load_errors.json", "_schemas/coverage_summary.schema.json", "_schemas/relationships.schema.json",
          "_schemas/load_errors.schema.json", "_schemas/schema_index.json"}) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    return Json{{"operationId", "report.coverage"}, {"sourceCount", request->sources.size()},
                {"loadedCount", loaded.size()},     {"failedCount", load_errors.size()},
                {"rowCount", relationships.size()}, {"artifacts", std::move(artifacts)}};
}

axk::app::Result<Json> execute_orphans(const axk::app::Sandbox &sandbox, const Json &input,
                                       const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.resolve_output_directory(request->destination, request->overwrite);
    if (!destination)
        return std::unexpected(destination.error());
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }

    std::vector<axk::ReportRow> rows;
    std::vector<axk::ReportRow> summaries;
    auto response_summaries = Json::array();
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_source(sandbox, source_ref, request->include_default_programs, context);
        if (!source)
            return std::unexpected(source.error());
        const auto *container = std::get_if<axk::Container>(&source->media.storage());
        if (container == nullptr) {
            return std::unexpected(operation_error("unsupported_media", "waveform orphan analysis requires SFS media",
                                                   source_ref.relative_path));
        }
        const auto report = axk::analyze_waveform_orphans(*container, source->inventory.catalog, source->graph);
        const auto display_path = source_display_path(source_ref, context);
        for (const auto &row : report.rows) {
            rows.push_back({
                {"source_path", display_path},
                {"partition_index", static_cast<std::uint64_t>(row.partition.value)},
                {"partition_name", row.partition_name},
                {"volume_name", row.volume_name},
                {"waveform_name", row.waveform_name},
                {"object_key", row.object_key},
                {"sfs_id", static_cast<std::uint64_t>(row.sfs_id.value)},
                {"smpl_link_id", static_cast<std::uint64_t>(row.smpl_link_id)},
                {"status", std::string{axk::waveform_status_name(row.status)}},
                {"referencing_sample_banks", joined_strings(row.referencing_sample_banks)},
                {"basis", row.basis},
                {"notes", row.notes},
            });
        }
        summaries.push_back({
            {"source_path", display_path},
            {"waveform_count", static_cast<std::uint64_t>(report.rows.size())},
            {"referenced_count", static_cast<std::uint64_t>(report.referenced_count)},
            {"known_unreferenced_count", static_cast<std::uint64_t>(report.known_unreferenced_count)},
            {"ambiguous_or_unresolved_count", static_cast<std::uint64_t>(report.ambiguous_or_unresolved_count)},
        });
        response_summaries.push_back({{"sourcePath", display_path},
                                      {"waveformCount", report.rows.size()},
                                      {"referencedCount", report.referenced_count},
                                      {"knownUnreferencedCount", report.known_unreferenced_count},
                                      {"ambiguousOrUnresolvedCount", report.ambiguous_or_unresolved_count}});
    }
    auto row_schema =
        write_report_set(*destination, request->destination, "waveform_orphans", rows, {}, request->overwrite);
    if (!row_schema)
        return std::unexpected(row_schema.error());
    auto summary_schema = write_report_set(*destination, request->destination, "waveform_orphan_summary", summaries, {},
                                           request->overwrite);
    if (!summary_schema)
        return std::unexpected(summary_schema.error());
    const std::array schemas{*row_schema, *summary_schema};
    const auto index_name = std::string{"_schemas/schema_index.json"};
    if (auto written = axk::write_report_schema_index(*destination / index_name, schemas, request->overwrite);
        !written) {
        return std::unexpected(core_error(
            written.error(), {request->destination.root_id, child_reference_path(request->destination, index_name)}));
    }
    auto artifacts = Json::array();
    for (const auto path : {"waveform_orphans.csv", "waveform_orphans.json", "waveform_orphan_summary.csv",
                            "waveform_orphan_summary.json", "_schemas/waveform_orphans.schema.json",
                            "_schemas/waveform_orphan_summary.schema.json", "_schemas/schema_index.json"}) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    if (context.progress != nullptr) {
        context.progress->report(
            {axk::ProgressPhase::writing, request->sources.size(), request->sources.size(), "orphans", std::nullopt});
    }
    return Json{{"operationId", "report.orphans"},
                {"sourceCount", request->sources.size()},
                {"loadedCount", request->sources.size()},
                {"failedCount", 0U},
                {"rowCount", rows.size()},
                {"summaries", std::move(response_summaries)},
                {"artifacts", std::move(artifacts)}};
}

axk::app::Result<Json> execute_relationships(const axk::app::Sandbox &sandbox, const Json &input,
                                             const axk::app::OperationContext &context) {
    const auto request = parse_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.resolve_output_directory(request->destination, request->overwrite);
    if (!destination)
        return std::unexpected(destination.error());
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }
    std::vector<LoadedSource> loaded;
    std::vector<axk::ReportRow> load_errors;
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_info_source(sandbox, source_ref, request->include_default_programs, context);
        if (!source) {
            load_errors.push_back({{"path", source_display_path(source_ref, context)},
                                   {"error_code", source.error().error_code},
                                   {"message", source.error().error.message},
                                   {"recoverable", true},
                                   {"original_exception", source.error().original_exception}});
            continue;
        }
        loaded.push_back(std::move(*source));
    }
    std::vector<axk::ReportRow> rows;
    std::size_t ambiguous_count{};
    for (const auto &source : loaded) {
        const auto display_path = source_display_path(source.source, context);
        for (const auto &row : source.graph.relationships) {
            rows.push_back(relationship_report_row(source, row, display_path));
            if (row.quality == axk::RelationshipQuality::tentative)
                ++ambiguous_count;
        }
    }
    const auto sbac_rows = sbac_detail_rows(loaded, context);
    const auto program_rows = program_detail_rows(loaded, context);
    const auto ignored_rows = program_ignored_detail_rows(loaded, context);
    const auto bitmap_rows = bitmap_detail_rows(loaded, context);

    std::vector<axk::ReportSchemaManifest> schemas;
    const auto append_report = [&](std::string name,
                                   std::span<const axk::ReportRow> report_rows) -> axk::app::Result<void> {
        auto schema =
            write_report_set(*destination, request->destination, std::move(name), report_rows, {}, request->overwrite);
        if (!schema)
            return std::unexpected(schema.error());
        schemas.push_back(std::move(*schema));
        return {};
    };
    if (auto written = append_report("relationships", rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("current_sbac_sbnk_links", sbac_rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("current_prog_bank_links", program_rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("current_prog_ignored_reserved_or_tail", ignored_rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("current_sbnk_program_bitmap_crosscheck", bitmap_rows); !written)
        return std::unexpected(written.error());
    if (auto written = append_report("load_errors", load_errors); !written)
        return std::unexpected(written.error());

    auto summary = coverage_summary(loaded, rows, load_errors.size());
    if (auto written =
            axk::write_report_object(*destination / "relationship_summary.json", summary, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "relationship_summary.json")}));
    }
    axk::ReportSchemaOptions summary_options;
    summary_options.source_command = "axklib";
    summary_options.library_version = std::string{axk::version()};
    auto summary_schema =
        axk::make_report_schema("relationship_summary", std::span{&summary, 1U}, std::move(summary_options));
    if (auto written = axk::write_report_schema(*destination / "_schemas" / "relationship_summary.schema.json",
                                                summary_schema, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(),
                       {request->destination.root_id,
                        child_reference_path(request->destination, "_schemas/relationship_summary.schema.json")}));
    }
    schemas.push_back(std::move(summary_schema));
    if (auto written = axk::write_report_schema_index(*destination / "_schemas" / "schema_index.json", schemas,
                                                      request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/schema_index.json")}));
    }
    auto artifacts = Json::array();
    for (const auto path : {
             "relationships.csv",
             "relationships.json",
             "current_sbac_sbnk_links.csv",
             "current_sbac_sbnk_links.json",
             "current_prog_bank_links.csv",
             "current_prog_bank_links.json",
             "current_prog_ignored_reserved_or_tail.csv",
             "current_prog_ignored_reserved_or_tail.json",
             "current_sbnk_program_bitmap_crosscheck.csv",
             "current_sbnk_program_bitmap_crosscheck.json",
             "load_errors.csv",
             "load_errors.json",
             "relationship_summary.json",
             "_schemas/relationships.schema.json",
             "_schemas/current_sbac_sbnk_links.schema.json",
             "_schemas/current_prog_bank_links.schema.json",
             "_schemas/current_prog_ignored_reserved_or_tail.schema.json",
             "_schemas/current_sbnk_program_bitmap_crosscheck.schema.json",
             "_schemas/load_errors.schema.json",
             "_schemas/relationship_summary.schema.json",
             "_schemas/schema_index.json",
         }) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    return Json{{"operationId", "report.relationships"},
                {"sourceCount", request->sources.size()},
                {"loadedCount", loaded.size()},
                {"failedCount", load_errors.size()},
                {"rowCount", rows.size()},
                {"ambiguousCount", ambiguous_count},
                {"artifacts", std::move(artifacts)}};
}

axk::app::Result<Json> execute_corpus_audit(const axk::app::Sandbox &sandbox, const Json &input,
                                            const axk::app::OperationContext &context) {
    const auto request = parse_corpus_audit_request(input);
    if (!request)
        return std::unexpected(request.error());
    const auto destination = sandbox.resolve_output_directory(request->destination, request->overwrite);
    if (!destination)
        return std::unexpected(destination.error());
    std::error_code filesystem_error;
    std::filesystem::create_directories(*destination / "_schemas", filesystem_error);
    if (filesystem_error) {
        return std::unexpected(operation_error("report_output_failed", "could not create report output directory",
                                               request->destination.relative_path));
    }

    std::vector<axk::ReportRow> manifest;
    std::vector<LoadedSource> loaded;
    std::size_t load_error_count{};
    loaded.reserve(request->sources.size());
    for (std::size_t index = 0; index < request->sources.size(); ++index) {
        const auto &source_ref = request->sources[index];
        const auto display = source_display_path(source_ref, context);
        const auto display_file = axk::text::path_from_utf8(display);
        auto suffix = display_file ? axk::text::path_to_utf8(display_file->extension()) : std::string{};
        std::ranges::transform(suffix, suffix.begin(),
                               [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
        const auto resolved = sandbox.resolve_file(source_ref);
        manifest.push_back({{"path", display},
                            {"exists", resolved.has_value()},
                            {"is_file", resolved.has_value()},
                            {"is_dir", false},
                            {"suffix", suffix}});
        if (context.progress != nullptr) {
            context.progress->report(
                {axk::ProgressPhase::reading, index, request->sources.size(), source_ref.relative_path, std::nullopt});
        }
        auto source = load_source(sandbox, source_ref, false, context, axk::MediaObjectReadMode::complete);
        if (!source) {
            ++load_error_count;
            continue;
        }
        loaded.push_back(std::move(*source));
    }

    std::vector<axk::ReportRow> inventory;
    std::vector<axk::ReportRow> relationships;
    std::vector<axk::ReportRow> validation_issues;
    std::vector<axk::ReportRow> wave_issues;
    std::uint64_t wave_decoded{};
    bool validation_failed{};
    std::uint64_t ambiguous{};
    for (const auto &source : loaded) {
        const auto display = source_display_path(source.source, context);
        for (const auto &item : source.inventory.catalog.objects)
            inventory.push_back(inventory_row(source, item, display));
        for (const auto &row : source.graph.relationships) {
            relationships.push_back(relationship_report_row(source, row, display));
            if (row.quality == axk::RelationshipQuality::tentative)
                ++ambiguous;
        }
        if (const auto *container = std::get_if<axk::Container>(&source.media.storage())) {
            const auto validation = axk::validate_semantics(*container, source.inventory.catalog, source.graph);
            validation_failed = validation_failed || !validation.valid();
            for (const auto &issue : validation.issues) {
                const auto severity = issue.severity == axk::ValidationSeverity::error     ? "error"
                                      : issue.severity == axk::ValidationSeverity::warning ? "warning"
                                                                                           : "info";
                validation_issues.push_back({{"severity", severity},
                                             {"code", issue.code},
                                             {"message", issue.message},
                                             {"scope", "relationship"},
                                             {"source_path", display},
                                             {"sampler_path", issue.sampler_path},
                                             {"object_key", issue.object_key},
                                             {"quality", "Known"},
                                             {"basis", "validation"},
                                             {"recommended_next_check", ""}});
            }
        }
        if (!request->skip_wave_smoke) {
            std::uint64_t successful{};
            for (const auto &item : source.inventory.catalog.objects) {
                if (item.object.header.type != axk::ObjectType::smpl)
                    continue;
                axk::Result<axk::Waveform> waveform =
                    source.media.kind() == axk::MediaKind::sfs
                        ? axk::decode_waveform(std::get<axk::Container>(source.media.storage()), item)
                        : [&]() -> axk::Result<axk::Waveform> {
                    const auto object =
                        std::ranges::find(source.inventory.objects, item.key, &axk::MediaObjectDescriptor::key);
                    if (object == source.inventory.objects.end()) {
                        return std::unexpected{axk::make_error(axk::ErrorCode::object_malformed,
                                                               axk::ErrorCategory::object,
                                                               "waveform object payload is unavailable")};
                    }
                    return axk::decode_waveform(item, object->logical_path);
                }();
                if (waveform) {
                    ++successful;
                } else {
                    wave_issues.push_back({{"source_path", display},
                                           {"container_kind", info_media_kind_name(source.media.kind())},
                                           {"object_key", public_object_key(source, item.key)},
                                           {"sample_name", item.object.header.name},
                                           {"code", static_cast<std::uint64_t>(waveform.error().code)},
                                           {"severity", "error"},
                                           {"message", waveform.error().message}});
                }
            }
            wave_decoded += std::min(successful, static_cast<std::uint64_t>(request->wave_smoke_limit));
        }
    }

    axk::ReportRow summary{
        {"input_count", static_cast<std::uint64_t>(request->sources.size())},
        {"loaded_container_count", static_cast<std::uint64_t>(loaded.size())},
        {"load_error_count", static_cast<std::uint64_t>(load_error_count)},
        {"relationship_load_error_count", static_cast<std::uint64_t>(load_error_count)},
        {"object_count", static_cast<std::uint64_t>(inventory.size())},
        {"validation_issue_count", static_cast<std::uint64_t>(validation_issues.size())},
        {"validation_failed", validation_failed},
        {"relationship_count", static_cast<std::uint64_t>(relationships.size())},
        {"ambiguous_relationship_count", ambiguous},
        {"wave_smoke_decoded", wave_decoded},
        {"wave_smoke_errors", static_cast<std::uint64_t>(wave_issues.size())},
    };
    if (auto written =
            axk::write_report_object(*destination / "corpus_audit_summary.json", summary, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "corpus_audit_summary.json")}));
    }
    if (auto written = axk::write_report_json(*destination / "input_manifest.json", manifest, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "input_manifest.json")}));
    }
    std::vector<axk::ReportSchemaManifest> schemas;
    axk::ReportSchemaOptions options;
    options.source_command = "axklib";
    options.library_version = std::string{axk::version()};
    const std::array summary_rows{summary};
    auto summary_schema = axk::make_report_schema("corpus_audit_summary", summary_rows, options);
    if (auto written = axk::write_report_schema(*destination / "_schemas" / "corpus_audit_summary.schema.json",
                                                summary_schema, request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(),
                       {request->destination.root_id,
                        child_reference_path(request->destination, "_schemas/corpus_audit_summary.schema.json")}));
    }
    schemas.push_back(std::move(summary_schema));
    for (const auto &[name, rows] :
         std::initializer_list<std::pair<std::string_view, const std::vector<axk::ReportRow> &>>{
             {"input_manifest", manifest},
             {"inventory_objects", inventory},
             {"validation_issues", validation_issues},
             {"relationships", relationships},
             {"wave_smoke_issues", wave_issues}}) {
        auto schema = write_csv_schema(*destination, request->destination, std::string{name}, rows, request->overwrite);
        if (!schema)
            return std::unexpected(schema.error());
        schemas.push_back(std::move(*schema));
    }
    if (auto written = axk::write_report_schema_index(*destination / "_schemas" / "schema_index.json", schemas,
                                                      request->overwrite);
        !written) {
        return std::unexpected(
            core_error(written.error(), {request->destination.root_id,
                                         child_reference_path(request->destination, "_schemas/schema_index.json")}));
    }

    auto artifacts = Json::array();
    for (const auto path : {"corpus_audit_summary.json", "input_manifest.csv", "input_manifest.json",
                            "inventory_objects.csv", "validation_issues.csv", "relationships.csv",
                            "wave_smoke_issues.csv", "_schemas/corpus_audit_summary.schema.json",
                            "_schemas/input_manifest.schema.json", "_schemas/inventory_objects.schema.json",
                            "_schemas/validation_issues.schema.json", "_schemas/relationships.schema.json",
                            "_schemas/wave_smoke_issues.schema.json", "_schemas/schema_index.json"}) {
        artifacts.push_back({{"rootId", request->destination.root_id},
                             {"relativePath", child_reference_path(request->destination, path)}});
    }
    if (context.progress != nullptr) {
        context.progress->report({axk::ProgressPhase::writing, request->sources.size(), request->sources.size(),
                                  "corpus audit", std::nullopt});
    }
    return Json{{"operationId", "corpus.audit"},         {"sourceCount", request->sources.size()},
                {"loadedCount", loaded.size()},          {"failedCount", load_error_count},
                {"objectCount", inventory.size()},       {"validationIssueCount", validation_issues.size()},
                {"validationFailed", validation_failed}, {"relationshipCount", relationships.size()},
                {"waveSmokeDecoded", wave_decoded},      {"waveSmokeErrorCount", wave_issues.size()},
                {"artifacts", std::move(artifacts)}};
}

} // namespace

axk::app::Result<void> axk::app::bind_file_operations(OperationRegistry &registry, const Sandbox &sandbox) {
    if (!registry.is_implemented("report.info")) {
        auto bound =
            registry.bind("report.info", [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                return execute_info(sandbox, request, context);
            });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.objects")) {
        auto bound =
            registry.bind("report.objects", [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                return execute_objects(sandbox, request, context);
            });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.inventory")) {
        auto bound = registry.bind("report.inventory",
                                   [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                                       return execute_inventory(sandbox, request, context);
                                   });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.orphans")) {
        auto bound =
            registry.bind("report.orphans", [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                return execute_orphans(sandbox, request, context);
            });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.coverage")) {
        auto bound = registry.bind("report.coverage",
                                   [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                                       return execute_coverage(sandbox, request, context);
                                   });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("report.relationships")) {
        auto bound = registry.bind("report.relationships",
                                   [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                                       return execute_relationships(sandbox, request, context);
                                   });
        if (!bound)
            return std::unexpected(bound.error());
    }
    if (!registry.is_implemented("corpus.audit")) {
        auto bound =
            registry.bind("corpus.audit", [&sandbox](const nlohmann::json &request, const OperationContext &context) {
                return execute_corpus_audit(sandbox, request, context);
            });
        if (!bound)
            return std::unexpected(bound.error());
    }
    return {};
}
