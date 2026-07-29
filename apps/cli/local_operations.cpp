#include "local_operations.hpp"

#include "exit_status.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <functional>
#include <iostream>
#include <optional>
#include <random>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/application/application_operations.hpp"
#include "axklib/application/filesystem.hpp"
#include "axklib/application/uploads.hpp"
#include "axklib/application/write_operations.hpp"
#include "axklib/utf8.hpp"
#include "commands/package_projection.hpp"
#include "local_operation_projection.hpp"

namespace {

axk::app::Error local_error(std::string code, std::string message,
                            std::optional<std::string> relative_path = std::nullopt) {
    axk::app::ErrorContext context;
    context.relative_path = std::move(relative_path);
    return {std::move(code), std::move(message), std::move(context)};
}
axk::app::Result<std::filesystem::path> normalized_absolute(const std::filesystem::path &path) {
    if (path.empty())
        return std::unexpected(local_error("invalid_local_path", "local path must not be empty"));
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    if (error)
        return std::unexpected(local_error("invalid_local_path", "local path cannot be made absolute"));
    return absolute.lexically_normal();
}
axk::app::Result<std::string> generic_relative_utf8(const std::filesystem::path &path) {
    std::string result;
    for (const auto &component : path) {
        if (component == "." || component == "..")
            return std::unexpected(local_error("invalid_local_path", "local path is not normalized"));
        auto text = axk::text::path_to_utf8(component);
        if (text.empty() || text == "/" || text == "\\")
            continue;
        if (text.find('/') != std::string::npos || text.find('\\') != std::string::npos ||
            text.find(':') != std::string::npos) {
            return std::unexpected(local_error("invalid_local_path", "local path component is not portable"));
        }
        if (!result.empty())
            result.push_back('/');
        result += text;
    }
    if (result.empty())
        return std::unexpected(local_error("invalid_local_path", "local path must name an entry"));
    return result;
}
axk::app::Result<std::filesystem::path> reserve_staging_directory() {
    std::error_code error;
    const auto parent = std::filesystem::temp_directory_path(error);
    if (error)
        return std::unexpected(local_error("local_staging_unavailable", "temporary directory is unavailable"));

    std::random_device source;
    std::uniform_int_distribution<std::uint64_t> distribution;
    for (std::size_t attempt = 0; attempt < 32U; ++attempt) {
        const auto path = parent / std::format("axklib-cli-{:016x}", distribution(source));
        if (std::filesystem::create_directory(path, error))
            return path;
        if (error && error != std::errc::file_exists)
            break;
        error.clear();
    }
    return std::unexpected(local_error("local_staging_unavailable", "temporary staging directory cannot be created"));
}
using Json = nlohmann::json;
Json file_ref_json(const axk::app::FileRef &reference) {
    return {{"rootId", reference.root_id}, {"relativePath", reference.relative_path}};
}

Json directory_ref_json(const axk::app::DirectoryRef &reference) {
    return {{"rootId", reference.root_id}, {"relativePath", reference.relative_path}};
}

Json source_refs_json(std::span<const axk::app::FileRef> sources) {
    auto result = Json::array();
    for (const auto &source : sources)
        result.push_back(file_ref_json(source));
    return result;
}

axk::app::Error projection_error(const axk::Error &error) {
    return {"invalid_application_result", axk::render_error(error)};
}

axk::app::OperationContext local_context(std::function<std::string(const axk::app::FileRef &)> display_path = {}) {
    return {.owner_id = "cli",
            .request_id = "cli",
            .cancellation = {},
            .progress = nullptr,
            .display_path = std::move(display_path)};
}

} // namespace

struct axk::cli::LocalOperationRuntime::RootMapping {
    std::string id;
    std::filesystem::path canonical_path;
};

axk::cli::LocalOperationRuntime::LocalOperationRuntime(std::vector<RootMapping> roots,
                                                       std::filesystem::path staging_directory,
                                                       std::map<std::filesystem::path, std::string> display_paths,
                                                       std::unique_ptr<app::Sandbox> sandbox,
                                                       std::unique_ptr<app::UploadStore> uploads,
                                                       app::OperationRegistry registry)
    : roots_(std::move(roots)), staging_directory_(std::move(staging_directory)),
      display_paths_(std::move(display_paths)), sandbox_(std::move(sandbox)), uploads_(std::move(uploads)),
      registry_(std::move(registry)) {}

