#include "axklib/application/system_service.hpp"

#include "axklib/version.hpp"

void axk::app::to_json(nlohmann::json &output, const EmptyRequest &) { output = nlohmann::json::object(); }

void axk::app::from_json(const nlohmann::json &input, EmptyRequest &) {
    if (!input.is_object() || !input.empty())
        throw nlohmann::json::type_error::create(302, "expected an empty object", &input);
}

void axk::app::to_json(nlohmann::json &output, const VersionResponse &response) {
    output = {
        {"semanticVersion", response.semantic_version},
        {"sourceIdentity", response.source_identity},
        {"packageBasename", response.package_basename},
        {"gitShaShort", response.git_sha_short},
        {"gitRef", response.git_ref},
        {"isDirty", response.is_dirty},
        {"isTaggedRelease", response.is_tagged_release},
        {"apiVersion", response.api_version},
        {"eventSchemaVersion", response.event_schema_version},
    };
}

void axk::app::from_json(const nlohmann::json &input, VersionResponse &response) {
    response = {.semantic_version = input.at("semanticVersion").get<std::string>(),
                .source_identity = input.at("sourceIdentity").get<std::string>(),
                .package_basename = input.at("packageBasename").get<std::string>(),
                .git_sha_short = input.at("gitShaShort").get<std::string>(),
                .git_ref = input.at("gitRef").get<std::string>(),
                .is_dirty = input.at("isDirty").get<bool>(),
                .is_tagged_release = input.at("isTaggedRelease").get<bool>(),
                .api_version = input.at("apiVersion").get<std::string>(),
                .event_schema_version = input.at("eventSchemaVersion").get<std::string>()};
}

axk::app::Result<axk::app::VersionResponse> axk::app::system_version(const EmptyRequest &, const OperationContext &) {
    const auto build = current_build_info();
    return VersionResponse{
        std::string{version()},
        build.source_identity,
        build.package_basename,
        build.git_sha_short,
        build.is_tagged_release ? build.git_tag : build.git_branch,
        build.is_dirty,
        build.is_tagged_release,
    };
}

axk::app::Result<axk::app::VersionResponse> axk::app::registered_system_version(const OperationRegistry &registry,
                                                                                const OperationContext &context) {
    auto result = registry.invoke("system.version", nlohmann::json::object(), context);
    if (!result)
        return std::unexpected(result.error());
    try {
        return result->get<VersionResponse>();
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(Error{"invalid_service_result", "system.version result does not match its schema"});
    }
}
