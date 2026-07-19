#include "axklib/server/contract.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json-schema.hpp>

namespace {

using Json = nlohmann::json;

struct RouteKey {
    axk::app::HttpMethod method;
    std::string route;

    friend auto operator<=>(const RouteKey &, const RouteKey &) = default;
};

Json schema_reference(std::string_view name) { return {{"$ref", "#/components/schemas/" + std::string{name}}}; }

constexpr std::string_view component_schema_prefix{"#/components/schemas/"};

const Json &resolve_schema_reference(const Json &document, const Json &schema) {
    const Json *resolved = &schema;
    for (std::size_t depth = 0U; depth < 32U; ++depth) {
        const auto reference = resolved->find("$ref");
        if (reference == resolved->end() || !reference->is_string())
            return *resolved;
        const auto value = reference->get<std::string>();
        if (!value.starts_with(component_schema_prefix))
            throw std::runtime_error{"OpenAPI contract uses an unsupported schema reference"};
        const auto name = std::string_view{value}.substr(component_schema_prefix.size());
        if (name.find('/') != std::string_view::npos)
            return *resolved;
        resolved = &document.at("components").at("schemas").at(name);
    }
    throw std::runtime_error{"OpenAPI contract contains a schema reference cycle"};
}

const Json &resolve_any_local_reference(const Json &document, const Json &schema) {
    const Json *resolved = &schema;
    for (std::size_t depth = 0U; depth < 32U; ++depth) {
        const auto reference = resolved->find("$ref");
        if (reference == resolved->end() || !reference->is_string())
            return *resolved;
        const auto value = reference->get<std::string>();
        if (!value.starts_with("#/"))
            throw std::runtime_error{"OpenAPI contract uses an unsupported schema reference"};
        resolved = &document.at(Json::json_pointer{value.substr(1U)});
    }
    throw std::runtime_error{"OpenAPI contract contains a schema reference cycle"};
}

void merge_example(Json &target, Json source) {
    if (target.is_object() && source.is_object()) {
        target.update(std::move(source));
        return;
    }
    target = std::move(source);
}

Json schema_example(const Json &document, const Json &schema, std::size_t depth = 0U) {
    if (depth > 64U)
        throw std::runtime_error{"OpenAPI schema example recursion is too deep"};
    const auto &resolved = resolve_any_local_reference(document, schema);
    if (resolved.contains("example"))
        return resolved.at("example");
    if (resolved.contains("const"))
        return resolved.at("const");
    if (resolved.contains("enum") && resolved.at("enum").is_array() && !resolved.at("enum").empty())
        return resolved.at("enum").front();
    if (resolved.contains("default"))
        return resolved.at("default");

    std::string type;
    if (const auto type_value = resolved.find("type"); type_value != resolved.end()) {
        if (type_value->is_string()) {
            type = type_value->get<std::string>();
        } else if (type_value->is_array()) {
            const auto selected = std::ranges::find_if(*type_value, [](const Json &candidate) {
                return candidate.is_string() && candidate.get_ref<const std::string &>() != "null";
            });
            if (selected != type_value->end())
                type = selected->get<std::string>();
        }
    }

    Json result;
    const auto add_required_properties = [&](const Json &required_schema, Json &object) {
        const auto required = required_schema.find("required");
        const auto properties = resolved.find("properties");
        if (required == required_schema.end() || !required->is_array() || properties == resolved.end() ||
            !properties->is_object()) {
            return;
        }
        for (const auto &name : *required) {
            if (!name.is_string() || object.contains(name.get_ref<const std::string &>()) ||
                !properties->contains(name.get_ref<const std::string &>())) {
                continue;
            }
            object[name.get_ref<const std::string &>()] =
                schema_example(document, properties->at(name.get_ref<const std::string &>()), depth + 1U);
        }
    };

    if (type == "object" || resolved.contains("properties")) {
        result = Json::object();
        add_required_properties(resolved, result);
    } else if (type == "array") {
        result = Json::array();
        const auto minimum = resolved.value("minItems", 0U);
        if (const auto items = resolved.find("items"); items != resolved.end()) {
            for (std::size_t index = 0U; index < minimum; ++index)
                result.push_back(schema_example(document, *items, depth + 1U));
        }
    } else if (type == "string") {
        const auto pattern = resolved.value("pattern", std::string{});
        if (pattern == "^[0-9a-f]{64}$")
            return std::string(64U, '0');
        if (pattern == "^\\.axk[a-z]+$")
            return ".axkvol";
        if (pattern == "^[A-Za-z0-9_.-]+$")
            return "request-1";
        return std::string(std::max<std::size_t>(1U, resolved.value("minLength", 0U)), 'x');
    } else if (type == "integer" || type == "number") {
        return resolved.value("minimum", 0);
    } else if (type == "boolean") {
        return false;
    } else if (type == "null") {
        return nullptr;
    } else {
        result = Json::object();
    }

    for (const auto keyword : {"allOf", "oneOf", "anyOf"}) {
        const auto variants = resolved.find(keyword);
        if (variants == resolved.end() || !variants->is_array() || variants->empty())
            continue;
        if (keyword == std::string_view{"allOf"}) {
            for (const auto &variant : *variants)
                merge_example(result, schema_example(document, variant, depth + 1U));
        } else {
            merge_example(result, schema_example(document, variants->front(), depth + 1U));
            if (result.is_object())
                add_required_properties(variants->front(), result);
        }
    }
    return result;
}

Json validation_root(const Json &document, const Json &schema) {
    auto root = schema;
    root["$schema"] = "http://json-schema.org/draft-07/schema#";
    root["components"] = document.at("components");
    return root;
}

axk::app::Result<void> validate_value(const Json &document, const Json &schema, const Json &value) {
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

Json translate_value(const Json &document, const Json &schema, const Json &value, bool to_application) {
    const auto &resolved = resolve_schema_reference(document, schema);
    if (const auto mapping = resolved.find("x-axklib-application-enum");
        mapping != resolved.end() && mapping->is_object() && value.is_string()) {
        const auto text = value.get<std::string>();
        if (to_application) {
            if (const auto found = mapping->find(text); found != mapping->end() && found->is_string())
                return *found;
        } else {
            for (const auto &[wire, application] : mapping->items()) {
                if (application.is_string() && application.get_ref<const std::string &>() == text)
                    return wire;
            }
        }
        return value;
    }

    auto result = value;
    for (const auto keyword : {"allOf", "oneOf", "anyOf"}) {
        const auto variants = resolved.find(keyword);
        if (variants == resolved.end() || !variants->is_array())
            continue;
        for (const auto &variant : *variants)
            result = translate_value(document, variant, result, to_application);
    }
    if (result.is_object()) {
        const auto properties = resolved.find("properties");
        if (properties != resolved.end() && properties->is_object()) {
            for (const auto &[name, property_schema] : properties->items()) {
                if (result.contains(name))
                    result[name] = translate_value(document, property_schema, result.at(name), to_application);
            }
        }
    } else if (result.is_array()) {
        const auto items = resolved.find("items");
        if (items != resolved.end()) {
            for (auto &item : result)
                item = translate_value(document, *items, item, to_application);
        }
    }
    return result;
}

void require_schema(const Json &document, std::string_view name) {
    auto &schemas = document["components"]["schemas"];
    if (!schemas.contains(name))
        throw std::runtime_error{"OpenAPI contract is missing canonical schema " + std::string{name}};
}

Json dispatched_variant_schema(const Json &document, const axk::app::OperationDescriptor &descriptor) {
    auto schema = resolve_schema_reference(document, schema_reference(descriptor.request_schema));
    if (schema.value("type", std::string{}) != "object" ||
        (schema.contains("properties") && !schema.at("properties").is_object())) {
        throw std::runtime_error{"shared operation routes require an object request schema"};
    }
    schema["type"] = "object";
    schema["properties"]["operationId"] = {{"type", "string"}, {"const", descriptor.id}};
    if (!schema.contains("required"))
        schema["required"] = Json::array();
    if (!schema.at("required").is_array())
        throw std::runtime_error{"OpenAPI request schema required field must be an array"};
    if (std::ranges::find(schema["required"], "operationId") == schema["required"].end())
        schema["required"].push_back("operationId");
    return schema;
}

Json dispatch_request_schema(const Json &document,
                             const std::vector<const axk::app::OperationDescriptor *> &descriptors) {
    if (descriptors.size() == 1U)
        return schema_reference(descriptors.front()->request_schema);
    Json variants = Json::array();
    for (const auto *descriptor : descriptors)
        variants.push_back(dispatched_variant_schema(document, *descriptor));
    return {{"oneOf", std::move(variants)}};
}

Json response_schema(const axk::app::OperationDescriptor &descriptor) {
    if (descriptor.mode == axk::app::ExecutionMode::job)
        return schema_reference("JobResponse");
    return {{"type", "object"},
            {"required", Json::array({"data", "meta"})},
            {"additionalProperties", false},
            {"properties",
             {{"data", schema_reference(descriptor.result_schema)}, {"meta", schema_reference("ResponseMeta")}}}};
}

std::string dispatch_operation_id(std::string_view route) {
    constexpr std::string_view api_prefix{"/api/v1/"};
    auto result = std::string{"operations.dispatch."};
    for (const auto character : route.substr(api_prefix.size())) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
            result.push_back(character);
        } else if (!result.ends_with('.')) {
            result.push_back('.');
        }
    }
    if (result.ends_with('.'))
        result.pop_back();
    return result;
}

