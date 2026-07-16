#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/application/operation_registry.hpp"

namespace axk::server {

class OpenApiValidator {
  public:
    explicit OpenApiValidator(nlohmann::json document);
    ~OpenApiValidator();

    OpenApiValidator(const OpenApiValidator &) = delete;
    OpenApiValidator &operator=(const OpenApiValidator &) = delete;
    OpenApiValidator(OpenApiValidator &&) noexcept;
    OpenApiValidator &operator=(OpenApiValidator &&) noexcept;

    [[nodiscard]] app::Result<void> validate(std::string_view schema_name, const nlohmann::json &value) const;
    [[nodiscard]] nlohmann::json application_value(std::string_view schema_name,
                                                   const nlohmann::json &wire_value) const;
    [[nodiscard]] nlohmann::json wire_value(std::string_view schema_name,
                                            const nlohmann::json &application_value) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] nlohmann::json build_openapi_document(std::string_view base_document,
                                                    const app::OperationRegistry &registry);
[[nodiscard]] app::Result<void> validate_openapi_value(const nlohmann::json &document, std::string_view schema_name,
                                                       const nlohmann::json &value);
[[nodiscard]] app::Result<void> validate_openapi_schema(const nlohmann::json &document, const nlohmann::json &schema,
                                                        const nlohmann::json &value);

} // namespace axk::server
