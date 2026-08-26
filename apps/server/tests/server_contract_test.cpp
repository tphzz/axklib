#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/operation_registry.hpp"
#include "axklib/server/contract.hpp"
#include "axklib/server/server.hpp"
#include "contract_test_support.hpp"

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
    EXPECT_TRUE(document.at("paths").contains("/files/media-source/inspect"));
    EXPECT_TRUE(document.at("paths").contains("/filesystem/directories"));
    EXPECT_TRUE(document.at("paths").contains("/filesystem/entries"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/companions"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/content"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/objects"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/relationships"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/validation/issues"));
    EXPECT_TRUE(document.at("paths").contains("/images/{imageId}/preview"));
    EXPECT_TRUE(document.at("paths").contains("/auditions/{auditionId}"));
    EXPECT_TRUE(document.at("paths").contains("/auditions/{auditionId}/content"));
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
    EXPECT_TRUE(document.at("components").at("schemas").contains("ImageCompanionsRequest"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("AuditionPrepareRequest"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("AuditionBundle"));
    EXPECT_TRUE(document.at("components").at("schemas").contains("AudioSourceInfo"));
    const auto &headers = document.at("components").at("headers");
    EXPECT_TRUE(headers.contains("XRequestId"));
}

TEST(ServerContract, GeneratedValidationPlanMatchesTheSchemaCompilerForRepresentativeValues) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    const axk::server::OpenApiValidator generated;
    const axk::server::OracleOpenApiValidator oracle{document};
    const auto representatives = std::to_array<nlohmann::json>({
        nullptr,
        false,
        true,
        -1,
        0,
        1,
        1.5,
        "",
        "x",
        "é",
        "A01",
        "A16",
        "A00",
        "A17",
        "B01",
        "B16",
        "Bch",
        "01",
        "16",
        "00",
        "17",
        ".iso",
        ".axkvol",
        ".AXKVOL",
        "/api/v1/",
        "/api/v1/system/version",
        "/api/v2/system/version",
        "/api/v1/download-archives/archive1/content",
        "/api/v1/download-archives/archive-1/content",
        "00-43-10",
        "0A",
        "0a",
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        nlohmann::json::array(),
        nlohmann::json::object(),
        nlohmann::json{{"unexpected", true}},
    });
    for (const auto &[name, schema] : document.at("components").at("schemas").items()) {
        static_cast<void>(schema);
        for (const auto &value : representatives) {
            EXPECT_EQ(static_cast<bool>(generated.validate(name, value)), oracle.validate(name, value))
                << name << ": " << value.dump();
        }
    }
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

    const auto &content_item = document.at("components").at("schemas").at("ImageContentItem");
    EXPECT_TRUE(std::ranges::contains(content_item.at("required"), "scopeRole"));
    EXPECT_TRUE(std::ranges::contains(content_item.at("required"), "partitionCapacity"));
    EXPECT_TRUE(std::ranges::contains(content_item.at("required"), "sizeBytes"));
    EXPECT_EQ(content_item.at("properties").at("sizeBytes").at("type"), nlohmann::json::array({"integer", "null"}));
    const auto &partition_capacity = document.at("components").at("schemas").at("ImagePartitionCapacity");
    EXPECT_EQ(partition_capacity.at("required"),
              nlohmann::json::array({"allocatedClusters", "freeClusters", "clusterSizeBytes"}));
    EXPECT_EQ(content_item.at("properties").at("scopeRole").at("enum"),
              nlohmann::json::array({"CONTAINED", "REFERENCE"}));
    const auto &object_item = document.at("components").at("schemas").at("ImageObjectItem");
    EXPECT_TRUE(std::ranges::contains(object_item.at("required"), "sizeWithDependenciesBytes"));
    EXPECT_EQ(object_item.at("properties").at("sizeWithDependenciesBytes").at("type"),
              nlohmann::json::array({"integer", "null"}));
}

TEST(ServerContract, SequenceMetadataSeparatesHeaderAndTimelineTempo) {
    const auto document = nlohmann::json::parse(axk::server::embedded_openapi());
    const auto &schemas = document.at("components").at("schemas");
    const auto &sequence = schemas.at("SequenceMetadata");
    EXPECT_TRUE(std::ranges::contains(sequence.at("required"), "headerTempoBpm"));
    EXPECT_TRUE(std::ranges::contains(sequence.at("required"), "effectiveInitialTempoMicrosecondsPerQuarterNote"));
    EXPECT_TRUE(std::ranges::contains(sequence.at("required"), "tempoEvents"));
    EXPECT_FALSE(sequence.at("properties").contains("tempoBpm"));
    EXPECT_EQ(sequence.at("properties").at("tempoEvents").at("items").at("$ref"),
              "#/components/schemas/SequenceTempoEvent");
    const auto &tempo = schemas.at("SequenceTempoEvent");
    EXPECT_EQ(tempo.at("properties").at("microsecondsPerQuarterNote").at("minimum"), 200'000U);
    EXPECT_EQ(tempo.at("properties").at("microsecondsPerQuarterNote").at("maximum"), 2'000'000U);
}

TEST(ServerContract, DirectoryListingsSeparateMediaSourceInspection) {
    const auto document = nlohmann::json::parse(axk::server::embedded_openapi());
    const auto &schemas = document.at("components").at("schemas");
    const auto &entry =
        schemas.at("DirectoryListResponse").at("properties").at("data").at("properties").at("entries").at("items");
    EXPECT_FALSE(entry.at("properties").contains("mediaSourceKind"));
    const auto &inspection =
        schemas.at("MediaSourceInspectResponse").at("properties").at("data").at("properties").at("mediaSourceKind");
    EXPECT_EQ(inspection.at("enum"), nlohmann::json::array({"AXK_OBJECT_DIRECTORY", nullptr}));
}

TEST(ServerContract, AlterationJobReportsIncludeTx16wDiskSetImports) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    axk::server::OpenApiValidator validator;
    const auto &type =
        document.at("components").at("schemas").at("AlterationOperationReport").at("properties").at("type");
    EXPECT_TRUE(std::ranges::contains(type.at("enum"), "IMPORT_TX16W_DISK_SET"));
    EXPECT_EQ(type.at("x-axklib-application-enum").at("IMPORT_TX16W_DISK_SET"), "import_tx16w_disk_set");
    const auto application_report = nlohmann::json{{"id", "tx16w-import"},
                                                   {"type", "import_tx16w_disk_set"},
                                                   {"partitionIndex", 0U},
                                                   {"volumeName", "TX16W"},
                                                   {"objectName", ""},
                                                   {"removedSfsIds", nlohmann::json::array()},
                                                   {"insertedSfsIds", nlohmann::json::array({4U, 5U})},
                                                   {"placedSfsIds", nlohmann::json::array()},
                                                   {"freedClusters", 0U},
                                                   {"allocatedClusters", 2U},
                                                   {"audioImport", nullptr}};
    const auto wire_report = validator.wire_value("AlterationOperationReport", application_report);
    EXPECT_EQ(wire_report.at("type"), "IMPORT_TX16W_DISK_SET");
    EXPECT_TRUE(validator.validate("AlterationOperationReport", wire_report));
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

TEST(ServerContract, SystemProgramContextsArePartitionScopedAndIndependentlyAvailable) {
    const auto document = nlohmann::json::parse(axk::server::embedded_openapi());
    const auto &path = document.at("paths").at("/images/{imageId}/system-program-contexts");
    EXPECT_EQ(path.at("get").at("operationId"), "images.systemProgramContexts");
    const auto &parameters = path.at("parameters");
    const auto partition = std::ranges::find_if(parameters, [](const auto &candidate) {
        return candidate.is_object() && candidate.value("name", "") == "partitionIndex";
    });
    ASSERT_NE(partition, parameters.end());
    EXPECT_TRUE(partition->at("required"));
    EXPECT_EQ(partition->at("schema").at("minimum"), 0U);
    EXPECT_EQ(partition->at("schema").at("maximum"), 7U);

    const auto &response =
        path.at("get").at("responses").at("200").at("content").at("application/json").at("schema").at("$ref");
    EXPECT_EQ(response, "#/components/schemas/SystemProgramContextsResponse");
    const auto &contexts = document.at("components").at("schemas").at("SystemProgramContexts");
    EXPECT_TRUE(std::ranges::contains(contexts.at("required"), "files"));
    const auto &files = contexts.at("properties").at("files");
    EXPECT_EQ(files.at("maxItems"), 2U);
    EXPECT_EQ(files.at("items").at("$ref"), "#/components/schemas/SystemProgramContext");
    const auto &context = document.at("components").at("schemas").at("SystemProgramContext");
    ASSERT_EQ(context.at("oneOf").size(), 4U);
}

TEST(ServerContract, RegistryIsTheOnlyDomainOperationRouteInventory) {
    const auto registry = axk::app::make_operation_registry();
    const auto entries = registry.entries();
    EXPECT_EQ(entries.size(), 57U);
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
        EXPECT_EQ(descriptor->at("mode").get<std::string>(),
                  std::string{axk::app::execution_mode_name(entry.descriptor.mode)});
        EXPECT_EQ(descriptor->at("operationClass").get<std::string>(),
                  std::string{axk::app::operation_class_name(entry.descriptor.operation_class)});
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

TEST(ServerContract, InfrastructureJsonOperationsDeclareConcreteRequestAndResponseSchemas) {
    struct Expectation {
        std::string_view path;
        std::string_view method;
        std::string_view request_schema;
        std::string_view status;
        std::string_view response_schema;
    };
    constexpr std::array expectations{
        Expectation{"/system/capabilities", "get", "", "200", "CapabilitiesResponse"},
        Expectation{"/system/metrics", "get", "", "200", "MetricsResponse"},
        Expectation{"/system/health/ready", "get", "", "200", "ReadinessResponse"},
        Expectation{"/roots", "get", "", "200", "RootsResponse"},
        Expectation{"/workspaces", "get", "", "200", "WorkspaceSnapshotResponse"},
        Expectation{"/workspaces", "post", "WorkspaceCreateRequest", "201", "WorkspaceResponse"},
        Expectation{"/workspaces/{workspaceId}", "patch", "WorkspaceUpdateRequest", "200", "WorkspaceResponse"},
        Expectation{"/workspaces/recovery/reset", "post", "", "200", "WorkspaceResetResponse"},
        Expectation{"/host-directories/roots", "get", "", "200", "HostDirectoryRootsResponse"},
        Expectation{"/host-directories/list", "post", "HostDirectoryListRequest", "200", "HostDirectoryListResponse"},
        Expectation{"/files/list", "post", "DirectoryListRequest", "200", "DirectoryListResponse"},
        Expectation{"/files/media-source/inspect", "post", "MediaSourceInspectRequest", "200",
                    "MediaSourceInspectResponse"},
        Expectation{"/files/metadata", "post", "EntryRef", "200", "EntryMetadataResponse"},
        Expectation{"/filesystem/directories", "post", "CreateDirectoryRequest", "201", "EntryMetadataResponse"},
        Expectation{"/filesystem/entries", "patch", "RenameEntryRequest", "200", "EntryMetadataResponse"},
        Expectation{"/images/{imageId}", "get", "", "200", "ImageSessionResponse"},
        Expectation{"/images/{imageId}", "delete", "", "200", "ImageCloseResponse"},
        Expectation{"/images/{imageId}/companions", "post", "ImageCompanionsRequest", "200", "ImageSessionResponse"},
        Expectation{"/images/{imageId}/content", "get", "", "200", "ImageContentPageResponse"},
        Expectation{"/images/{imageId}/objects", "get", "", "200", "ImageObjectPageResponse"},
        Expectation{"/images/{imageId}/relationships", "get", "", "200", "ImageRelationshipPageResponse"},
        Expectation{"/images/{imageId}/validation/issues", "get", "", "200", "ImageValidationPageResponse"},
        Expectation{"/images/{imageId}/preview", "get", "", "200", "ImagePreviewResponse"},
        Expectation{"/uploads", "post", "UploadCreateRequest", "201", "UploadResponse"},
        Expectation{"/uploads/{uploadId}", "get", "", "200", "UploadResponse"},
        Expectation{"/uploads/{uploadId}", "put", "", "200", "UploadResponse"},
        Expectation{"/uploads/{uploadId}/complete", "post", "", "200", "UploadResponse"},
        Expectation{"/uploads/{uploadId}/materialize", "post", "UploadMaterializeRequest", "201",
                    "MaterializedFileResponse"},
    };
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    const auto &schemas = document.at("components").at("schemas");
    for (const auto &expected : expectations) {
        const auto &operation = document.at("paths").at(expected.path).at(expected.method);
        if (!expected.request_schema.empty()) {
            const auto reference = "#/components/schemas/" + std::string{expected.request_schema};
            EXPECT_EQ(operation.at("requestBody").at("content").at("application/json").at("schema").at("$ref"),
                      reference);
            EXPECT_TRUE(schemas.contains(expected.request_schema));
        }
        const auto reference = "#/components/schemas/" + std::string{expected.response_schema};
        EXPECT_EQ(
            operation.at("responses").at(expected.status).at("content").at("application/json").at("schema").at("$ref"),
            reference);
        EXPECT_TRUE(schemas.contains(expected.response_schema));
    }
    for (const auto &[path, path_item] : document.at("paths").items()) {
        for (const auto &[method, operation] : path_item.items()) {
            if (!operation.is_object() || !operation.contains("responses") ||
                operation.contains("x-axklib-operation-ids")) {
                continue;
            }
            const auto &fallback = operation.at("responses").at("default");
            EXPECT_EQ(fallback.at("content").at("application/json").at("schema").at("$ref"),
                      "#/components/schemas/ErrorResponse")
                << method << ' ' << path;
            for (const auto &[status, response] : operation.at("responses").items()) {
                if (status.size() != 3U || (status.front() != '4' && status.front() != '5') || status == "416" ||
                    status == "503") {
                    continue;
                }
                EXPECT_EQ(response.at("content").at("application/json").at("schema").at("$ref"),
                          "#/components/schemas/ErrorResponse")
                    << status << ' ' << method << ' ' << path;
            }
        }
    }
}

TEST(ServerContract, CheckedInCompleteDocumentMatchesRegistry) {
    const auto path = std::filesystem::path{AXK_SOURCE_ROOT} / "apps/server/contracts/openapi-v1.json";
    std::ifstream input{path, std::ios::binary};
    ASSERT_TRUE(input) << path;
    const auto checked_in = nlohmann::json::parse(input);
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    EXPECT_EQ(checked_in, document);
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

    const auto &preview_parameters = document.at("paths").at("/images/{imageId}/preview").at("parameters");
    const auto bins = std::ranges::find_if(preview_parameters, [](const auto &parameter) {
        return parameter.is_object() && parameter.value("name", "") == "bins";
    });
    ASSERT_NE(bins, preview_parameters.end());
    EXPECT_EQ(bins->at("schema").at("maximum"), 4096);

    const auto &limits = document.at("components").at("schemas").at("ApiLimits");
    for (const auto name : {"maximumDownloadArchiveDepth", "maximumDownloadArchivePathBytes",
                            "maximumConcurrentArchiveDownloads", "maximumMediaBuildObjectBytes",
                            "maximumMediaBuildPayloadBytes", "maximumMediaBuildOutputBytes", "maximumUploads"}) {
        EXPECT_TRUE(std::ranges::contains(limits.at("required"), name)) << name;
    }
}

TEST(ServerContract, RelationshipDiagnosticsValidateForInspectionAndTerminalExtractionResults) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    axk::server::OpenApiValidator validator;
    const auto diagnostic = nlohmann::json{
        {"code", "unconfirmed_relationship_excluded"},
        {"message", "Unconfirmed relationship excluded from exact export"},
        {"fatal", false},
        {"relationshipType", "SBNK_LEFT_MEMBER_TO_SMPL"},
        {"relationshipQuality", "Tentative"},
        {"reason", "exact export requires a concrete Known relationship target"},
        {"sourceObjectKey", "sample"},
        {"candidateObjectKeys", nlohmann::json::array({"wave-a", "wave-b"})},
        {"basis", "ambiguous Wave Data candidates"},
        {"assignmentState", "unknown"},
    };
    const auto wire_diagnostic = validator.wire_value("RelationshipDiagnostic", diagnostic);
    EXPECT_EQ(wire_diagnostic.at("relationshipQuality"), "TENTATIVE");
    EXPECT_EQ(wire_diagnostic.at("assignmentState"), "UNKNOWN");
    const auto wire_issue = validator.wire_value("ExportIssue", diagnostic);
    EXPECT_EQ(wire_issue.at("relationshipQuality"), "TENTATIVE");
    EXPECT_EQ(wire_issue.at("assignmentState"), "UNKNOWN");
    const auto inspection = nlohmann::json{
        {"imageId", "image-1"},
        {"revision", 2U},
        {"rootCount", 1U},
        {"programCount", 0U},
        {"sampleBankCount", 0U},
        {"sampleCount", 1U},
        {"waveDataCount", 0U},
        {"wavFileCount", 1U},
        {"sfzFileCount", 0U},
        {"sfzEligible", false},
        {"defaultDirectoryName", "Sample"},
        {"issues", nlohmann::json::array({diagnostic})},
    };
    const auto wire_inspection = validator.wire_value("ImageSessionAudioExportInspection", inspection);
    EXPECT_EQ(wire_inspection.at("issues").at(0).at("relationshipQuality"), "TENTATIVE");
    EXPECT_EQ(wire_inspection.at("issues").at(0).at("assignmentState"), "UNKNOWN");
    EXPECT_TRUE(validator.validate("ImageSessionAudioExportInspection", wire_inspection));

    const auto extraction = nlohmann::json{
        {"schemaVersion", "1.0"},
        {"mode", "WAV"},
        {"destination", directory_ref()},
        {"artifactCount", 0U},
        {"waveformCount", 0U},
        {"writtenFileCount", 0U},
        {"selectionGraphCount", 0U},
        {"sfzFileCount", 0U},
        {"decodeErrorCount", 0U},
        {"loadErrorCount", 0U},
        {"artifacts", nlohmann::json::array()},
        {"warnings", nlohmann::json::array({diagnostic})},
    };
    const auto wire_extraction = validator.wire_value("ExtractionResult", extraction);
    EXPECT_TRUE(validator.validate("ExtractionResult", wire_extraction));

    auto invalid = wire_inspection;
    invalid["issues"][0]["unexpected"] = true;
    EXPECT_FALSE(validator.validate("ImageSessionAudioExportInspection", invalid));
}

TEST(ServerContract, ProgramAssignmentAdjustmentsValidateForPlansAndImportResults) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    axk::server::OpenApiValidator validator;
    const auto adjustment = nlohmann::json{
        {"adjustmentId", "adjustment-1"},
        {"origin", "existing-program"},
        {"packageIndex", nullptr},
        {"actionId", nullptr},
        {"existingObjectKey", "sfs:0:17"},
        {"programSlot", "001"},
        {"programName", "Program One"},
        {"assignmentOrdinal", 2U},
        {"targetObjectType", "SBAC"},
        {"targetName", "Imported Bank"},
        {"partitionIndex", 0U},
        {"groupName", ""},
        {"volumeName", "Imported"},
        {"rawGroup", ""},
        {"rawVolume", ""},
        {"reasonCode", "UNRESOLVED_PROGRAM_ASSIGNMENT_COLLISION"},
        {"disposition", "clear-assignment"},
    };
    const auto adjustments = nlohmann::json::array({adjustment});
    const auto placements = nlohmann::json::array({{
        {"placementId", "placement-1"},
        {"partitionIndex", 0U},
        {"volumeName", "Imported"},
        {"mode", "contiguous"},
        {"applied", false},
        {"suggestedStartSlot", 5U},
        {"requiredSlotCount", 4U},
        {"availableSlotCount", 124U},
        {"occupiedRanges", nlohmann::json::array({{{"first", 1U}, {"last", 4U}}})},
        {"sourceRanges", nlohmann::json::array({{{"first", 1U}, {"last", 4U}}})},
        {"destinationRanges", nlohmann::json::array({{{"first", 5U}, {"last", 8U}}})},
        {"mappings", nlohmann::json::array({{{"packageIndex", 0U},
                                             {"nodeId", "program-1"},
                                             {"sourceSlot", 1U},
                                             {"destinationSlot", 5U},
                                             {"requiresUserAction", true}}})},
    }});
    const auto application_plan = nlohmann::json{
        {"schemaVersion", "1.0"},
        {"planToken", "plan-token"},
        {"expiresInSeconds", 600U},
        {"planId", "plan-1"},
        {"targetKind", "sfs"},
        {"targetSnapshotId", "snapshot-1"},
        {"valid", true},
        {"warnings", nlohmann::json::array({{{"code", "TARGET_SEQUENCE_PRESERVED_OPAQUE"},
                                             {"message", "existing Sequence will be preserved unchanged"},
                                             {"origin", "target"},
                                             {"packageIndex", nullptr},
                                             {"nodeId", ""},
                                             {"objectType", "SEQU"},
                                             {"objectName", "Opaque Sequence"},
                                             {"partitionIndex", 0U},
                                             {"volumeName", "Existing"}}})},
        {"opaqueSequences", nlohmann::json::array({{{"packageIndex", 0U},
                                                    {"nodeId", "sequence-1"},
                                                    {"name", "Opaque Sequence"},
                                                    {"action", "preserve-unchanged"}}})},
        {"conflicts", nlohmann::json::array()},
        {"actions", nlohmann::json::array()},
        {"programAssignmentAdjustments", adjustments},
        {"programSlotPlacements", placements},
        {"allocation", nlohmann::json::array()},
        {"sfsIndexCapacity", nlohmann::json::array()},
    };
    const auto wire_plan = validator.wire_value("PackageImportPlan", application_plan);
    ASSERT_TRUE(validator.validate("PackageImportPlan", wire_plan));
    EXPECT_EQ(wire_plan.at("programAssignmentAdjustments").at(0).at("origin"), "EXISTING_PROGRAM");
    EXPECT_EQ(wire_plan.at("programAssignmentAdjustments").at(0).at("disposition"), "CLEAR_ASSIGNMENT");
    EXPECT_EQ(wire_plan.at("programSlotPlacements").at(0).at("mode"), "CONTIGUOUS");
    EXPECT_EQ(wire_plan.at("warnings").at(0).at("origin"), "TARGET");
    EXPECT_EQ(wire_plan.at("opaqueSequences").at(0).at("action"), "PRESERVE_UNCHANGED");

    const auto application_result = nlohmann::json{
        {"schemaVersion", "1.0"},
        {"planId", "plan-1"},
        {"output", file_ref("imported.hds")},
        {"sourceSnapshotId", "snapshot-1"},
        {"outputSnapshotId", "snapshot-2"},
        {"programAssignmentAdjustments", adjustments},
        {"applied", true},
    };
    const auto wire_result = validator.wire_value("PackageImportResult", application_result);
    EXPECT_TRUE(validator.validate("PackageImportResult", wire_result));

    auto application_session_result = application_plan;
    application_session_result.erase("planToken");
    application_session_result.erase("expiresInSeconds");
    application_session_result.erase("valid");
    application_session_result.erase("warnings");
    application_session_result.erase("opaqueSequences");
    application_session_result.erase("conflicts");
    application_session_result.erase("sfsIndexCapacity");
    application_session_result["imageId"] = "image-1";
    application_session_result["revision"] = 2U;
    application_session_result["objectCount"] = 4U;
    application_session_result["applied"] = true;
    const auto wire_session_result =
        validator.wire_value("ImageSessionPackageImportResult", application_session_result);
    EXPECT_TRUE(validator.validate("ImageSessionPackageImportResult", wire_session_result));
    auto invalid_session_result = wire_session_result;
    invalid_session_result["sfsIndexCapacity"] = nlohmann::json::array();
    EXPECT_FALSE(validator.validate("ImageSessionPackageImportResult", invalid_session_result));

    auto invalid_result = wire_result;
    invalid_result.at("programAssignmentAdjustments").at(0)["unexpected"] = true;
    EXPECT_FALSE(validator.validate("PackageImportResult", invalid_result));
}

TEST(ServerContract, CanonicalReportRequestSchemasMatchApplicationInputs) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());

    const auto info = nlohmann::json{{"sources", nlohmann::json::array({{{"kind", "FILE"}, {"file", file_ref()}}})},
                                     {"strict", true},
                                     {"includeDefaultPrograms", true}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "InfoRequest", info));
    auto directory_info = info;
    directory_info["sources"] =
        nlohmann::json::array({{{"kind", "AXK_OBJECT_DIRECTORY"}, {"directory", directory_ref()}}});
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "InfoRequest", directory_info));
    auto obsolete_info = info;
    obsolete_info["sources"] = nlohmann::json::array({file_ref()});
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "InfoRequest", obsolete_info));
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

