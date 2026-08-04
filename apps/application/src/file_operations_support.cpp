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

axk::app::Error operation_error(std::string code, std::string message, std::optional<std::string> relative_path) {
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

axk::app::Error image_source_error(const axk::Error &error, const axk::app::ImageSourceRef &source) {
    return core_error(error, axk::app::FileRef{source.root_id, source.relative_path});
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
            return std::unexpected(
                operation_error("invalid_request", "sources must contain 1 to 1024 image source values"));
        }
        for (const auto &source : input.at("sources")) {
            const auto kind = source.at("kind").get<std::string>();
            if (kind == "FILE") {
                const auto &file = source.at("file");
                result.sources.push_back({file.at("rootId").get<std::string>(),
                                          file.at("relativePath").get<std::string>(), axk::app::ImageSourceKind::file});
            } else if (kind == "AXK_OBJECT_DIRECTORY") {
                const auto &directory = source.at("directory");
                result.sources.push_back({directory.at("rootId").get<std::string>(),
                                          directory.at("relativePath").get<std::string>(),
                                          axk::app::ImageSourceKind::axk_object_directory});
            } else {
                return std::unexpected(operation_error("invalid_request", "info source kind is unsupported"));
            }
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
    case axk::MediaKind::fat12_floppy_set:
        return "fat12_floppy_set";
    case axk::MediaKind::iso9660:
        return "iso";
    case axk::MediaKind::standalone_object:
        return "standalone_object";
    case axk::MediaKind::axk_object_directory:
        return "axk_object_directory";
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

axk::app::Result<LoadedSource> load_source(const axk::app::Sandbox &sandbox, const axk::app::FileRef &source,
                                           bool include_default_programs, const axk::app::OperationContext &context,
                                           axk::MediaObjectReadMode read_mode) {
    const auto file = sandbox.open_file(source);
    if (!file)
        return std::unexpected(file.error());
    auto media = axk::open_media(file->reader, std::filesystem::path{file->filename}, context.cancellation);
    if (!media)
        return std::unexpected(core_error(media.error(), source));
    auto inventory = axk::build_media_inventory(*media, read_mode, 64U * 1024U * 1024U, context.cancellation);
    if (!inventory)
        return std::unexpected(core_error(inventory.error(), source));
    auto graph = axk::build_relationship_graph(inventory->catalog);
    auto tree = axk::build_content_tree(*media, inventory->catalog, graph, include_default_programs);
    return LoadedSource{{source.root_id, source.relative_path, axk::app::ImageSourceKind::file},
                        std::move(*media),
                        std::move(*inventory),
                        std::move(graph),
                        std::move(tree)};
}

std::expected<LoadedSource, InfoLoadFailure> load_info_source(const axk::app::Sandbox &sandbox,
                                                              const axk::app::ImageSourceRef &source,
                                                              bool include_default_programs,
                                                              const axk::app::OperationContext &context) {
    std::optional<axk::MediaContainer> media;
    if (source.kind == axk::app::ImageSourceKind::file) {
        const auto file = sandbox.open_file({source.root_id, source.relative_path});
        if (!file) {
            return std::unexpected(
                InfoLoadFailure{.error = file.error(),
                                .error_code = static_cast<std::uint64_t>(axk::ErrorCode::io_open_failed),
                                .original_exception = "axk::Error"});
        }
        auto opened = axk::open_media(file->reader, std::filesystem::path{file->filename}, context.cancellation);
        if (!opened) {
            return std::unexpected(InfoLoadFailure{.error = image_source_error(opened.error(), source),
                                                   .error_code = static_cast<std::uint64_t>(opened.error().code),
                                                   .original_exception = "axk::Error"});
        }
        media.emplace(std::move(*opened));
    } else {
        auto tree = sandbox.open_tree({source.root_id, source.relative_path},
                                      {.maximum_entries = axk::AxkObjectDirectory::maximum_entries,
                                       .maximum_total_file_bytes = axk::AxkObjectDirectory::maximum_payload_bytes,
                                       .maximum_depth = axk::AxkObjectDirectory::maximum_depth,
                                       .maximum_path_bytes = 64U * 1024U});
        if (!tree) {
            return std::unexpected(
                InfoLoadFailure{.error = tree.error(),
                                .error_code = static_cast<std::uint64_t>(axk::ErrorCode::io_open_failed),
                                .original_exception = "axk::Error"});
        }
        std::vector<axk::AxkObjectDirectoryEntry> entries;
        std::vector<std::function<axk::app::Result<void>()>> verifiers;
        for (std::size_t index = 0U; index < tree->entries().size(); ++index) {
            const auto &entry = tree->entries()[index];
            if (entry.kind != axk::app::SandboxTreeEntryKind::file)
                continue;
            auto file = tree->open_file(index);
            if (!file) {
                return std::unexpected(
                    InfoLoadFailure{.error = file.error(),
                                    .error_code = static_cast<std::uint64_t>(axk::ErrorCode::io_open_failed),
                                    .original_exception = "axk::Error"});
            }
            entries.push_back({entry.relative_path, file->reader});
            verifiers.push_back(std::move(file->verify_unchanged));
        }
        auto opened = axk::AxkObjectDirectory::open(std::move(entries), source.relative_path, context.cancellation);
        if (!opened) {
            return std::unexpected(InfoLoadFailure{.error = image_source_error(opened.error(), source),
                                                   .error_code = static_cast<std::uint64_t>(opened.error().code),
                                                   .original_exception = "axk::Error"});
        }
        for (const auto &verify : verifiers) {
            if (const auto unchanged = verify(); !unchanged) {
                return std::unexpected(
                    InfoLoadFailure{.error = unchanged.error(),
                                    .error_code = static_cast<std::uint64_t>(axk::ErrorCode::io_read_failed),
                                    .original_exception = "axk::Error"});
            }
        }
        media.emplace(std::move(*opened));
    }
    auto inventory = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata, 64U * 1024U * 1024U,
                                                context.cancellation);
    if (!inventory) {
        return std::unexpected(InfoLoadFailure{.error = image_source_error(inventory.error(), source),
                                               .error_code = static_cast<std::uint64_t>(inventory.error().code),
                                               .original_exception = "axk::Error"});
    }
    auto graph = axk::build_relationship_graph(inventory->catalog);
    auto tree = axk::build_content_tree(*media, inventory->catalog, graph, include_default_programs);
    return LoadedSource{source, std::move(*media), std::move(*inventory), std::move(graph), std::move(tree)};
}

std::expected<LoadedSource, InfoLoadFailure> load_info_source(const axk::app::Sandbox &sandbox,
                                                              const axk::app::FileRef &source,
                                                              bool include_default_programs,
                                                              const axk::app::OperationContext &context) {
    return load_info_source(sandbox, {source.root_id, source.relative_path, axk::app::ImageSourceKind::file},
                            include_default_programs, context);
}

std::string source_display_path(const axk::app::FileRef &source, const axk::app::OperationContext &context) {
    if (context.display_path) {
        const auto display = context.display_path(source);
        if (!display.empty())
            return display;
    }
    return source.relative_path;
}

std::string source_display_path(const axk::app::ImageSourceRef &source, const axk::app::OperationContext &context) {
    return source_display_path(axk::app::FileRef{source.root_id, source.relative_path}, context);
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
    if (source.media.kind() == axk::MediaKind::axk_object_directory)
        return std::format("{}:axk-object-directory:{}", filename, object->logical_path);
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
    if (source.media.kind() == axk::MediaKind::axk_object_directory)
        return std::format("{}:axk-object-directory", display_path);
    const auto object = std::ranges::find(source.inventory.objects, item.key, &axk::MediaObjectDescriptor::key);
    return object == source.inventory.objects.end() ? std::format("{}:iso", display_path)
                                                    : std::format("{}:{}", display_path, object->scope_key);
}

} // namespace axk::app::file_operations_internal
