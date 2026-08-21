#pragma once

#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

#include "axklib/application/contracts.hpp"

namespace axk::server {

class OracleOpenApiValidator {
  public:
    explicit OracleOpenApiValidator(nlohmann::json document);
    ~OracleOpenApiValidator();

    OracleOpenApiValidator(const OracleOpenApiValidator &) = delete;
    OracleOpenApiValidator &operator=(const OracleOpenApiValidator &) = delete;

    [[nodiscard]] bool validate(std::string_view schema_name, const nlohmann::json &value) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] app::Result<void> validate_openapi_value(const nlohmann::json &document, std::string_view schema_name,
                                                       const nlohmann::json &value);
[[nodiscard]] app::Result<void> validate_openapi_schema(const nlohmann::json &document, const nlohmann::json &schema,
                                                        const nlohmann::json &value);

} // namespace axk::server
