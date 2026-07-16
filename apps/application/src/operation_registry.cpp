#include "axklib/application/operation_registry.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include "axklib/application/system_service.hpp"

namespace {

using axk::app::ExecutionMode;
using axk::app::HttpMethod;
using axk::app::OperationClass;
using axk::app::OperationDescriptor;

const std::array descriptors{
    OperationDescriptor{"system.version",
                        "axklib --version",
                        HttpMethod::get,
                        "/api/v1/system/version",
                        ExecutionMode::request,
                        {},
                        "EmptyRequest",
                        "VersionResponse"},
    OperationDescriptor{"report.info",
                        "axklib info",
                        HttpMethod::post,
                        "/api/v1/reports/info",
                        ExecutionMode::job,
                        {},
                        "InfoRequest",
                        "InfoResult"},
    OperationDescriptor{"report.objects",
                        "axklib objects",
                        HttpMethod::post,
                        "/api/v1/reports/objects",
                        ExecutionMode::job,
                        {},
                        "ObjectsRequest",
                        "ReportResult"},
    OperationDescriptor{"report.relationships",
                        "axklib relationships",
                        HttpMethod::post,
                        "/api/v1/reports/relationships",
                        ExecutionMode::job,
                        {},
                        "RelationshipsRequest",
                        "RelationshipsResult"},
    OperationDescriptor{"report.inventory",
                        "axklib inventory",
                        HttpMethod::post,
                        "/api/v1/reports/inventory",
                        ExecutionMode::job,
                        {},
                        "InventoryRequest",
                        "InventoryResult"},
    OperationDescriptor{"report.coverage",
                        "axklib coverage",
                        HttpMethod::post,
                        "/api/v1/reports/coverage",
                        ExecutionMode::job,
                        {},
                        "CoverageRequest",
                        "ReportResult"},
    OperationDescriptor{"corpus.audit",
                        "axklib corpus audit",
                        HttpMethod::post,
                        "/api/v1/corpus-audits",
                        ExecutionMode::job,
                        {},
                        "CorpusAuditRequest",
                        "CorpusAuditResult"},
    OperationDescriptor{"extract.wav", "axklib extract wav", HttpMethod::post, "/api/v1/extractions",
                        ExecutionMode::job, "WAV", "ExtractionRequest", "ExtractionResult", OperationClass::write,
                        true},
    OperationDescriptor{"extract.sfz", "axklib extract sfz", HttpMethod::post, "/api/v1/extractions",
                        ExecutionMode::job, "SFZ", "ExtractionRequest", "ExtractionResult", OperationClass::write,
                        true},
    OperationDescriptor{"package.export",
                        "axklib package export",
                        HttpMethod::post,
                        "/api/v1/package-exports",
                        ExecutionMode::job,
                        {},
                        "PackageExportRequest",
                        "PackageExportResult",
                        OperationClass::write,
                        true},
    OperationDescriptor{"package.inspect",
                        "axklib package inspect",
                        HttpMethod::post,
                        "/api/v1/package-inspections",
                        ExecutionMode::request,
                        {},
                        "PackageReadRequest",
                        "PackageInspection"},
    OperationDescriptor{"package.verify",
                        "axklib package verify",
                        HttpMethod::post,
                        "/api/v1/package-verifications",
                        ExecutionMode::request,
                        {},
                        "PackageReadRequest",
                        "PackageVerification"},
    OperationDescriptor{"package.plan_import",
                        "axklib package plan-import",
                        HttpMethod::post,
                        "/api/v1/package-import-plans",
                        ExecutionMode::request,
                        {},
                        "PackageImportPlanRequest",
                        "PackageImportPlan"},
    OperationDescriptor{"package.import",
                        "axklib package import",
                        HttpMethod::post,
                        "/api/v1/package-imports",
                        ExecutionMode::job,
                        {},
                        "PackageImportRequest",
                        "PackageImportResult",
                        OperationClass::write,
                        true},
    OperationDescriptor{"report.orphans",
                        "axklib orphans",
                        HttpMethod::post,
                        "/api/v1/reports/orphans",
                        ExecutionMode::job,
                        {},
                        "OrphansRequest",
                        "OrphansResult"},
    OperationDescriptor{"report.validate",
                        "axklib validate",
                        HttpMethod::post,
                        "/api/v1/reports/validation",
                        ExecutionMode::job,
                        {},
                        "ValidationRequest",
                        "ValidationResult"},
    OperationDescriptor{"create.plan",
                        {},
                        HttpMethod::post,
                        "/api/v1/image-build-plans",
                        ExecutionMode::request,
                        {},
                        "ImageBuildPlanRequest",
                        "ImageBuildPlan",
                        OperationClass::read,
                        false,
                        false},
    OperationDescriptor{"create.hds", "axklib create hds", HttpMethod::post, "/api/v1/image-builds", ExecutionMode::job,
                        "HDS", "ImageBuildRequest", "ImageBuildResult", OperationClass::write, true},
    OperationDescriptor{"create.floppy", "axklib create floppy", HttpMethod::post, "/api/v1/image-builds",
                        ExecutionMode::job, "FLOPPY", "ImageBuildRequest", "ImageBuildResult", OperationClass::write,
                        true},
    OperationDescriptor{"create.iso", "axklib create iso", HttpMethod::post, "/api/v1/image-builds", ExecutionMode::job,
                        "ISO", "ImageBuildRequest", "ImageBuildResult", OperationClass::write, true},
    OperationDescriptor{"create.manifest",
                        "axklib create manifest",
                        HttpMethod::post,
                        "/api/v1/manifest-templates",
                        ExecutionMode::request,
                        {},
                        "ManifestTemplateRequest",
                        "ManifestTemplateResult"},
    OperationDescriptor{"alter.plan",
                        {},
                        HttpMethod::post,
                        "/api/v1/image-alteration-plans",
                        ExecutionMode::request,
                        {},
                        "ImageAlterationPlanRequest",
                        "ImageAlterationPlan",
                        OperationClass::read,
                        false,
                        false},
    OperationDescriptor{"alter.hds",
                        "axklib alter hds",
                        HttpMethod::post,
                        "/api/v1/image-alterations",
                        ExecutionMode::job,
                        {},
                        "ImageAlterationRequest",
                        "ImageAlterationResult",
                        OperationClass::write,
                        true},
    OperationDescriptor{"alter.manifest",
                        "axklib alter manifest",
                        HttpMethod::post,
                        "/api/v1/alteration-manifest-templates",
                        ExecutionMode::request,
                        {},
                        "AlterationManifestTemplateRequest",
                        "ManifestTemplateResult"},
};

axk::app::Error registry_error(std::string code, std::string message) { return {std::move(code), std::move(message)}; }

} // namespace