TEST(ServerContract, ObjectDeletionUsesBoundedBatchSelectionsAndReportsPartialApplicability) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    const auto request = nlohmann::json{{"imageId", "image-1"},
                                        {"expectedRevision", 4U},
                                        {"targetObjectIds", nlohmann::json::array({"program-1", "sample-1"})},
                                        {"referrerObjectIds", nlohmann::json::array()},
                                        {"cleanupObjectIds", nlohmann::json::array({"wave-1"})}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImageObjectDeletionRequest", request));

    auto single_target = request;
    single_target["targetObjectIds"] = "sample-1";
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "ImageObjectDeletionRequest", single_target));

    auto duplicate_targets = request;
    duplicate_targets["targetObjectIds"] = nlohmann::json::array({"sample-1", "sample-1"});
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "ImageObjectDeletionRequest", duplicate_targets));

    const auto inspection =
        nlohmann::json{{"canApply", true},
                       {"imageId", "image-1"},
                       {"revision", 4U},
                       {"targetObjectIds", nlohmann::json::array({"program-1", "sample-1"})},
                       {"referrerObjectIds", nlohmann::json::array()},
                       {"cleanupObjectIds", nlohmann::json::array({"wave-1"})},
                       {"selectedObjectIds", nlohmann::json::array({"program-1"})},
                       {"impacts", nlohmann::json::array({{{"objectId", "program-1"},
                                                           {"objectType", "PROG"},
                                                           {"objectName", "001: Piano"},
                                                           {"partitionIndex", 0U},
                                                           {"partitionName", "Partition 0"},
                                                           {"volumeName", "Piano"},
                                                           {"role", "TARGET"},
                                                           {"status", "REQUIRED"},
                                                           {"requested", true},
                                                           {"selected", true},
                                                           {"storedSizeBytes", 512U},
                                                           {"freedClusters", 1U},
                                                           {"prerequisiteObjectIds", nlohmann::json::array()},
                                                           {"reason", "Selected object"}},
                                                          {{"objectId", "sample-1"},
                                                           {"objectType", "SBNK"},
                                                           {"objectName", "Piano C3"},
                                                           {"partitionIndex", 0U},
                                                           {"partitionName", "Partition 0"},
                                                           {"volumeName", "Piano"},
                                                           {"role", "TARGET"},
                                                           {"status", "BLOCKED"},
                                                           {"requested", true},
                                                           {"selected", false},
                                                           {"storedSizeBytes", 512U},
                                                           {"freedClusters", 1U},
                                                           {"prerequisiteObjectIds", nlohmann::json::array()},
                                                           {"reason", "A Sample Bank still refers to this Sample"}}})},
                       {"references", nlohmann::json::array()},
                       {"blockers", nlohmann::json::array({{{"code", "incoming_reference"},
                                                            {"message", "A Sample Bank still refers to this Sample"},
                                                            {"objectIds", nlohmann::json::array({"sample-1"})}}})},
                       {"warnings", nlohmann::json::array()},
                       {"estimatedFreedBytes", 1024U},
                       {"estimatedFreedClusters", 1U}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImageObjectDeletionInspection", inspection));
}

