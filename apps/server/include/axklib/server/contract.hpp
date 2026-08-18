#pragma once

#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/application/operation_registry.hpp"

namespace axk::server {

class OpenApiValidator {
  public:
    OpenApiValidator() = default;

    [[nodiscard]] app::Result<void> validate(std::string_view schema_name, const nlohmann::json &value) const;
    [[nodiscard]] nlohmann::json application_value(std::string_view schema_name,
                                                   const nlohmann::json &wire_value) const;
    [[nodiscard]] nlohmann::json wire_value(std::string_view schema_name,
                                            const nlohmann::json &application_value) const;
};

[[nodiscard]] nlohmann::json build_openapi_document(std::string_view base_document,
                                                    const app::OperationRegistry &registry);
} // namespace axk::server
