#include <algorithm>
#include <cctype>
#include <ranges>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/operation_registry.hpp"
#include "axklib/server/contract.hpp"
#include "axklib/server/server.hpp"

namespace {

nlohmann::json file_ref(std::string_view path = "fixture.hds") {
    return {{"rootId", "workspace"}, {"relativePath", path}};
}

nlohmann::json directory_ref(std::string_view path = "reports") {
    return {{"rootId", "workspace"}, {"relativePath", path}};
}

bool is_upper_snake(std::string_view value) {
    return !value.empty() && std::ranges::all_of(value, [](unsigned char character) {
        return std::isupper(character) != 0 || std::isdigit(character) != 0 || character == '_';
    });
}

void expect_upper_snake_enums(const nlohmann::json &value, std::string_view path = "$") {
    if (value.is_object()) {
        if (const auto enumeration = value.find("enum"); enumeration != value.end()) {
            for (const auto &candidate : *enumeration) {
                if (candidate.is_string()) {
                    EXPECT_TRUE(is_upper_snake(candidate.get_ref<const std::string &>())) << path << ": " << candidate;
                }
            }
        }
        for (const auto &[name, child] : value.items())
            expect_upper_snake_enums(child, std::string{path} + '/' + name);
    } else if (value.is_array()) {
        for (std::size_t index = 0U; index < value.size(); ++index)
            expect_upper_snake_enums(value[index], std::string{path} + '/' + std::to_string(index));
    }
}

TEST(ServerContract, EmbedsValidOpenApi31WithSandboxReferences) {
    const auto document = nlohmann::json::parse(axk::server::embedded_openapi());
    EXPECT_EQ(document.at("openapi"), "3.1.0");
    EXPECT_TRUE(document.at("paths").contains("/system/version"));
    EXPECT_TRUE(document.at("paths").contains("/system/shutdown"));
    EXPECT_TRUE(document.at("paths").contains("/roots"));
    EXPECT_TRUE(document.at("paths").contains("/files/list"));
    EXPECT_TRUE(document.at("paths").contains("/filesystem/directories"));
    EXPECT_TRUE(document.at("paths").contains("/filesystem/entries"));
    EXPECT_TRUE(document.at("paths").contains("/images"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/content"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/objects"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/relationships"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/validation/issues"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/preview"));
    EXPECT_TRUE(document.at("paths").contains("/auditions/{auditionId}"));
    EXPECT_TRUE(document.at("paths").contains("/auditions/{auditionId}/audio"));
    EXPECT_TRUE(document.at("paths").contains("/jobs/{jobId}"));
    EXPECT_TRUE(document.at("paths").contains("/jobs/{jobId}/events"));
    EXPECT_TRUE(document.at("paths").contains("/event-tickets"));
    EXPECT_TRUE(document.at("paths").contains("/events"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("FileRef"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("DirectoryRef"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("EntryRef"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("EntryMetadata"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("UploadRef"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("ErrorResponse"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("Job"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("JobEvent"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("ImageContentItem"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("ImageContentPageResponse"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("AuditionPrepareRequest"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("Audition"));
    const auto &policy = document.at("info").at("x-axklib-deprecation-policy");
    EXPECT_EQ(policy.at("minimumNoticeDays"), 180);
    EXPECT_TRUE(policy.at("removalRequiresNewApiMajor"));
    const auto &headers = document.at("components").at("headers");
    EXPECT_TRUE(headers.contains("Deprecation"));
    EXPECT_TRUE(headers.contains("Sunset"));
    EXPECT_TRUE(headers.contains("DeprecationLink"));
}

TEST(ServerContract, ImageObjectScopeUsesAnOpaqueContentNodeIdentifier) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    const auto &parameters = document.at("paths").at("/images/{imageId}/objects").at("parameters");
    const auto scope = std::ranges::find_if(parameters, [](const auto &parameter) {
        return parameter.is_object() && parameter.value("name", "") == "scopeId";
    });
    ASSERT_NE(scope, parameters.end());
    EXPECT_EQ(scope->at("in"), "query");
    EXPECT_FALSE(scope->value("required", false));
    EXPECT_EQ(scope->at("schema").at("type"), "string");

    const auto &content_response = document.at("paths")
                                       .at("/images/{imageId}/content")
                                       .at("get")
                                       .at("responses")
                                       .at("200")
                                       .at("content")
                                       .at("application/json")
                                       .at("schema")
                                       .at("$ref");
    EXPECT_EQ(content_response, "#/components/schemas/ImageContentPageResponse");
}

TEST(ServerContract, ImageRelationshipsExposeBoundedFiltersAndAssignmentChannelMetadata) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    const auto &path = document.at("paths").at("/images/{imageId}/relationships");
    const auto &parameters = path.at("parameters");
    for (const auto name : {"scopeId", "sourceObjectId", "targetObjectId", "type"}) {
        const auto parameter = std::ranges::find_if(parameters, [name](const auto &candidate) {
            return candidate.is_object() && candidate.value("name", "") == name;
        });
        ASSERT_NE(parameter, parameters.end()) << name;
        EXPECT_EQ(parameter->at("in"), "query");
        EXPECT_FALSE(parameter->value("required", false));
        EXPECT_EQ(parameter->at("schema").at("type"), "string");
    }

    const auto &response =
        path.at("get").at("responses").at("200").at("content").at("application/json").at("schema").at("$ref");
    EXPECT_EQ(response, "#/components/schemas/ImageRelationshipPageResponse");
    const auto &item = document.at("components").at("schemas").at("ImageRelationshipItem");
    EXPECT_TRUE(std::ranges::contains(item.at("required"), "receiveChannelDisplay"));
    EXPECT_EQ(item.at("properties").at("receiveChannelDisplay").at("type"), "string");
}

TEST(ServerContract, RegistryIsTheOnlyDomainOperationRouteInventory) {
    const auto registry = axk::app::make_operation_registry();
    const auto entries = registry.entries();
    EXPECT_EQ(entries.size(), 27U);
    EXPECT_EQ(entries.front().descriptor.id, "system.version");
    EXPECT_EQ(entries.front().descriptor.route, "/api/v1/system/version");
}

TEST(ServerContract, CompleteDocumentDerivesEveryDomainRouteAndSchemaFromRegistry) {
    const auto registry = axk::app::make_operation_registry();
    const auto document = axk::server::build_openapi_document(axk::server::embedded_openapi(), registry);
    const auto &schemas = document.at("components").at("schemas");
    for (const auto &entry : registry.entries()) {
        const auto path = entry.descriptor.route.substr(std::string_view{"/api/v1"}.size());
        const auto method = entry.descriptor.method == axk::app::HttpMethod::get ? "get" : "post";
        ASSERT_TRUE(document.at("paths").contains(path)) << entry.descriptor.id;
        ASSERT_TRUE(document.at("paths").at(path).contains(method)) << entry.descriptor.id;
        const auto &operation = document.at("paths").at(path).at(method);
        const auto operation_ids = operation.at("x-axklib-operation-ids").get<std::vector<std::string>>();
        EXPECT_TRUE(std::ranges::contains(operation_ids, entry.descriptor.id)) << entry.descriptor.id;
        const auto &operation_descriptors = operation.at("x-axklib-operation-descriptors");
        const auto descriptor = std::ranges::find_if(operation_descriptors, [&entry](const auto &candidate) {
            return candidate.at("id") == entry.descriptor.id;
        });
        ASSERT_NE(descriptor, operation_descriptors.end()) << entry.descriptor.id;
        EXPECT_EQ(descriptor->at("cliCommand"), entry.descriptor.cli_command.empty()
                                                    ? nlohmann::json(nullptr)
                                                    : nlohmann::json(entry.descriptor.cli_command));
        EXPECT_EQ(descriptor->at("cliParity"), entry.descriptor.cli_parity);
        EXPECT_EQ(descriptor->at("mode"), axk::app::execution_mode_name(entry.descriptor.mode));
        EXPECT_EQ(descriptor->at("operationClass"), axk::app::operation_class_name(entry.descriptor.operation_class));
        EXPECT_EQ(descriptor->at("requiresIdempotency"), entry.descriptor.requires_idempotency);
        EXPECT_EQ(descriptor->at("variant"), entry.descriptor.variant.empty()
                                                 ? nlohmann::json(nullptr)
                                                 : nlohmann::json(entry.descriptor.variant));
        EXPECT_EQ(descriptor->at("requestSchema"), entry.descriptor.request_schema);
        EXPECT_EQ(descriptor->at("resultSchema"), entry.descriptor.result_schema);
        EXPECT_TRUE(schemas.contains(entry.descriptor.request_schema)) << entry.descriptor.request_schema;
        EXPECT_TRUE(schemas.contains(entry.descriptor.result_schema)) << entry.descriptor.result_schema;
        EXPECT_FALSE(schemas.at(entry.descriptor.request_schema).value("description", "").starts_with("Canonical "));
        EXPECT_FALSE(schemas.at(entry.descriptor.result_schema).value("description", "").starts_with("Canonical "));
        for (const auto status : {"400", "401", "403", "404", "409", "413", "422", "429", "500"}) {
            const auto &response = operation.at("responses").at(status);
            EXPECT_EQ(response.at("content").at("application/json").at("schema").at("$ref"),
                      "#/components/schemas/ErrorResponse");
        }
        for (const auto &[status, response] : operation.at("responses").items()) {
            static_cast<void>(status);
            EXPECT_EQ(response.at("headers").at("X-Request-Id").at("$ref"), "#/components/headers/XRequestId");
        }
    }
}

TEST(ServerContract, EveryOpenApiOperationIdIsUnique) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    std::vector<std::string> operation_ids;
    for (const auto &[path, path_item] : document.at("paths").items()) {
        for (const auto &[method, operation] : path_item.items()) {
            if (!operation.is_object() || !operation.contains("operationId"))
                continue;
            const auto operation_id = operation.at("operationId").get<std::string>();
            EXPECT_FALSE(std::ranges::contains(operation_ids, operation_id))
                << operation_id << " is duplicated at " << method << ' ' << path;
            operation_ids.push_back(operation_id);
        }
    }
    EXPECT_TRUE(std::ranges::contains(operation_ids, "operations.dispatch.extractions"));
    EXPECT_TRUE(std::ranges::contains(operation_ids, "operations.dispatch.image.builds"));
}

TEST(ServerContract, EveryHttpResponseCarriesRequestIdAndPaginationIsBounded) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    for (const auto &[path, path_item] : document.at("paths").items()) {
        for (const auto &[method, operation] : path_item.items()) {
            if (!operation.is_object() || !operation.contains("responses"))
                continue;
            for (const auto &[status, response] : operation.at("responses").items()) {
                EXPECT_EQ(response.at("headers").at("X-Request-Id").at("$ref"), "#/components/headers/XRequestId")
                    << path << ' ' << method << ' ' << status;
            }
        }
    }
    const auto &parameters = document.at("components").at("parameters");
    EXPECT_EQ(parameters.at("PageLimit").at("schema").at("minimum"), 1);
    EXPECT_EQ(parameters.at("PageLimit").at("schema").at("maximum"), 5000);
    EXPECT_EQ(parameters.at("PageCursor").at("schema").at("minLength"), 1);
    EXPECT_EQ(parameters.at("PageCursor").at("schema").at("maxLength"), 512);
}

TEST(ServerContract, CanonicalReportRequestSchemasMatchApplicationInputs) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());

    const auto info = nlohmann::json{
        {"sources", nlohmann::json::array({file_ref()})}, {"strict", true}, {"includeDefaultPrograms", true}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "InfoRequest", info));
    auto invalid_info = info;
    invalid_info["destination"] = directory_ref();
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "InfoRequest", invalid_info));

    const auto objects = nlohmann::json{
        {"sources", nlohmann::json::array({file_ref()})}, {"destination", directory_ref()}, {"objectType", "SMPL"}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ObjectsRequest", objects));
    auto invalid_objects = objects;
    invalid_objects["unknownField"] = true;
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "ObjectsRequest", invalid_objects));

    const auto audit = nlohmann::json{{"sources", nlohmann::json::array({file_ref()})},
                                      {"destination", directory_ref()},
                                      {"policy", "NORMAL"},
                                      {"waveSmokeLimit", 1'000'000U},
                                      {"skipWaveSmoke", true},
                                      {"overwrite", false}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "CorpusAuditRequest", audit));
    auto excessive_audit = audit;
    excessive_audit["waveSmokeLimit"] = 1'000'001U;
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "CorpusAuditRequest", excessive_audit));

    const auto validation = nlohmann::json{{"exports", directory_ref("exports")},
                                           {"destination", directory_ref("validation")},
                                           {"policy", "SALVAGE_AWARE"},
                                           {"overwrite", true}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ValidationRequest", validation));
    auto ambiguous_validation = validation;
    ambiguous_validation["sources"] = nlohmann::json::array({file_ref()});
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "ValidationRequest", ambiguous_validation));
}

TEST(ServerContract, WireEnumsAreUpperSnakeAndTranslateOnlyAtTheApplicationBoundary) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    expect_upper_snake_enums(document.at("components").at("schemas"));
    axk::server::OpenApiValidator validator{document};