TEST(ServerContract, VolumeDeletionAndScopedPlacementRepairUseSeparateRevisionBoundedContracts) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    const auto targets = nlohmann::json::array(
        {{{"partitionIndex", 0U}, {"volumeName", "Samples"}}, {{"partitionIndex", 2U}, {"volumeName", "Programs"}}});
    const auto deletion_request =
        nlohmann::json{{"imageId", "image-1"}, {"expectedRevision", 4U}, {"targets", targets}};
    EXPECT_TRUE(
        axk::server::validate_openapi_value(document, "ImageVolumeDeletionInspectionRequest", deletion_request));
    auto invalid_partition = deletion_request;
    invalid_partition["targets"][0]["partitionIndex"] = 8U;
    EXPECT_FALSE(
        axk::server::validate_openapi_value(document, "ImageVolumeDeletionInspectionRequest", invalid_partition));
    auto duplicate_targets = deletion_request;
    duplicate_targets["targets"].push_back(duplicate_targets["targets"].front());
    EXPECT_FALSE(
        axk::server::validate_openapi_value(document, "ImageVolumeDeletionInspectionRequest", duplicate_targets));

    const auto deletion_inspection = nlohmann::json{
        {"imageId", "image-1"},
        {"revision", 4U},
        {"targets", targets},
        {"canDelete", false},
        {"crossingRelationshipCount", 1U},
        {"blockers", nlohmann::json::array({{{"code", "KNOWN_RELATIONSHIP_CROSSES_VOLUME"},
                                             {"message", "A known relationship crosses the volume boundary"},
                                             {"count", 1U}}})}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImageVolumeDeletionInspection", deletion_inspection));

    const auto partition_scope = nlohmann::json{{"kind", "PARTITION"}, {"partitionIndex", 0U}};
    const auto volume_scope = nlohmann::json{{"kind", "VOLUME"}, {"partitionIndex", 0U}, {"volumeName", "Samples"}};
    for (const auto &scope : {partition_scope, volume_scope}) {
        const auto request = nlohmann::json{{"imageId", "image-1"}, {"expectedRevision", 4U}, {"scope", scope}};
        EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImagePlacementInspectionRequest", request));
        EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImagePlacementRepairRequest", request));
    }
    auto invalid_scope = partition_scope;
    invalid_scope["volumeName"] = "Samples";
    const auto invalid_request =
        nlohmann::json{{"imageId", "image-1"}, {"expectedRevision", 4U}, {"scope", invalid_scope}};
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "ImagePlacementInspectionRequest", invalid_request));

    const auto placement_inspection =
        nlohmann::json{{"imageId", "image-1"},
                       {"revision", 4U},
                       {"scope", partition_scope},
                       {"canRepair", true},
                       {"repairObjectCount", 62U},
                       {"blockedObjectCount", 0U},
                       {"recoveryVolumeName", "Recovered"},
                       {"destinations", nlohmann::json::array({{{"volumeName", "Recovered"},
                                                                {"createsVolume", true},
                                                                {"objectCount", 62U},
                                                                {"objectTypeCounts", {{"SMPL", 62U}}}}})},
                       {"blockers", nlohmann::json::array()}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImagePlacementInspection", placement_inspection));
}