axk::cli::LocalOperationRuntime::~LocalOperationRuntime() {
    uploads_.reset();
    std::error_code error;
    std::filesystem::remove_all(staging_directory_, error);
}

axk::app::Result<std::unique_ptr<axk::cli::LocalOperationRuntime>>
axk::cli::LocalOperationRuntime::create(std::span<const std::filesystem::path> paths) {
    if (paths.empty())
        return std::unexpected(local_error("invalid_local_path", "at least one local path is required"));

    std::vector<RootMapping> mappings;
    std::vector<app::RootDefinition> definitions;
    std::map<std::filesystem::path, std::string> display_paths;
    for (const auto &path : paths) {
        auto absolute = normalized_absolute(path);
        if (!absolute)
            return std::unexpected(absolute.error());
        std::error_code display_error;
        const auto display_key = std::filesystem::weakly_canonical(*absolute, display_error);
        if (!display_error)
            display_paths.try_emplace(display_key, axk::text::path_to_utf8(path));
        const auto root_path = absolute->root_path();
        if (root_path.empty())
            return std::unexpected(local_error("invalid_local_path", "local path has no filesystem root"));
        std::error_code error;
        const auto canonical = std::filesystem::canonical(root_path, error);
        if (error)
            return std::unexpected(local_error("invalid_local_path", "local filesystem root is unavailable"));
        if (std::ranges::find(mappings, canonical, &RootMapping::canonical_path) != mappings.end())
            continue;
        const auto id = std::format("local-{}", mappings.size());
        mappings.push_back({id, canonical});
        definitions.push_back({id, "Local filesystem", canonical, true});
    }

    auto staging = reserve_staging_directory();
    if (!staging)
        return std::unexpected(staging.error());
    definitions.push_back({"cli-staging", "CLI staging", *staging, true});
    auto sandbox = app::Sandbox::create(std::move(definitions));
    if (!sandbox) {
        std::error_code error;
        std::filesystem::remove_all(*staging, error);
        return std::unexpected(sandbox.error());
    }
    auto sandbox_pointer = std::make_unique<app::Sandbox>(std::move(*sandbox));
    auto uploads =
        std::make_unique<app::UploadStore>(*staging, 1U << 30U, 1U << 30U, 64U, 8U << 20U, std::chrono::minutes{30});
    auto registry = app::make_application_registry(*sandbox_pointer, *uploads);
    if (!registry) {
        std::error_code error;
        std::filesystem::remove_all(*staging, error);
        return std::unexpected(registry.error());
    }
    return std::unique_ptr<LocalOperationRuntime>{
        new LocalOperationRuntime{std::move(mappings), std::move(*staging), std::move(display_paths),
                                  std::move(sandbox_pointer), std::move(uploads), std::move(*registry)}};
}

axk::app::Result<axk::app::FileRef>
axk::cli::LocalOperationRuntime::reference(const std::filesystem::path &path) const {
    auto absolute = normalized_absolute(path);
    if (!absolute)
        return std::unexpected(absolute.error());
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(*absolute, error);
    if (error)
        return std::unexpected(local_error("invalid_local_path", "local path cannot be resolved"));
    for (const auto &root : roots_) {
        const auto relative = canonical.lexically_relative(root.canonical_path);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
            continue;
        auto encoded = generic_relative_utf8(relative);
        if (!encoded)
            return std::unexpected(encoded.error());
        return app::FileRef{root.id, std::move(*encoded)};
    }
    return std::unexpected(local_error("invalid_local_path", "local path is outside the selected filesystem roots"));
}

axk::app::Result<axk::app::FileRef> axk::cli::LocalOperationRuntime::file_ref(const std::filesystem::path &path) const {
    return reference(path);
}

axk::app::Result<axk::app::DirectoryRef>
axk::cli::LocalOperationRuntime::directory_ref(const std::filesystem::path &path) const {
    auto file = reference(path);
    if (!file)
        return std::unexpected(file.error());
    return app::DirectoryRef{std::move(file->root_id), std::move(file->relative_path)};
}