    const auto wire_request =
        nlohmann::json{{"source", file_ref()},
                       {"output", file_ref("package.axkprog")},
                       {"roots", nlohmann::json::array({{{"kind", "PROGRAM"}, {"objectName", "Program 1"}}})}};
    ASSERT_TRUE(validator.validate("PackageExportRequest", wire_request));
    const auto application_request = validator.application_value("PackageExportRequest", wire_request);
    EXPECT_EQ(application_request.at("roots").at(0).at("kind"), "prog");
    EXPECT_EQ(validator.wire_value("PackageExportRequest", application_request), wire_request);

    const auto application_result = nlohmann::json{
        {"schemaVersion", "1.0"},
        {"packageId", "package-1"},
        {"packageKind", "program"},
        {"requiredExtension", ".axksbnk"},
        {"sourceMediaKind", "sfs"},
        {"valid", true},
        {"payloadsVerified", true},
        {"roots", nlohmann::json::array(
                      {{{"kind", "prog"}, {"displayName", "Program 1"}, {"nodeIds", nlohmann::json::array()}}})},
        {"objects", nlohmann::json::array()},
        {"relationshipCount", 0},
        {"issues", nlohmann::json::array()}};
    const auto wire_result = validator.wire_value("PackageInspection", application_result);
    EXPECT_EQ(wire_result.at("packageKind"), "PROGRAM");
    EXPECT_EQ(wire_result.at("sourceMediaKind"), "SFS");
    EXPECT_EQ(wire_result.at("roots").at(0).at("kind"), "PROGRAM");
    EXPECT_TRUE(validator.validate("PackageInspection", wire_result));
    EXPECT_EQ(validator.application_value("PackageInspection", wire_result), application_result);
}