TEST(ServerContract, WaveDataOrphanInspectionIsVolumeScopedAndResponseBounded) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    const auto request =
        nlohmann::json{{"imageId", "image-1"}, {"expectedRevision", 4U}, {"contentScopeId", "content-volume-1"}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImageWaveDataOrphanInspectionRequest", request));
    auto missing_scope = request;
    missing_scope.erase("contentScopeId");
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "ImageWaveDataOrphanInspectionRequest", missing_scope));

    const auto inspection = nlohmann::json{{"imageId", "image-1"},
                                           {"revision", 4U},
                                           {"contentScopeId", "content-volume-1"},
                                           {"totalCandidateCount", 1U},
                                           {"candidates", nlohmann::json::array({{{"objectId", "object-wave-1"},
                                                                                  {"objectType", "SMPL"},
                                                                                  {"objectName", "Unused Wave"},
                                                                                  {"partitionIndex", 0U},
                                                                                  {"partitionName", "Partition 0"},
                                                                                  {"volumeName", "Volume"},
                                                                                  {"storedSizeBytes", 4096U},
                                                                                  {"recoverableBytes", 8192U},
                                                                                  {"recoverableClusters", 2U}}})}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImageWaveDataOrphanInspection", inspection));
    auto wrong_type = inspection;
    wrong_type["candidates"][0]["objectType"] = "SBNK";
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "ImageWaveDataOrphanInspection", wrong_type));
}