axk::app::FileRef axk::cli::LocalOperationRuntime::scratch_file_ref(std::string filename) const {
    return {"cli-staging", std::move(filename)};
}

axk::app::Result<std::filesystem::path>
axk::cli::LocalOperationRuntime::resolve_file(const app::FileRef &reference) const {
    return sandbox_->resolve_file(reference);
}

std::string axk::cli::LocalOperationRuntime::display_path(const app::FileRef &reference) const {
    auto resolved = sandbox_->resolve_file(reference);
    if (!resolved)
        return reference.relative_path;
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(*resolved, error);
    if (!error) {
        if (const auto found = display_paths_.find(canonical); found != display_paths_.end())
            return found->second;
    }
    return reference.relative_path;
}

axk::app::Result<axk::cli::ReportResult>
axk::cli::LocalOperationRuntime::report(std::string_view operation_id, std::span<const app::FileRef> sources,
                                        const app::DirectoryRef &destination, bool overwrite, bool strict,
                                        bool include_payloads, bool pretty,
                                        const std::optional<std::string> &object_type) const {
    Json input{{"sources", source_refs_json(sources)},
               {"destination", directory_ref_json(destination)},
               {"overwrite", overwrite}};
    if (operation_id == "report.objects") {
        input["strict"] = strict;
        input["includePayloads"] = include_payloads;
        input["pretty"] = pretty;
        if (object_type)
            input["objectType"] = *object_type;
    } else if (operation_id == "report.inventory") {
        input["strict"] = strict;
    }
    auto result = registry_.invoke(
        operation_id, input, local_context([this](const app::FileRef &reference) { return display_path(reference); }));
    if (!result)
        return std::unexpected(result.error());
    return ReportResult{.source_count = result->at("sourceCount").get<std::size_t>(),
                        .loaded_count = result->at("loadedCount").get<std::size_t>(),
                        .failed_count = result->at("failedCount").get<std::size_t>(),
                        .row_count = result->at("rowCount").get<std::size_t>(),
                        .ambiguous_count = result->value("ambiguousCount", std::size_t{}),
                        .decode_issue_count = result->value("decodeIssueCount", std::size_t{})};
}

axk::app::Result<std::vector<axk::cli::OrphanSummary>>
axk::cli::LocalOperationRuntime::report_orphans(std::span<const app::FileRef> sources,
                                                const app::DirectoryRef &destination, bool overwrite) const {
    auto result =
        registry_.invoke("report.orphans",
                         {{"sources", source_refs_json(sources)},
                          {"destination", directory_ref_json(destination)},
                          {"overwrite", overwrite}},
                         local_context([this](const app::FileRef &reference) { return display_path(reference); }));
    if (!result)
        return std::unexpected(result.error());
    std::vector<OrphanSummary> summaries;
    for (const auto &summary : result->at("summaries")) {
        summaries.push_back(
            {.source_path = summary.at("sourcePath").get<std::string>(),
             .waveform_count = summary.at("waveformCount").get<std::size_t>(),
             .referenced_count = summary.at("referencedCount").get<std::size_t>(),
             .known_unreferenced_count = summary.at("knownUnreferencedCount").get<std::size_t>(),
             .ambiguous_or_unresolved_count = summary.at("ambiguousOrUnresolvedCount").get<std::size_t>()});
    }
    return summaries;
}

axk::app::Result<axk::cli::ValidationResult> axk::cli::LocalOperationRuntime::report_validation(
    std::span<const app::FileRef> sources, const app::DirectoryRef &destination,
    const std::optional<app::DirectoryRef> &exports, std::string_view policy, bool overwrite) const {
    Json input{{"sources", source_refs_json(sources)},
               {"destination", directory_ref_json(destination)},
               {"policy", policy},
               {"overwrite", overwrite}};
    if (exports)
        input["exports"] = directory_ref_json(*exports);
    auto result =
        registry_.invoke("report.validate", input,
                         local_context([this](const app::FileRef &reference) { return display_path(reference); }));
    if (!result)
        return std::unexpected(result.error());
    return ValidationResult{.issue_count = result->at("issueCount").get<std::size_t>(),
                            .failed = result->at("failed").get<bool>(),
                            .policy = result->at("policy").get<std::string>()};
}

