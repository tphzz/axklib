#include "axklib/application/write_operations.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/alteration_transaction.hpp"
#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/secure_random.hpp"
#include "axklib/file_publication.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/utf8.hpp"
#include "axklib/version.hpp"
#include "axklib/writer.hpp"
#include <nlohmann/json.hpp>

#include "content_digest.hpp"

#include "write_operations_internal.hpp"

using namespace axk::app::write_operations_internal;
using axk::app::detail::file_sha256;
using axk::app::detail::reader_sha256;

axk::app::Result<void> axk::app::bind_write_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                       UploadStore &uploads,
                                                       const axk::MediaBuildLimits &media_limits) {
    if (auto bound = bind_manifest_operations(registry); !bound)
        return bound;

    auto state = std::make_shared<WriteOperationState>();

    OperationRegistry::Handler create_plan_handler =
        [state, &sandbox, &uploads, media_limits](const Json &input, const OperationContext &context) {
            std::string kind_text;
            try {
                kind_text = input.at("kind").get<std::string>();
            } catch (const Json::exception &) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "kind is required"))};
            }
            auto manifest_kind = parse_build_kind(kind_text);
            if (!manifest_kind)
                return Result<Json>{std::unexpected(manifest_kind.error())};
            auto output = parse_file_ref(input, "output");
            if (!output)
                return Result<Json>{std::unexpected(output.error())};
            const auto overwrite = input.value("overwrite", false);
            auto output_path = sandbox.resolve_output_file(*output, overwrite);
            if (!output_path)
                return Result<Json>{std::unexpected(output_path.error())};
            auto document = load_manifest(input, context, sandbox, uploads);
            if (!document)
                return Result<Json>{std::unexpected(document.error())};

            const auto serialized = document->json.dump();
            Json summary;
            std::variant<axk::HdsBuildManifest, axk::MediaBuildManifest> manifest;
            std::vector<std::filesystem::path> required_paths;
            if (*manifest_kind == axk::BuildManifestKind::hds) {
                auto parsed = axk::parse_hds_build_manifest(serialized);
                if (!parsed)
                    return Result<Json>{std::unexpected(core_error(parsed.error()))};
                required_paths = external_paths(*parsed);
                if (auto admitted = require_bound_inputs(required_paths, document->bound_input_paths); !admitted)
                    return Result<Json>{std::unexpected(admitted.error())};
                auto planned = axk::plan_hds_build(*parsed, context.cancellation);
                if (!planned)
                    return Result<Json>{std::unexpected(core_error(planned.error()))};
                summary = {{"format", "HDS"},
                           {"sizeBytes", planned->size_bytes},
                           {"partitionCount", planned->partition_count},
                           {"objectCount", planned->object_count}};
                manifest = std::move(*parsed);
            } else {
                auto parsed = axk::parse_media_build_manifest(serialized);
                if (!parsed)
                    return Result<Json>{std::unexpected(core_error(parsed.error()))};
                const auto expected_format = *manifest_kind == axk::BuildManifestKind::fat12_floppy
                                                 ? axk::MediaImageFormat::fat12_floppy
                                                 : axk::MediaImageFormat::iso9660;
                if (parsed->format != expected_format) {
                    return Result<Json>{std::unexpected(
                        operation_error("manifest_kind_mismatch", "manifest format does not match image build"))};
                }
                if (parsed->transfer && contains_path(document->upload_input_paths, parsed->transfer->source_path)) {
                    return Result<Json>{std::unexpected(operation_error(
                        "whole_source_requires_file_ref", "whole-source transfer input must be a persistent FileRef"))};
                }
                required_paths = external_paths(*parsed);
                if (auto admitted = require_bound_inputs(required_paths, document->bound_input_paths); !admitted)
                    return Result<Json>{std::unexpected(admitted.error())};
                auto planned = axk::plan_media_build(*parsed, media_limits, context.cancellation);
                if (!planned)
                    return Result<Json>{std::unexpected(core_error(planned.error()))};
                summary = {{"format", write_plan_kind_name(write_plan_kind(*manifest_kind))},
                           {"objectCount", planned->object_count}};
                manifest = std::move(*parsed);
            }

            auto fingerprint_paths = document->observed_paths;
            fingerprint_paths.insert(fingerprint_paths.end(), required_paths.begin(), required_paths.end());
            auto fingerprints = fingerprint_files(fingerprint_paths, context.cancellation);
            if (!fingerprints)
                return Result<Json>{std::unexpected(fingerprints.error())};
            std::error_code filesystem_error;
            const auto output_existed = std::filesystem::exists(*output_path, filesystem_error);
            if (filesystem_error) {
                return Result<Json>{
                    std::unexpected(operation_error("output_read_failed", "could not inspect build destination"))};
            }
            std::optional<std::string> output_digest;
            if (output_existed) {
                output_digest = known_fingerprint(*fingerprints, *output_path);
                if (!output_digest) {
                    auto digest = file_sha256(*output_path, context.cancellation);
                    if (!digest)
                        return Result<Json>{std::unexpected(digest.error())};
                    output_digest = std::move(*digest);
                }
            }
            const auto build = axk::current_build_info();
            const auto now = Clock::now();
            auto token = axk::app::secure_random_hex(24U);
            if (!token)
                return Result<Json>{std::unexpected(token.error())};
            auto record = std::make_shared<WritePlanRecord>(WritePlanRecord{std::move(*token),
                                                                            context.owner_id,
                                                                            now + state->retention,
                                                                            write_plan_kind(*manifest_kind),
                                                                            *output,
                                                                            *output_path,
                                                                            overwrite,
                                                                            output_existed,
                                                                            std::move(output_digest),
                                                                            std::move(*fingerprints),
                                                                            std::move(document->file_inputs),
                                                                            std::move(document->file_input_sha256),
                                                                            std::move(document->logical_input_paths),
                                                                            std::move(document->leases),
                                                                            std::move(document->staging),
                                                                            std::move(manifest),
                                                                            std::move(summary),
                                                                            std::string{axk::version()},
                                                                            build.source_identity,
                                                                            false});
            if (auto registered = register_plan(state, record); !registered)
                return Result<Json>{std::unexpected(registered.error())};
            return Result<Json>{write_plan_json(*record, static_cast<std::uint64_t>(state->retention.count() * 60))};
        };

    if (!registry.is_implemented("create.plan")) {
        auto bound = registry.bind("create.plan", create_plan_handler);
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("create.hds.profiles")) {
        auto bound = registry.bind("create.hds.profiles", [](const Json &, const OperationContext &) {
            auto profiles = Json::array();
            for (const auto &profile : axk::hds_creation_profiles()) {
                auto options = Json::array();
                for (const auto &option : profile.partition_options) {
                    options.push_back({{"partitionCount", option.partition_count},
                                       {"partitionSizeBytes", option.partitions.front().filesystem_sector_count * 512U},
                                       {"unusedTailBytes", option.unused_tail_sectors * 512U}});
                }
                profiles.push_back({{"profileId", hds_creation_profile_wire_id(profile.id)},
                                    {"sizeBytes", profile.size_bytes},
                                    {"defaultPartitionCount", profile.default_partition_count},
                                    {"partitionOptions", std::move(options)}});
            }
            return Result<Json>{Json{{"schemaVersion", "1.0"}, {"profiles", std::move(profiles)}}};
        });
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("create.hds.plan")) {
        auto bound = registry.bind("create.hds.plan", [create_plan_handler](const Json &input,
                                                                            const OperationContext &context) {
            std::string profile_text;
            std::uint8_t partition_count{};
            try {
                profile_text = input.at("profileId").get<std::string>();
                partition_count = input.at("partitionCount").get<std::uint8_t>();
            } catch (const Json::exception &) {
                return Result<Json>{
                    std::unexpected(operation_error("invalid_request", "profileId and partitionCount are required"))};
            }
            const auto profile = parse_hds_creation_profile_wire_id(profile_text);
            if (!profile) {
                return Result<Json>{
                    std::unexpected(operation_error("invalid_request", "profileId is not a supported HDS profile"))};
            }
            auto planned = axk::plan_hds_creation({*profile, partition_count}, context.cancellation);
            if (!planned)
                return Result<Json>{std::unexpected(core_error(planned.error()))};
            if (!input.contains("output")) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "output is required"))};
            }

            auto partitions = Json::array();
            for (const auto &partition : planned->manifest.partitions) {
                partitions.push_back({{"name", partition.name}, {"volumes", Json::array()}});
            }
            Json generic_request{{"kind", "HDS"},
                                 {"manifest",
                                  {{"inline",
                                    {{"schema_version", axk::build_manifest_schema_version},
                                     {"size_bytes", planned->manifest.size_bytes},
                                     {"partitions", std::move(partitions)}}}}},
                                 {"output", input.at("output")},
                                 {"overwrite", input.value("overwrite", false)}};
            return create_plan_handler(generic_request, context);
        });
        if (!bound)
            return bound;
    }

    const auto bind_create = [&](std::string_view operation_id, axk::BuildManifestKind expected_kind) -> Result<void> {
        if (registry.is_implemented(operation_id))
            return {};
        auto bound = registry.bind(operation_id, [state, expected_kind, &sandbox,
                                                  media_limits](const Json &input, const OperationContext &context) {
            std::string token;
            try {
                token = input.at("planToken").get<std::string>();
            } catch (const Json::exception &) {
                return Result<Json>{std::unexpected(operation_error("invalid_request", "planToken is required"))};
            }
            auto claim = claim_plan(state, token, context.owner_id, write_plan_kind(expected_kind));
            if (!claim)
                return Result<Json>{std::unexpected(claim.error())};
            const auto &record = *claim->record();
            if (auto verified = verify_plan_state(record, sandbox, context.cancellation); !verified) {
                claim->consume();
                return Result<Json>{std::unexpected(verified.error())};
            }
            auto output_path = sandbox.resolve_output_file(record.output, record.overwrite);
            if (!output_path)
                return Result<Json>{std::unexpected(output_path.error())};
            if (normalized_path(*output_path) != normalized_path(record.output_path)) {
                return Result<Json>{std::unexpected(
                    operation_error("write_plan_stale", "destination identity changed after planning"))};
            }
            auto staging_directory = sandbox.create_staging_directory("axklib-image-build");
            if (!staging_directory)
                return Result<Json>{std::unexpected(staging_directory.error())};
            TemporaryDirectoryCleanup staging_cleanup{*staging_directory};
            const auto staged_output = *staging_directory / record.output_path.filename();
            const auto publish = [&]() -> Result<void> {
                auto reader = axk::FileReader::open(staged_output);
                if (!reader)
                    return std::unexpected(core_error(reader.error(), record.output.relative_path));
                return sandbox.publish_file(record.output, record.overwrite, **reader);
            };
            if (expected_kind == axk::BuildManifestKind::hds) {
                const auto &manifest = std::get<axk::HdsBuildManifest>(record.manifest);
                auto written = axk::write_hds_image(manifest, staged_output, false, context.cancellation);
                if (!written)
                    return Result<Json>{std::unexpected(core_error(written.error(), record.output.relative_path))};
                auto partitions = Json::array();
                for (const auto &partition : written->partitions) {
                    partitions.push_back({{"index", partition.geometry.index},
                                          {"name", partition.name},
                                          {"startSector", partition.geometry.start_sector},
                                          {"sectorCount", partition.geometry.filesystem_sector_count},
                                          {"clusterCount", partition.geometry.cluster_count},
                                          {"freeKiB", partition.sampler_visible_free_kib}});
                }
                auto result = validate_written_image(staged_output, record.output, context);
                if (!result)
                    return Result<Json>{std::unexpected(result.error())};
                if (auto published = publish(); !published)
                    return Result<Json>{std::unexpected(published.error())};
                decorate_build_result(*result, record);
                (*result)["partitions"] = std::move(partitions);
                (*result)["unusedTailSectors"] = written->unused_tail_sectors;
                claim->consume();
                return result;
            } else {
                const auto &manifest = std::get<axk::MediaBuildManifest>(record.manifest);
                auto written =
                    axk::write_media_image(manifest, staged_output, false, media_limits, context.cancellation);
                if (!written)
                    return Result<Json>{std::unexpected(core_error(written.error(), record.output.relative_path))};
            }
            auto result = validate_written_image(staged_output, record.output, context);
            if (!result)
                return Result<Json>{std::unexpected(result.error())};
            if (auto published = publish(); !published)
                return Result<Json>{std::unexpected(published.error())};
            decorate_build_result(*result, record);
            claim->consume();
            return result;
        });
        if (!bound)
            return bound;
        return registry.bind_path_accesses(
            operation_id,
            [state, expected_kind](const Json &input,
                                   const OperationContext &context) -> Result<std::vector<PathAccess>> {
                std::string token;
                try {
                    token = input.at("planToken").get<std::string>();
                } catch (const Json::exception &) {
                    return std::unexpected(operation_error("invalid_request", "planToken is required"));
                }
                std::lock_guard lock{state->mutex};
                cleanup_plans(*state, Clock::now());
                const auto found = state->plans.find(token);
                if (found == state->plans.end() || found->second->owner_id != context.owner_id ||
                    found->second->kind != write_plan_kind(expected_kind) || found->second->claimed) {
                    return std::unexpected(operation_error("write_plan_not_found", "write plan is expired or unknown"));
                }
                std::vector<PathAccess> accesses;
                accesses.reserve(found->second->input_refs.size() + 1U);
                for (const auto &source : found->second->input_refs)
                    accesses.push_back({source, PathAccessMode::shared});
                accesses.push_back({found->second->output, PathAccessMode::exclusive});
                return accesses;
            });
    };
    if (auto bound = bind_create("create.hds", axk::BuildManifestKind::hds); !bound)
        return bound;
    if (auto bound = bind_create("create.floppy", axk::BuildManifestKind::fat12_floppy); !bound)
        return bound;
    if (auto bound = bind_create("create.iso", axk::BuildManifestKind::iso9660); !bound)
        return bound;

    if (!registry.is_implemented("alter.inspect")) {
        auto bound =
            registry.bind("alter.inspect", [&sandbox, &uploads](const Json &input, const OperationContext &context) {
                auto source = parse_file_ref(input, "source");
                if (!source)
                    return Result<Json>{std::unexpected(source.error())};
                auto source_file = sandbox.open_file(*source);
                if (!source_file)
                    return Result<Json>{std::unexpected(source_file.error())};
                auto staging_directory = sandbox.create_staging_directory("axklib-alteration-inspection");
                if (!staging_directory)
                    return Result<Json>{std::unexpected(staging_directory.error())};
                TemporaryDirectoryCleanup staging_cleanup{*staging_directory};
                const auto source_path = *staging_directory / "source.hds";
                if (auto staged = write_reader(source_path, *source_file->reader); !staged)
                    return Result<Json>{std::unexpected(staged.error())};
                auto document = load_manifest(input, context, sandbox, uploads);
                if (!document)
                    return Result<Json>{std::unexpected(document.error())};
                auto manifest = axk::parse_alteration_manifest(document->json.dump());
                if (!manifest)
                    return Result<Json>{std::unexpected(core_error(manifest.error()))};
                const auto required_paths = external_paths(*manifest);
                if (auto admitted = require_bound_inputs(required_paths, document->bound_input_paths); !admitted)
                    return Result<Json>{std::unexpected(admitted.error())};
                auto inspection =
                    axk::inspect_hds_alteration(source_path, *manifest, context.cancellation, context.progress);
                if (!inspection)
                    return Result<Json>{std::unexpected(core_error(inspection.error(), source->relative_path))};

                auto operations = Json::array();
                for (const auto &operation : inspection->operations)
                    operations.push_back(operation_report_json(operation, document->logical_input_paths));
                const auto build = axk::current_build_info();
                Json result = {{"schemaVersion", "1.0"},
                               {"kind", "ALTERATION"},
                               {"semanticVersion", axk::version()},
                               {"sourceIdentity", build.source_identity},
                               {"summary", alteration_summary(inspection->operations)},
                               {"valid", true},
                               {"operations", std::move(operations)},
                               {"warnings", Json::array()},
                               {"validation", {{"valid", true}, {"issueCount", 0U}}}};
                return Result<Json>{std::move(result)};
            });
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("alter.hds")) {
        auto bound = registry.bind("alter.hds", [&sandbox, &uploads](const Json &input,
                                                                     const OperationContext &context) {
            auto source = parse_file_ref(input, "source");
            if (!source)
                return Result<Json>{std::unexpected(source.error())};
            auto source_file = sandbox.open_file(*source);
            if (!source_file)
                return Result<Json>{std::unexpected(source_file.error())};
            auto source_digest = reader_sha256(*source_file->reader, context.cancellation);
            if (!source_digest)
                return Result<Json>{std::unexpected(source_digest.error())};
            auto source_identity_path = sandbox.resolve_file(*source);
            if (!source_identity_path)
                return Result<Json>{std::unexpected(source_identity_path.error())};
            const auto replace_source = input.value("replaceSource", false);
            auto output = parse_file_ref(input, "output");
            if (!output)
                return Result<Json>{std::unexpected(output.error())};
            if (replace_source && input.value("overwrite", false)) {
                return Result<Json>{std::unexpected(
                    operation_error("invalid_request", "overwrite must be omitted when replaceSource is true"))};
            }
            if (replace_source) {
                auto output_identity = sandbox.resolve_file(*output);
                if (!output_identity)
                    return Result<Json>{std::unexpected(output_identity.error())};
                if (normalized_path(*source_identity_path) != normalized_path(*output_identity)) {
                    return Result<Json>{std::unexpected(
                        operation_error("invalid_request", "output must match source when replaceSource is true"))};
                }
            } else if (const auto distinct = sandbox.require_distinct(*source, *output); !distinct) {
                return Result<Json>{std::unexpected(distinct.error())};
            }
            const auto overwrite = replace_source ? true : input.value("overwrite", false);
            auto output_path = sandbox.resolve_output_file(*output, overwrite);
            if (!output_path)
                return Result<Json>{std::unexpected(output_path.error())};
            auto staging_directory = sandbox.create_staging_directory("axklib-alteration");
            if (!staging_directory)
                return Result<Json>{std::unexpected(staging_directory.error())};
            TemporaryDirectoryCleanup staging_cleanup{*staging_directory};
            const auto source_path = *staging_directory / "source.hds";
            if (auto staged = write_reader(source_path, *source_file->reader); !staged)
                return Result<Json>{std::unexpected(staged.error())};
            auto document = load_manifest(input, context, sandbox, uploads);
            if (!document)
                return Result<Json>{std::unexpected(document.error())};
            auto manifest = axk::parse_alteration_manifest(document->json.dump());
            if (!manifest)
                return Result<Json>{std::unexpected(core_error(manifest.error()))};
            const auto required_paths = external_paths(*manifest);
            if (auto admitted = require_bound_inputs(required_paths, document->bound_input_paths); !admitted)
                return Result<Json>{std::unexpected(admitted.error())};
            auto fingerprint_paths = document->observed_paths;
            fingerprint_paths.push_back(source_path);
            fingerprint_paths.insert(fingerprint_paths.end(), required_paths.begin(), required_paths.end());
            auto fingerprints = fingerprint_files(fingerprint_paths, context.cancellation);
            if (!fingerprints)
                return Result<Json>{std::unexpected(fingerprints.error())};
            std::error_code filesystem_error;
            const auto output_existed = std::filesystem::exists(*output_path, filesystem_error);
            if (filesystem_error) {
                return Result<Json>{
                    std::unexpected(operation_error("output_read_failed", "could not inspect alteration destination"))};
            }
            std::optional<std::string> output_digest;
            if (output_existed) {
                output_digest = known_fingerprint(*fingerprints, *output_path);
                if (!output_digest) {
                    auto digest = file_sha256(*output_path, context.cancellation);
                    if (!digest)
                        return Result<Json>{std::unexpected(digest.error())};
                    output_digest = std::move(*digest);
                }
            }
            const auto staging = *staging_directory / "output.hds";
            auto altered =
                axk::alter_hds(source_path, *manifest, staging, context.cancellation, context.progress, false);
            if (!altered)
                return Result<Json>{std::unexpected(core_error(altered.error(), output->relative_path))};
            if (context.progress) {
                context.progress->report(
                    {axk::ProgressPhase::validating, 0U, 1U, "validating alteration image", std::nullopt});
            }
            if (const auto checked = context.cancellation.check(); !checked)
                return Result<Json>{std::unexpected(core_error(checked.error(), output->relative_path))};
            auto validation = validate_written_image(staging, *output, context);
            if (!validation)
                return Result<Json>{std::unexpected(validation.error())};
            if (context.progress) {
                context.progress->report(
                    {axk::ProgressPhase::validating, 1U, 1U, "validated alteration image", std::nullopt});
            }
            if (auto verified = verify_alteration_state(*fingerprints, *output_path, output_existed, output_digest,
                                                        context.cancellation);
                !verified) {
                return Result<Json>{std::unexpected(verified.error())};
            }
            const std::array source_reference{*source};
            const std::array source_sha256{*source_digest};
            if (auto verified = verify_sandbox_files(source_reference, source_sha256, sandbox, context.cancellation);
                !verified) {
                return Result<Json>{std::unexpected(verified.error())};
            }
            if (auto verified = verify_sandbox_files(document->file_inputs, document->file_input_sha256, sandbox,
                                                     context.cancellation);
                !verified) {
                return Result<Json>{std::unexpected(verified.error())};
            }
            if (context.progress) {
                context.progress->report(
                    {axk::ProgressPhase::publishing, 0U, 1U, "publishing alteration image", std::nullopt});
            }
            if (const auto checked = context.cancellation.check(); !checked)
                return Result<Json>{std::unexpected(core_error(checked.error(), output->relative_path))};
            auto staged_reader = axk::FileReader::open(staging);
            if (!staged_reader)
                return Result<Json>{std::unexpected(core_error(staged_reader.error(), output->relative_path))};
            if (auto published = sandbox.publish_file(*output, overwrite, **staged_reader); !published)
                return Result<Json>{std::unexpected(published.error())};
            if (context.progress) {
                context.progress->report(
                    {axk::ProgressPhase::publishing, 1U, 1U, "published alteration image", std::nullopt});
            }
            auto operations = Json::array();
            for (const auto &operation : altered->operations)
                operations.push_back(operation_report_json(operation, document->logical_input_paths));
            (*validation)["operations"] = std::move(operations);
            (*validation)["applied"] = altered->applied;
            (*validation)["schemaVersion"] = "1.0";
            (*validation)["kind"] = "ALTERATION";
            (*validation)["summary"] = alteration_summary(altered->operations);
            (*validation)["warnings"] = Json::array();
            return validation;
        });
        if (!bound)
            return bound;
    }
    return {};
}