TEST(ServerContract, SharedRouteSchemaAdmitsOnlyDeclaredOperationDiscriminators) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    const auto &schema = document.at("paths")
                             .at("/extractions")
                             .at("post")
                             .at("requestBody")
                             .at("content")
                             .at("application/json")
                             .at("schema");
    const auto request = nlohmann::json{{"operationId", "extract.sfz"},
                                        {"sources", nlohmann::json::array({file_ref()})},
                                        {"destination", directory_ref("exports")},
                                        {"scope", "FILE"}};
    EXPECT_TRUE(axk::server::validate_openapi_schema(document, schema, request));
    auto unknown = request;
    unknown["operationId"] = "extract.unknown";
    EXPECT_FALSE(axk::server::validate_openapi_schema(document, schema, unknown));
    auto extra = request;
    extra["unexpected"] = true;
    EXPECT_FALSE(axk::server::validate_openapi_schema(document, schema, extra));
}

TEST(ServerContract, EveryDomainOperationCarriesSchemaValidExamplesAndMediaTypes) {
    const auto registry = axk::app::make_operation_registry();
    const auto document = axk::server::build_openapi_document(axk::server::embedded_openapi(), registry);
    for (const auto &entry : registry.entries()) {
        const auto path = entry.descriptor.route.substr(std::string_view{"/api/v1"}.size());
        const auto method = entry.descriptor.method == axk::app::HttpMethod::get ? "get" : "post";
        const auto &operation = document.at("paths").at(path).at(method);
        if (entry.descriptor.method == axk::app::HttpMethod::post) {
            const auto &content = operation.at("requestBody").at("content").at("application/json");
            const auto &example = content.at("examples").at("default").at("value");
            EXPECT_TRUE(axk::server::validate_openapi_schema(document, content.at("schema"), example))
                << entry.descriptor.id << " request";
        }

        const auto success_status = entry.descriptor.mode == axk::app::ExecutionMode::job ? "202" : "200";
        const auto &success = operation.at("responses").at(success_status).at("content").at("application/json");
        for (const auto example_name : {"success", "warning"}) {
            const auto &example = success.at("examples").at(example_name).at("value");
            EXPECT_TRUE(axk::server::validate_openapi_schema(document, success.at("schema"), example))
                << entry.descriptor.id << ' ' << example_name;
        }
        for (const auto status : {"400", "401", "403", "404", "409", "413", "422", "429", "500"}) {
            const auto &error = operation.at("responses").at(status).at("content").at("application/json");
            const auto &example = error.at("examples").at("default").at("value");
            EXPECT_TRUE(axk::server::validate_openapi_schema(document, error.at("schema"), example))
                << entry.descriptor.id << ' ' << status;
        }
    }
}

TEST(ServerContract, MissingCanonicalSchemaFailsGeneration) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(registry.declare({"synthetic.missing",
                                  {},
                                  axk::app::HttpMethod::post,
                                  "/api/v1/synthetic",
                                  axk::app::ExecutionMode::request,
                                  {},
                                  "MissingRequest",
                                  "VersionResponse",
                                  axk::app::OperationClass::read,
                                  false,
                                  false}));
    EXPECT_THROW(
        {
            const auto document = axk::server::build_openapi_document(axk::server::embedded_openapi(), registry);
            static_cast<void>(document);
        },
        std::runtime_error);
}

} // namespace