Json error_response(std::string_view description) {
    return {{"description", description},
            {"content",
             {{"application/json",
               {{"schema", schema_reference("ErrorResponse")},
                {"examples",
                 {{"default",
                   {{"value",
                     {{"error",
                       {{"code", "invalid_request"},
                        {"message", "request could not be processed"},
                        {"context", Json::object()},
                        {"requestId", "request-1"},
                        {"retryable", false}}}}}}}}}}}}}};
}

void add_operation_examples(const Json &document, Json &operation) {
    if (const auto request_body = operation.find("requestBody"); request_body != operation.end()) {
        auto &content = operation["requestBody"]["content"]["application/json"];
        content["examples"]["default"]["value"] = schema_example(document, content.at("schema"));
    }
    for (auto &[status, response] : operation["responses"].items()) {
        static_cast<void>(status);
        if (!response.contains("content") || !response["content"].contains("application/json"))
            continue;
        auto &content = response["content"]["application/json"];
        if (!content.contains("schema") || content.contains("examples"))
            continue;
        auto success = schema_example(document, content.at("schema"));
        if (success.is_object() && success.contains("meta")) {
            success["meta"]["requestId"] = "request-1";
            auto warning = success;
            warning["meta"]["warnings"] = Json::array({{{"code", "example_warning"},
                                                        {"message", "operation completed with a warning"},
                                                        {"context", Json::object()}}});
            content["examples"] = {{"success", {{"value", std::move(success)}}},
                                   {"warning", {{"value", std::move(warning)}}}};
        }
    }
}