TEST(ServerContract, ProgramGenerationInspectionAndJobUseReviewedSelections) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    const auto inspection_request =
        nlohmann::json{{"imageId", "image-1"}, {"expectedRevision", 4U}, {"contentScopeId", "content-volume-1"}};
    EXPECT_TRUE(
        axk::server::validate_openapi_value(document, "ImageProgramGenerationInspectionRequest", inspection_request));

    const auto inspection = nlohmann::json{{"imageId", "image-1"},
                                           {"revision", 4U},
                                           {"contentScopeId", "content-volume-1"},
                                           {"availableProgramNumbers", nlohmann::json::array({2U, 5U})},
                                           {"candidates", nlohmann::json::array({{{"targetObjectId", "object-bank"},
                                                                                  {"targetObjectType", "SBAC"},
                                                                                  {"targetObjectName", "Bass Bank"},
                                                                                  {"defaultProgramName", "Bass Bnk"},
                                                                                  {"programNumber", 2U},
                                                                                  {"defaultSelected", true}},
                                                                                 {{"targetObjectId", "object-sample"},
                                                                                  {"targetObjectType", "SBNK"},
                                                                                  {"targetObjectName", "Loose"},
                                                                                  {"defaultProgramName", "Loose"},
                                                                                  {"programNumber", 5U},
                                                                                  {"defaultSelected", true}}})},
                                           {"notices", nlohmann::json::array()}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImageProgramGenerationInspection", inspection));
    auto invalid_candidate = inspection;
    invalid_candidate["candidates"][0]["targetObjectType"] = "SMPL";
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "ImageProgramGenerationInspection", invalid_candidate));

    const auto request = nlohmann::json{
        {"imageId", "image-1"},
        {"expectedRevision", 4U},
        {"contentScopeId", "content-volume-1"},
        {"programs", nlohmann::json::array(
                         {{{"targetObjectId", "object-bank"}, {"programNumber", 2U}, {"programName", "Bass Bnk"}}})}};
    EXPECT_TRUE(axk::server::validate_openapi_value(document, "ImageProgramGenerationRequest", request));
    auto invalid_name = request;
    invalid_name["programs"][0]["programName"] = " TooLong ";
    EXPECT_FALSE(axk::server::validate_openapi_value(document, "ImageProgramGenerationRequest", invalid_name));
}