axk::app::Result<axk::cli::CorpusAuditResult> axk::cli::LocalOperationRuntime::corpus_audit(
    std::span<const app::FileRef> sources, const app::DirectoryRef &destination, std::string_view policy,
    std::size_t wave_smoke_limit, bool skip_wave_smoke, bool overwrite) const {
    auto result =
        registry_.invoke("corpus.audit",
                         {{"sources", source_refs_json(sources)},
                          {"destination", directory_ref_json(destination)},
                          {"policy", policy},
                          {"waveSmokeLimit", wave_smoke_limit},
                          {"skipWaveSmoke", skip_wave_smoke},
                          {"overwrite", overwrite}},
                         local_context([this](const app::FileRef &reference) { return display_path(reference); }));
    if (!result)
        return std::unexpected(result.error());
    return CorpusAuditResult{.loaded_count = result->at("loadedCount").get<std::size_t>(),
                             .failed_count = result->at("failedCount").get<std::size_t>(),
                             .object_count = result->at("objectCount").get<std::size_t>(),
                             .validation_issue_count = result->at("validationIssueCount").get<std::size_t>(),
                             .relationship_count = result->at("relationshipCount").get<std::size_t>(),
                             .wave_smoke_decoded = result->at("waveSmokeDecoded").get<std::size_t>(),
                             .wave_smoke_error_count = result->at("waveSmokeErrorCount").get<std::size_t>(),
                             .validation_failed = result->at("validationFailed").get<bool>()};
}

axk::app::Result<axk::cli::schema::info_v1::InfoOutput>
axk::cli::LocalOperationRuntime::info(std::span<const LocalInfoSource> sources, bool strict,
                                      bool include_default_programs) const {
    auto encoded = Json::array();
    for (const auto &source : sources) {
        if (source.object_directory) {
            encoded.push_back(
                {{"kind", "AXK_OBJECT_DIRECTORY"}, {"directory", directory_ref_json(*source.object_directory)}});
        } else if (source.file) {
            encoded.push_back({{"kind", "FILE"}, {"file", file_ref_json(*source.file)}});
        } else {
            return std::unexpected(local_error("invalid_local_source", "info source has no file or directory"));
        }
    }
    auto result = registry_.invoke(
        "report.info",
        {{"sources", std::move(encoded)}, {"strict", strict}, {"includeDefaultPrograms", include_default_programs}},
        local_context([this](const app::FileRef &reference) { return display_path(reference); }));
    if (!result)
        return std::unexpected(result.error());
    return local_projection::info_output(*result);
}

axk::app::Result<axk::cli::ExtractionResult> axk::cli::LocalOperationRuntime::extract(
    bool sfz, std::span<const app::FileRef> sources, const app::DirectoryRef &destination, std::string_view scope,
    std::span<const std::string> selectors, std::string_view stereo, bool overwrite, bool strict) const {
    auto result =
        registry_.invoke(sfz ? "extract.sfz" : "extract.wav",
                         {{"sources", source_refs_json(sources)},
                          {"destination", directory_ref_json(destination)},
                          {"scope", scope},
                          {"selectors", selectors},
                          {"stereo", stereo},
                          {"overwrite", overwrite},
                          {"strict", strict}},
                         local_context([this](const app::FileRef &reference) { return display_path(reference); }));
    if (!result)
        return std::unexpected(result.error());
    ExtractionResult projected{.waveform_count = result->at("waveformCount").get<std::size_t>(),
                               .written_file_count = result->at("writtenFileCount").get<std::size_t>(),
                               .selection_graph_count = result->at("selectionGraphCount").get<std::size_t>(),
                               .sfz_file_count = result->at("sfzFileCount").get<std::size_t>(),
                               .decode_error_count = result->at("decodeErrorCount").get<std::size_t>(),
                               .load_error_count = result->at("loadErrorCount").get<std::size_t>(),
                               .warnings = {},
                               .artifacts = {}};
    for (const auto &warning : result->at("warnings")) {
        projected.warnings.push_back(
            {.code = warning.at("code").get<std::string>(), .message = warning.at("message").get<std::string>()});
    }
    for (const auto &artifact : result->at("artifacts")) {
        projected.artifacts.push_back({.relative_path = artifact.at("relativePath").get<std::string>(),
                                       .sha256 = artifact.at("sha256").get<std::string>()});
    }
    return projected;
}

