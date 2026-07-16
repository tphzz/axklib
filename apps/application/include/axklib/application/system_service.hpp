#pragma once

#include <string>
#include <vector>

#include "axklib/application/contracts.hpp"
#include "axklib/application/operation_registry.hpp"

namespace axk::app {

struct EmptyRequest {};

struct VersionResponse {
    std::string semantic_version;
    std::string source_identity;
    std::string package_basename;
    std::string git_sha_short;
    std::string git_ref;
    bool is_dirty{};
    bool is_tagged_release{};
    std::string api_version{"v1"};
    std::string event_schema_version{"1"};
};

void to_json(nlohmann::json &output, const EmptyRequest &request);
void from_json(const nlohmann::json &input, EmptyRequest &request);
void to_json(nlohmann::json &output, const VersionResponse &response);
void from_json(const nlohmann::json &input, VersionResponse &response);

[[nodiscard]] Result<VersionResponse> system_version(const EmptyRequest &request, const OperationContext &context);
[[nodiscard]] Result<VersionResponse> registered_system_version(const OperationRegistry &registry,
                                                                const OperationContext &context = {});

} // namespace axk::app
