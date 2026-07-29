#include "validation_operations_internal.hpp"

#include <array>
#include <filesystem>
#include <format>
#include <ranges>
#include <string>
#include <utility>

#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"

namespace axk::app::validation_operations_internal {

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
    return {error.code == axk::ErrorCode::operation_cancelled ? "operation_cancelled" : "validation_failed",
            error.message, std::move(context)};
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

} // namespace axk::app::validation_operations_internal