axk::app::Result<axk::cli::schema::package_v1::PackageOutput>
axk::cli::LocalOperationRuntime::package_export(const app::FileRef &source, const app::FileRef &output,
                                                std::span<const PackageRootSelector> roots, bool overwrite) const {
    auto encoded_roots = Json::array();
    for (const auto &root : roots) {
        Json value{{"kind", std::string{package_root_kind_name(root.kind)}},
                   {"groupName", root.group_name},
                   {"volumeName", root.volume_name},
                   {"objectName", root.object_name}};
        if (root.partition_index)
            value["partitionIndex"] = static_cast<std::uint32_t>(*root.partition_index);
        encoded_roots.push_back(std::move(value));
    }
    auto result =
        registry_.invoke("package.export",
                         {{"source", file_ref_json(source)},
                          {"output", file_ref_json(output)},
                          {"roots", std::move(encoded_roots)},
                          {"overwrite", overwrite}},
                         local_context([this](const app::FileRef &reference) { return display_path(reference); }));
    if (!result)
        return std::unexpected(result.error());
    const auto &effective = result->at("output");
    auto resolved =
        resolve_file({effective.at("rootId").get<std::string>(), effective.at("relativePath").get<std::string>()});
    if (!resolved)
        return std::unexpected(resolved.error());
    auto projected = schema::package_v1::project_package(*resolved, *result);
    if (!projected)
        return std::unexpected(projection_error(projected.error()));
    return *projected;
}

axk::app::Result<axk::cli::schema::package_v1::PackageOutput>
axk::cli::LocalOperationRuntime::package_inspect(const std::filesystem::path &package_path, const app::FileRef &package,
                                                 bool verify) const {
    auto result = registry_.invoke(
        verify ? "package.verify" : "package.inspect", {{"package", {{"fileRef", file_ref_json(package)}}}},
        local_context([this](const app::FileRef &reference) { return display_path(reference); }));
    if (!result)
        return std::unexpected(result.error());
    auto projected = schema::package_v1::project_package(package_path, *result);
    if (!projected)
        return std::unexpected(projection_error(projected.error()));
    return *projected;
}

axk::app::Result<axk::cli::schema::package_v1::PlanOutput> axk::cli::LocalOperationRuntime::package_import(
    const std::filesystem::path &target_path, std::span<const std::filesystem::path> package_paths,
    const app::FileRef &target, const app::FileRef &output, std::span<const app::FileRef> packages,
    const axk::PackageImportRequest &request, bool apply, bool overwrite) const {
    auto package_inputs = Json::array();
    for (const auto &package : packages)
        package_inputs.push_back({{"fileRef", file_ref_json(package)}});
    auto destinations = Json::array();
    for (const auto &destination : request.root_destinations) {
        Json value{{"packageIndex", destination.package_index}, {"rootIndex", destination.root_index},
                   {"groupName", destination.group_name},       {"volumeName", destination.volume_name},
                   {"rawGroup", destination.raw_group},         {"rawVolume", destination.raw_volume},
                   {"create", destination.create_destination}};
        if (destination.partition_index)
            value["partitionIndex"] = static_cast<std::uint32_t>(*destination.partition_index);
        destinations.push_back(std::move(value));
    }
    auto renames = Json::array();
    for (const auto &rename : request.policy.renames) {
        renames.push_back({{"packageIndex", rename.package_index},
                           {"nodeId", rename.node_id},
                           {"destinationName", rename.destination_name}});
    }
    auto plan =
        registry_.invoke("package.plan_import",
                         {{"target", file_ref_json(target)},
                          {"output", file_ref_json(output)},
                          {"packages", std::move(package_inputs)},
                          {"destinations", std::move(destinations)},
                          {"renames", std::move(renames)},
                          {"overwrite", overwrite}},
                         local_context([this](const app::FileRef &reference) { return display_path(reference); }));
    if (!plan)
        return std::unexpected(plan.error());
    std::optional<std::filesystem::path> output_path;
    std::optional<Json> application_result;
    if (apply && plan->at("valid").get<bool>()) {
        auto applied =
            registry_.invoke("package.import", {{"planToken", plan->at("planToken").get<std::string>()}},
                             local_context([this](const app::FileRef &reference) { return display_path(reference); }));
        if (!applied)
            return std::unexpected(applied.error());
        application_result = std::move(*applied);
        const auto &effective = application_result->at("output");
        auto resolved =
            resolve_file({effective.at("rootId").get<std::string>(), effective.at("relativePath").get<std::string>()});
        if (!resolved)
            return std::unexpected(resolved.error());
        output_path = std::move(*resolved);
    }
    if (!application_result) {
        auto released =
            registry_.invoke("package.plan_import.release", {{"planToken", plan->at("planToken")}}, local_context());
        if (!released)
            return std::unexpected(released.error());
    }
    const std::vector<std::filesystem::path> package_path_vector{package_paths.begin(), package_paths.end()};
    auto projected = schema::package_v1::project_plan(target_path, package_path_vector, *plan, output_path,
                                                      application_result ? &*application_result : nullptr);
    if (!projected)
        return std::unexpected(projection_error(projected.error()));
    return *projected;
}