void add_request_id_headers(Json &document) {
    document["components"]["headers"]["XRequestId"] = {
        {"description", "Request correlation identifier"},
        {"schema", {{"type", "string"}, {"minLength", 1}, {"maxLength", 96}, {"pattern", "^[A-Za-z0-9_.-]+$"}}}};
    for (auto &[path, path_item] : document["paths"].items()) {
        static_cast<void>(path);
        for (auto &[method, operation] : path_item.items()) {
            static_cast<void>(method);
            if (!operation.is_object() || !operation.contains("responses"))
                continue;
            for (auto &[status, response] : operation["responses"].items()) {
                static_cast<void>(status);
                response["headers"]["X-Request-Id"] = {{"$ref", "#/components/headers/XRequestId"}};
            }
        }
    }
}

void add_infrastructure_error_responses(Json &document) {
    for (auto &[path, path_item] : document["paths"].items()) {
        static_cast<void>(path);
        for (auto &[method, operation] : path_item.items()) {
            static_cast<void>(method);
            if (!operation.is_object() || !operation.contains("responses") ||
                operation.contains("x-axklib-operation-ids")) {
                continue;
            }
            operation["responses"]["default"] = error_response("Request could not be completed");
            for (auto &[status, response] : operation["responses"].items()) {
                if (status.size() != 3U || (status.front() != '4' && status.front() != '5') || status == "416" ||
                    status == "503") {
                    continue;
                }
                if (!response.contains("content"))
                    response["content"] = error_response(response.value("description", "Request failed")).at("content");
            }
        }
    }
}

} // namespace

struct axk::server::OpenApiValidator::Impl {
    explicit Impl(Json value) : document(std::move(value)) {
        for (const auto &[name, schema] : document.at("components").at("schemas").items()) {
            auto validator = std::make_unique<nlohmann::json_schema::json_validator>();
            validator->set_root_schema(validation_root(document, schema));
            validators.emplace(name, std::move(validator));
        }
    }

    Json document;
    std::map<std::string, std::unique_ptr<nlohmann::json_schema::json_validator>, std::less<>> validators;
};

axk::server::OpenApiValidator::OpenApiValidator(nlohmann::json document)
    : impl_(std::make_unique<Impl>(std::move(document))) {}

axk::server::OpenApiValidator::~OpenApiValidator() = default;
axk::server::OpenApiValidator::OpenApiValidator(OpenApiValidator &&) noexcept = default;
axk::server::OpenApiValidator &axk::server::OpenApiValidator::operator=(OpenApiValidator &&) noexcept = default;

axk::app::Result<void> axk::server::OpenApiValidator::validate(std::string_view schema_name,
                                                               const nlohmann::json &value) const {
    const auto found = impl_->validators.find(schema_name);
    if (found == impl_->validators.end())
        return std::unexpected(app::Error{"contract_error", "OpenAPI schema is not available"});
    try {
        static_cast<void>(found->second->validate(value));
    } catch (const std::exception &) {
        return std::unexpected(app::Error{"invalid_request", "request body does not match its declared schema"});
    }
    return {};
}