TEST(ServerContract, WorkspaceCreateRequestRejectsUnknownFields) {
    axk::server::OpenApiValidator validator;
    const auto request =
        nlohmann::json{{"displayName", "Samples"}, {"path", "/samples"}, {"writable", false}, {"revision", 0U}};
    EXPECT_TRUE(validator.validate("WorkspaceCreateRequest", request));

    auto misspelled = request;
    misspelled.erase("writable");
    misspelled["writeable"] = false;
    EXPECT_FALSE(validator.validate("WorkspaceCreateRequest", misspelled));
}

TEST(ServerContract, MediaConversionRequestsAndTerminalResultsMatchTheirSchemas) {
    axk::server::OpenApiValidator validator;
    const auto inspection_request = nlohmann::json{{"imageId", "image-one"},
                                                   {"expectedRevision", 3U},
                                                   {"format", "FAT12_FLOPPY"},
                                                   {"partitionIndex", 0U},
                                                   {"volumeDirectoryId", 17U}};
    EXPECT_TRUE(validator.validate("ImageSessionMediaConversionInspectionRequest", inspection_request));
    auto invalid_request = inspection_request;
    invalid_request.erase("volumeDirectoryId");
    EXPECT_FALSE(validator.validate("ImageSessionMediaConversionInspectionRequest", invalid_request));

    const auto plan = nlohmann::json{
        {"imageId", "image-one"},
        {"revision", 3U},
        {"format", "FAT12_FLOPPY"},
        {"scope", "VOLUME"},
        {"artifactKind", "FLOPPY_DISK_SET"},
        {"outputExtension", ".zip"},
        {"floppyImageCount", 2U},
        {"partitionIndex", 0U},
        {"partitionName", "PARTITION 1"},
        {"canExport", true},
        {"objectCount", 2U},
        {"payloadBytes", 1024U},
        {"projectedOutputBytes", 2950000U},
        {"capacityBytes", 47185920U},
        {"volumes", nlohmann::json::array(
                        {{{"volumeDirectoryId", 17U}, {"name", "KIT"}, {"objectCount", 2U}, {"payloadBytes", 1024U}}})},
        {"issues",
         nlohmann::json::array({{{"code", "MEDIA_CONVERSION_MULTI_FLOPPY_SAVE_RELOAD_VALIDATION_PENDING"},
                                 {"message", "Sampler load and audition are verified; sampler save/reload validation "
                                             "is pending"},
                                 {"blocking", false},
                                 {"measurement", {{"required", 2U}, {"available", 32U}, {"unit", "FLOPPY_IMAGES"}}}}})},
        {"defaultFilename", "disk_p00_KIT.zip"}};
    EXPECT_TRUE(validator.validate("ImageSessionMediaConversionInspection", plan));

    auto result = plan;
    result["sizeBytes"] = 2950000U;
    result["destination"] = "WORKSPACE";
    result["output"] = nlohmann::json{{"rootId", "workspace"}, {"relativePath", "exports/KIT.zip"}};
    result["download"] = nullptr;
    EXPECT_TRUE(validator.validate("ImageSessionMediaConversionResult", result));
}