axk::app::Result<axk::cli::ImageCreationResult> axk::cli::create_image(std::string_view kind,
                                                                       const std::filesystem::path &manifest_path,
                                                                       const std::filesystem::path &output_path,
                                                                       bool overwrite, bool apply) {
    auto prepared = app::prepare_local_build_manifest(kind, manifest_path);
    if (!prepared)
        return std::unexpected(app::Error{"manifest_invalid", axk::render_error(prepared.error())});
    std::vector<std::filesystem::path> runtime_paths{output_path};
    for (const auto &binding : prepared->bindings)
        runtime_paths.push_back(binding.input_path);
    auto runtime = LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return std::unexpected(runtime.error());
    auto output = (*runtime)->file_ref(output_path);
    if (!output)
        return std::unexpected(output.error());
    auto bindings = Json::array();
    for (const auto &binding : prepared->bindings) {
        auto source = (*runtime)->file_ref(binding.input_path);
        if (!source)
            return std::unexpected(source.error());
        bindings.push_back({{"manifestPath", binding.manifest_path}, {"input", {{"fileRef", file_ref_json(*source)}}}});
    }
    auto plan = (*runtime)->registry_.invoke("create.plan",
                                             {{"kind", kind},
                                              {"manifest", {{"inline", std::move(prepared->manifest)}}},
                                              {"inputBindings", std::move(bindings)},
                                              {"output", file_ref_json(*output)},
                                              {"overwrite", overwrite}},
                                             local_context());
    if (!plan)
        return std::unexpected(plan.error());
    const auto &summary = plan->at("summary");
    ImageCreationResult result{
        .plan =
            {
                .kind = plan->at("kind").get<std::string>(),
                .format = summary.at("format").get<std::string>(),
                .output_path = output_path,
                .overwrite = plan->at("overwrite").get<bool>(),
                .object_count = summary.at("objectCount").get<std::size_t>(),
                .size_bytes = summary.contains("sizeBytes")
                                  ? std::optional<std::uint64_t>{summary.at("sizeBytes").get<std::uint64_t>()}
                                  : std::nullopt,
                .partition_count = summary.contains("partitionCount")
                                       ? std::optional<std::size_t>{summary.at("partitionCount").get<std::size_t>()}
                                       : std::nullopt,
            },
        .written = std::nullopt,
    };
    if (!apply)
        return result;
    auto operation = std::string{"create."};
    operation += kind == "HDS" ? "hds" : kind == "FLOPPY" ? "floppy" : "iso";
    auto written = (*runtime)->registry_.invoke(operation, {{"planToken", plan->at("planToken")}}, local_context());
    if (!written)
        return std::unexpected(written.error());
    result.written = ImageWriteResult{.size_bytes = written->at("sizeBytes").get<std::uint64_t>(),
                                      .object_count = written->at("objectCount").get<std::size_t>(),
                                      .unused_tail_sectors = written->value("unusedTailSectors", std::uint64_t{}),
                                      .partitions = {}};
    if (const auto partitions = written->find("partitions"); partitions != written->end()) {
        for (const auto &partition : *partitions) {
            result.written->partitions.push_back({.index = partition.at("index").get<std::uint32_t>(),
                                                  .name = partition.at("name").get<std::string>(),
                                                  .start_sector = partition.at("startSector").get<std::uint32_t>(),
                                                  .sector_count = partition.at("sectorCount").get<std::uint32_t>(),
                                                  .cluster_count = partition.at("clusterCount").get<std::uint32_t>(),
                                                  .free_kib = partition.at("freeKiB").get<std::uint64_t>()});
        }
    }
    return result;
}