axk::app::Result<void> axk::app::OperationRegistry::declare(OperationDescriptor descriptor) {
    if (descriptor.id.empty() || (descriptor.cli_parity && descriptor.cli_command.empty()) ||
        descriptor.route.empty() || descriptor.request_schema.empty() || descriptor.result_schema.empty()) {
        return std::unexpected(registry_error("invalid_operation_descriptor", "operation metadata is incomplete"));
    }
    if (!descriptor.route.starts_with("/api/v1/")) {
        return std::unexpected(registry_error("invalid_operation_descriptor", "operation route must be in the v1 API"));
    }
    if (by_id_.contains(descriptor.id))
        return std::unexpected(registry_error("duplicate_operation", "operation ID is already registered"));
    if (!descriptor.cli_command.empty() &&
        std::ranges::any_of(operations_, [&descriptor](const RegisteredOperation &operation) {
            return operation.descriptor.cli_command == descriptor.cli_command;
        })) {
        return std::unexpected(registry_error("duplicate_operation", "CLI command path is already registered"));
    }
    by_id_.emplace(descriptor.id, operations_.size());
    operations_.push_back({std::move(descriptor), {}, 0U});
    return {};
}

axk::app::Result<void> axk::app::OperationRegistry::bind(std::string_view operation_id, Handler handler) {
    const auto found = by_id_.find(std::string{operation_id});
    if (found == by_id_.end())
        return std::unexpected(registry_error("unknown_operation", "operation is not registered"));
    if (!handler)
        return std::unexpected(registry_error("invalid_operation_handler", "operation handler is empty"));
    auto &operation = operations_[found->second];
    if (operation.handler)
        return std::unexpected(registry_error("duplicate_operation_handler", "operation already has a handler"));
    operation.handler_type_hash = handler.target_type().hash_code();
    operation.handler = std::move(handler);
    return {};
}

axk::app::Result<axk::app::OperationRegistry::Json>
axk::app::OperationRegistry::invoke(std::string_view operation_id, const Json &input,
                                    const OperationContext &context) const {
    const auto found = by_id_.find(std::string{operation_id});
    if (found == by_id_.end())
        return std::unexpected(registry_error("unknown_operation", "operation is not registered"));
    const auto &operation = operations_[found->second];
    if (!operation.handler) {
        return std::unexpected(
            registry_error("operation_not_implemented", "operation is registered but not implemented"));
    }
    return operation.handler(input, context);
}

std::vector<axk::app::OperationEntry> axk::app::OperationRegistry::entries() const {
    std::vector<OperationEntry> result;
    result.reserve(operations_.size());
    for (const auto &operation : operations_)
        result.push_back({operation.descriptor, static_cast<bool>(operation.handler), operation.handler_type_hash});
    return result;
}

const axk::app::OperationDescriptor *axk::app::OperationRegistry::find(std::string_view operation_id) const noexcept {
    const auto found = by_id_.find(std::string{operation_id});
    return found == by_id_.end() ? nullptr : &operations_[found->second].descriptor;
}

bool axk::app::OperationRegistry::is_implemented(std::string_view operation_id) const noexcept {
    const auto found = by_id_.find(std::string{operation_id});
    return found != by_id_.end() && static_cast<bool>(operations_[found->second].handler);
}

axk::app::OperationRegistry axk::app::make_operation_registry() {
    OperationRegistry registry;
    for (const auto &descriptor : descriptors) {
        if (!registry.declare(descriptor))
            std::terminate();
    }
    if (!registry.bind_typed<EmptyRequest, VersionResponse>("system.version", system_version))
        std::terminate();
    return registry;
}

std::string_view axk::app::http_method_name(HttpMethod method) noexcept {
    switch (method) {
    case HttpMethod::get:
        return "GET";
    case HttpMethod::post:
        return "POST";
    }
    return "POST";
}

std::string_view axk::app::execution_mode_name(ExecutionMode mode) noexcept {
    switch (mode) {
    case ExecutionMode::request:
        return "request";
    case ExecutionMode::job:
        return "job";
    }
    return "request";
}

std::string_view axk::app::operation_class_name(OperationClass operation_class) noexcept {
    switch (operation_class) {
    case OperationClass::read:
        return "read";
    case OperationClass::write:
        return "write";
    }
    return "read";
}