TEST(ServerContract, WireEnumsAreUpperSnakeAndTranslateOnlyAtTheApplicationBoundary) {
    const auto document =
        axk::server::build_openapi_document(axk::server::embedded_openapi(), axk::app::make_operation_registry());
    expect_upper_snake_enums(document.at("components").at("schemas"));
    axk::server::OpenApiValidator validator;

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
        {"totalPayloadBytes", 0},
        {"roots", nlohmann::json::array(
                      {{{"kind", "prog"}, {"displayName", "Program 1"}, {"nodeIds", nlohmann::json::array()}}})},
        {"objects", nlohmann::json::array()},
        {"relationships", nlohmann::json::array({{{"edgeId", "edge-1"},
                                                  {"sourceNodeId", "node-1"},
                                                  {"targetNodeId", "node-2"},
                                                  {"role", "PROG_ASSIGNMENT_TO_SBNK"},
                                                  {"ordinal", 0}}})},
        {"relationshipCount", 0},
        {"issues", nlohmann::json::array()}};
    const auto wire_result = validator.wire_value("PackageInspection", application_result);
    EXPECT_EQ(wire_result.at("packageKind"), "PROGRAM");
    EXPECT_EQ(wire_result.at("sourceMediaKind"), "SFS");
    EXPECT_EQ(wire_result.at("roots").at(0).at("kind"), "PROGRAM");
    EXPECT_EQ(wire_result.at("relationships").at(0).at("role"), "PROG_ASSIGNMENT_TO_SBNK");
    EXPECT_TRUE(validator.validate("PackageInspection", wire_result));
    EXPECT_EQ(validator.application_value("PackageInspection", wire_result), application_result);

    auto object_directory_result = application_result;
    object_directory_result["sourceMediaKind"] = "axk-object-directory";
    const auto object_directory_wire_result = validator.wire_value("PackageInspection", object_directory_result);
    EXPECT_EQ(object_directory_wire_result.at("sourceMediaKind"), "AXK_OBJECT_DIRECTORY");
    EXPECT_TRUE(validator.validate("PackageInspection", object_directory_wire_result));
    EXPECT_EQ(validator.application_value("PackageInspection", object_directory_wire_result), object_directory_result);
}

TEST(ServerContract, ImageSessionVolumeSelectorsUseExactContentIdentity) {
    axk::server::OpenApiValidator validator;
    const auto exact = nlohmann::json{{"kind", "VOLUME"}, {"contentId", "content-volume-1"}};
    EXPECT_TRUE(validator.validate("ImageSessionExportRoot", exact));
    EXPECT_FALSE(
        validator.validate("ImageSessionExportRoot",
                           nlohmann::json{{"kind", "VOLUME"}, {"partitionIndex", 0U}, {"volumeName", "Duplicate"}}));

    const auto request = nlohmann::json{
        {"imageId", "image-1"},
        {"expectedRevision", 1U},
        {"scopeId", "content-partition-1"},
        {"destination", {{"kind", "WORKSPACE"}, {"output", {{"rootId", "workspace"}, {"relativePath", "packages"}}}}}};
    EXPECT_TRUE(validator.validate("ImageSessionVolumePackageExportRequest", request));
    auto floppy_request = request;
    floppy_request["destination"]["output"]["relativePath"] = "floppies";
    EXPECT_TRUE(validator.validate("ImageSessionVolumeFloppyExportRequest", floppy_request));
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