axk::app::Result<axk::cli::schema::operations_v1::AlterationOutput>
axk::cli::alter_image(const std::filesystem::path &source_path, const std::filesystem::path &manifest_path,
                      const std::optional<std::filesystem::path> &output_path) {
    auto prepared = app::prepare_local_alteration_manifest(manifest_path);
    if (!prepared)
        return std::unexpected(app::Error{"manifest_invalid", axk::render_error(prepared.error())});
    std::vector<std::filesystem::path> runtime_paths{source_path};
    if (output_path)
        runtime_paths.push_back(*output_path);
    for (const auto &binding : prepared->bindings)
        runtime_paths.push_back(binding.input_path);
    auto runtime = LocalOperationRuntime::create(runtime_paths);
    if (!runtime)
        return std::unexpected(runtime.error());
    auto source = (*runtime)->file_ref(source_path);
    if (!source)
        return std::unexpected(source.error());
    auto bindings = Json::array();
    for (const auto &binding : prepared->bindings) {
        auto input = (*runtime)->file_ref(binding.input_path);
        if (!input)
            return std::unexpected(input.error());
        bindings.push_back({{"manifestPath", binding.manifest_path}, {"input", {{"fileRef", file_ref_json(*input)}}}});
    }
    Json request{{"source", file_ref_json(*source)},
                 {"manifest", {{"inline", std::move(prepared->manifest)}}},
                 {"inputBindings", std::move(bindings)}};
    if (output_path) {
        auto output = (*runtime)->file_ref(*output_path);
        if (!output)
            return std::unexpected(output.error());
        request["output"] = file_ref_json(*output);
        request["overwrite"] = false;
    }
    auto altered = (*runtime)->registry_.invoke(output_path ? "alter.hds" : "alter.inspect", request, local_context());
    if (!altered)
        return std::unexpected(altered.error());
    schema::operations_v1::AlterationOutput result{
        .source_path_utf8 = axk::text::path_to_utf8(source_path),
        .output_path_utf8 = output_path ? std::optional{axk::text::path_to_utf8(*output_path)} : std::nullopt,
        .applied = output_path.has_value(),
        .operations = {}};
    for (auto &operation : altered->at("operations")) {
        auto &audio = operation.at("audioImport");
        if (!audio.is_null()) {
            const auto &logical = audio.at("sourcePath").get_ref<const std::string &>();
            const auto binding =
                std::ranges::find(prepared->bindings, logical, &app::LocalManifestInputBinding::manifest_path);
            if (binding != prepared->bindings.end())
                audio["sourcePath"] = axk::text::path_to_utf8(binding->input_path);
        }
        result.operations.push_back(local_projection::operation_output(operation));
    }
    return result;
}

axk::app::Result<std::string> axk::cli::create_manifest_document(const app::OperationRegistry &registry,
                                                                 std::string_view kind) {
    auto manifest = registry.invoke("create.manifest", {{"kind", kind}}, local_context());
    if (!manifest)
        return std::unexpected(manifest.error());
    return manifest->at("canonicalJson").get<std::string>();
}

axk::app::Result<std::string> axk::cli::alteration_manifest_document(const app::OperationRegistry &registry) {
    auto manifest = registry.invoke("alter.manifest", Json::object(), local_context());
    if (!manifest)
        return std::unexpected(manifest.error());
    return manifest->at("canonicalJson").get<std::string>();
}

int axk::cli::report_application_failure(const app::Error &error) {
    std::cerr << error.code << ": " << error.message;
    if (error.context.relative_path)
        std::cerr << " [path=" << *error.context.relative_path << ']';
    std::cerr << '\n';
    return exit_code(application_error_status(error.code));
}