nlohmann::json axk::server::OpenApiValidator::application_value(std::string_view schema_name,
                                                                const nlohmann::json &wire_value) const {
    const auto &schema = impl_->document.at("components").at("schemas").at(schema_name);
    return translate_value(impl_->document, schema, wire_value, true);
}

nlohmann::json axk::server::OpenApiValidator::wire_value(std::string_view schema_name,
                                                         const nlohmann::json &application_value) const {
    const auto &schema = impl_->document.at("components").at("schemas").at(schema_name);
    return translate_value(impl_->document, schema, application_value, false);
}

nlohmann::json axk::server::build_openapi_document(std::string_view base_document,
                                                   const app::OperationRegistry &registry) {
    auto document = Json::parse(base_document);
    std::map<RouteKey, std::vector<const app::OperationDescriptor *>> routes;
    const auto entries = registry.entries();
    for (const auto &entry : entries) {
        routes[{entry.descriptor.method, entry.descriptor.route}].push_back(&entry.descriptor);
        require_schema(document, entry.descriptor.request_schema);
        require_schema(document, entry.descriptor.result_schema);
    }

    for (const auto &[key, descriptors] : routes) {
        const auto path = key.route.substr(std::string_view{"/api/v1"}.size());
        const auto method = std::string{http_method_name(key.method) == "GET" ? "get" : "post"};
        Json operation_ids = Json::array();
        Json operation_descriptors = Json::array();
        for (const auto *descriptor : descriptors) {
            operation_ids.push_back(descriptor->id);
            operation_descriptors.push_back({
                {"id", descriptor->id},
                {"cliCommand", descriptor->cli_command.empty() ? Json(nullptr) : Json(descriptor->cli_command)},
                {"cliParity", descriptor->cli_parity},
                {"mode", app::execution_mode_name(descriptor->mode)},
                {"operationClass", app::operation_class_name(descriptor->operation_class)},
                {"requiresIdempotency", descriptor->requires_idempotency},
                {"variant", descriptor->variant.empty() ? Json(nullptr) : Json(descriptor->variant)},
                {"requestSchema", descriptor->request_schema},
                {"resultSchema", descriptor->result_schema},
            });
        }

        const auto &primary = *descriptors.front();
        Json operation{
            {"operationId", descriptors.size() == 1U ? Json(primary.id) : Json(dispatch_operation_id(key.route))},
            {"x-axklib-operation-ids", std::move(operation_ids)},
            {"x-axklib-operation-descriptors", std::move(operation_descriptors)},
            {"responses",
             {{primary.mode == app::ExecutionMode::job ? "202" : "200",
               {{"description", primary.mode == app::ExecutionMode::job ? "Job accepted" : "Operation completed"},
                {"content", {{"application/json", {{"schema", response_schema(primary)}}}}}}}}}};
        operation["responses"]["400"] = error_response("Malformed or schema-invalid request");
        operation["responses"]["401"] = error_response("Authentication is required");
        operation["responses"]["413"] = error_response("Configured request limit exceeded");
        operation["responses"]["422"] = error_response("Unsupported or invalid domain request");
        operation["responses"]["429"] = error_response("Transient server capacity exhausted");
        operation["responses"]["500"] = error_response("Contained internal failure");
        if (key.method == app::HttpMethod::post) {
            operation["requestBody"] = {
                {"required", true},
                {"content", {{"application/json", {{"schema", dispatch_request_schema(document, descriptors)}}}}}};
        }
        add_operation_examples(document, operation);
        document["paths"][path][method] = std::move(operation);
    }
    for (auto &[path, path_item] : document["paths"].items()) {
        static_cast<void>(path);
        for (auto &[method, operation] : path_item.items()) {
            static_cast<void>(method);
            if (!operation.is_object() || !operation.contains("x-axklib-operation-ids"))
                continue;
            operation["responses"]["403"] = error_response("Authenticated caller is not authorized");
            operation["responses"]["404"] = error_response("Referenced resource does not exist");
            operation["responses"]["409"] = error_response("Request conflicts with current state");
        }
    }
    add_infrastructure_error_responses(document);
    add_request_id_headers(document);
    return document;
}

axk::app::Result<void> axk::server::validate_openapi_value(const nlohmann::json &document, std::string_view schema_name,
                                                           const nlohmann::json &value) {
    try {
        return validate_value(document, schema_reference(schema_name), value);
    } catch (const std::exception &) {
        return std::unexpected(app::Error{"contract_error", "OpenAPI schema is not available"});
    }
}

axk::app::Result<void> axk::server::validate_openapi_schema(const nlohmann::json &document,
                                                            const nlohmann::json &schema, const nlohmann::json &value) {
    return validate_value(document, schema, value);
}
