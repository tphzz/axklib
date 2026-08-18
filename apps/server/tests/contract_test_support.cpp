#include "contract_test_support.hpp"

#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json-schema.hpp>

namespace {

nlohmann::json validation_root(const nlohmann::json &document, const nlohmann::json &schema) {
    auto root = schema;
    root["$schema"] = "http://json-schema.org/draft-07/schema#";
    root["components"] = document.at("components");
    return root;
}

axk::app::Result<void> validate_value(const nlohmann::json &document, const nlohmann::json &schema,
                                      const nlohmann::json &value) {
    try {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(validation_root(document, schema));
        static_cast<void>(validator.validate(value));
    } catch (const std::exception &) {
        return std::unexpected(
            axk::app::Error{"invalid_request", "request body does not match the declared OpenAPI schema"});
    }
    return {};
}

} // namespace

struct axk::server::OracleOpenApiValidator::Impl {
    explicit Impl(nlohmann::json value) : document(std::move(value)) {
        for (const auto &[name, schema] : document.at("components").at("schemas").items()) {
            auto validator = std::make_unique<nlohmann::json_schema::json_validator>();
            validator->set_root_schema(validation_root(document, schema));
            validators.emplace(name, std::move(validator));
        }
    }

    nlohmann::json document;
    std::map<std::string, std::unique_ptr<nlohmann::json_schema::json_validator>, std::less<>> validators;
};

axk::server::OracleOpenApiValidator::OracleOpenApiValidator(nlohmann::json document)
    : impl_(std::make_unique<Impl>(std::move(document))) {}

axk::server::OracleOpenApiValidator::~OracleOpenApiValidator() = default;

bool axk::server::OracleOpenApiValidator::validate(std::string_view schema_name, const nlohmann::json &value) const {
    const auto found = impl_->validators.find(schema_name);
    if (found == impl_->validators.end())
        return false;
    try {
        static_cast<void>(found->second->validate(value));
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

axk::app::Result<void> axk::server::validate_openapi_value(const nlohmann::json &document, std::string_view schema_name,
                                                           const nlohmann::json &value) {
    try {
        return validate_value(document, {{"$ref", "#/components/schemas/" + std::string{schema_name}}}, value);
    } catch (const std::exception &) {
        return std::unexpected(app::Error{"contract_error", "OpenAPI schema is not available"});
    }
}

axk::app::Result<void> axk::server::validate_openapi_schema(const nlohmann::json &document,
                                                            const nlohmann::json &schema, const nlohmann::json &value) {
    return validate_value(document, schema, value);
}
