#include "axklib/application/package_operations.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "axklib/alteration_transaction.hpp"
#include "axklib/application/alteration_journal.hpp"
#include "axklib/application/download_archives.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/application/secure_random.hpp"
#include "axklib/package.hpp"
#include "axklib/package_import_planning.hpp"
#include "content_digest.hpp"
#include "package_operations_internal.hpp"

using namespace axk::app::package_operations_internal;

axk::app::Result<void> axk::app::bind_session_package_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                                 UploadStore &uploads, ImageSessionManager &images,
                                                                 AlterationJournalStore &journals,
                                                                 DownloadArchiveStore &downloads) {
    auto state = std::make_shared<SessionPackageOperationState>();

    if (!registry.is_implemented("images.package_import.plan")) {
        auto bound = registry.bind(
            "images.package_import.plan",
            [state, &sandbox, &uploads, &images](const Json &input, const OperationContext &context) -> Result<Json> {
                const auto operation_started = Clock::now();
                const auto diagnostic = [&](std::string_view phase, Clock::time_point started,
                                            const Json &details = Json::object()) {
                    if (!context.diagnostic)
                        return;
                    auto event = details;
                    event["event"] = "package_import_plan_phase";
                    event["requestId"] = context.request_id;
                    event["phase"] = phase;
                    event["durationMs"] =
                        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
                    context.diagnostic(event);
                };

                const auto admission_started = Clock::now();
                const auto identity = parse_session_identity(input);
                if (!identity)
                    return std::unexpected(identity.error());
                auto session = images.begin_read(identity->first, context.owner_id, identity->second);
                if (!session)
                    return std::unexpected(session.error());
                if (session->media->kind() != axk::MediaKind::sfs) {
                    return std::unexpected(operation_error("image_mutation_unsupported",
                                                           "package import requires a writable SFS image session"));
                }

                PackageInput source;
                std::optional<std::string> replace_plan_token;
                try {
                    auto parsed = parse_package_input(input.at("package"));
                    if (!parsed)
                        return std::unexpected(parsed.error());
                    source = std::move(*parsed);
                    if (input.contains("replacePlanToken")) {
                        replace_plan_token = input.at("replacePlanToken").get<std::string>();
                        if (replace_plan_token->empty())
                            return std::unexpected(
                                operation_error("invalid_request", "replacePlanToken must not be empty"));
                    }
                } catch (const Json::exception &) {
                    return std::unexpected(operation_error("invalid_request", "package is required"));
                }
                diagnostic("admission", admission_started,
                           {{"imageId", identity->first},
                            {"revision", identity->second},
                            {"imageBytes", session->reader->size()},
                            {"targetObjectCount", session->catalog_objects.size()},
                            {"replacement", replace_plan_token.has_value()}});

                const auto package_started = Clock::now();
                std::shared_ptr<const VerifiedPackageSnapshot> package_snapshot;
                if (replace_plan_token) {
                    std::lock_guard lock{state->mutex};
                    cleanup_session_plans(*state, Clock::now());
                    const auto found = state->plans.find(*replace_plan_token);
                    if (found == state->plans.end() || found->second->owner_id != context.owner_id) {
                        return std::unexpected(operation_error("package_plan_not_found",
                                                               "replacement package import plan is absent or expired"));
                    }
                    if (found->second->claimed) {
                        return std::unexpected(
                            operation_error("package_plan_in_use", "package import plan is already being applied"));
                    }
                    if (found->second->image_id != identity->first ||
                        found->second->expected_revision != identity->second ||
                        found->second->package_snapshot->input != source) {
                        return std::unexpected(operation_error(
                            "package_plan_stale", "replacement plan does not match this image, revision, and package"));
                    }
                    package_snapshot = found->second->package_snapshot;
                } else {
                    auto resolved = resolve_package(source, context.owner_id, sandbox, uploads);
                    if (!resolved)
                        return std::unexpected(resolved.error());
                    auto package = read_package(*resolved, true, context);
                    if (!package)
                        return std::unexpected(package.error());
                    auto retained_bytes = retained_package_bytes(*package);
                    if (!retained_bytes)
                        return std::unexpected(retained_bytes.error());
                    package_snapshot = std::make_shared<VerifiedPackageSnapshot>(
                        VerifiedPackageSnapshot{source, std::move(*package), *retained_bytes});
                }
                diagnostic("package", package_started,
                           {{"imageId", identity->first},
                            {"cacheHit", replace_plan_token.has_value()},
                            {"packageObjectCount", package_snapshot->package.nodes.size()},
                            {"packagePayloadBytes", package_snapshot->retained_payload_bytes}});

                auto request = parse_session_import_request(input, package_snapshot->package);
                if (!request)
                    return std::unexpected(request.error());

                const auto planning_started = Clock::now();
                const auto packages = std::span<const axk::PortablePackage>{&package_snapshot->package, 1U};
                axk::package_import_internal::RetainedPackageImportStats planning_stats;
                const axk::package_import_internal::RetainedPackageImportTarget target{
                    session->reader,          std::filesystem::path{session->source.relative_path},
                    session->media,           session->target_snapshot_id,
                    session->catalog_objects, session->catalog_issues,
                    &planning_stats,          true};
                auto plan = axk::package_import_internal::plan_package_import_retained(target, packages, *request,
                                                                                       context.cancellation);
                if (!plan)
                    return std::unexpected(core_error(plan.error(), session->source.relative_path));
                diagnostic("planning", planning_started,
                           {{"imageId", identity->first},
                            {"actionCount", plan->objects.size()},
                            {"conflictCount", plan->conflicts.size()},
                            {"targetPayloadBytesRead", planning_stats.target_payload_bytes_read},
                            {"targetPayloadObjectsRead", planning_stats.target_payload_objects_read}});

                const auto storage_started = Clock::now();
                const auto now = Clock::now();
                auto token = secure_random_hex(24U);
                if (!token)
                    return std::unexpected(token.error());
                auto record = std::make_shared<SessionPackagePlanRecord>(
                    SessionPackagePlanRecord{*token, context.owner_id, now + state->retention, identity->first,
                                             identity->second, package_snapshot, std::move(*plan), false});
                {
                    std::lock_guard lock{state->mutex};
                    cleanup_session_plans(*state, now);
                    if (replace_plan_token) {
                        const auto found = state->plans.find(*replace_plan_token);
                        if (found == state->plans.end() || found->second->owner_id != context.owner_id ||
                            found->second->claimed || found->second->package_snapshot != package_snapshot) {
                            return std::unexpected(operation_error(
                                "package_plan_stale", "replacement package import plan changed while replanning"));
                        }
                    } else if (state->plans.size() >= state->maximum_plans) {
                        return std::unexpected(operation_error(
                            "package_plan_capacity", "too many package import plans are active", std::nullopt, true));
                    }
                    if (!replace_plan_token) {
                        const auto retained = retained_session_package_bytes(*state);
                        if (retained > state->maximum_retained_package_bytes ||
                            package_snapshot->retained_payload_bytes >
                                state->maximum_retained_package_bytes - retained) {
                            return std::unexpected(operation_error(
                                "package_plan_capacity", "retained package import payload budget is exhausted",
                                std::nullopt, true));
                        }
                    }
                    if (state->plans.contains(*token))
                        return std::unexpected(operation_error("secure_random_failed", "package plan token collision"));
                    state->plans.emplace(*token, record);
                    if (replace_plan_token)
                        state->plans.erase(*replace_plan_token);
                }
                diagnostic("storage", storage_started,
                           {{"imageId", identity->first},
                            {"replacement", replace_plan_token.has_value()},
                            {"retainedPackageBytes", package_snapshot->retained_payload_bytes}});
                auto result =
                    plan_json(record->plan, record->token, static_cast<std::uint64_t>(state->retention.count() * 60));
                result["imageId"] = record->image_id;
                result["revision"] = record->expected_revision;
                diagnostic("total", operation_started,
                           {{"imageId", identity->first},
                            {"revision", identity->second},
                            {"cacheHit", replace_plan_token.has_value()},
                            {"imageBytes", session->reader->size()},
                            {"packageObjectCount", package_snapshot->package.nodes.size()},
                            {"packagePayloadBytes", package_snapshot->retained_payload_bytes},
                            {"targetPayloadBytesRead", planning_stats.target_payload_bytes_read},
                            {"targetPayloadObjectsRead", planning_stats.target_payload_objects_read}});
                return result;
            });
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("images.package_import.release")) {
        auto bound =
            registry.bind("images.package_import.release",
                          [state](const Json &input, const OperationContext &context) -> Result<Json> {
                              std::string token;
                              try {
                                  token = input.at("planToken").get<std::string>();
                              } catch (const Json::exception &) {
                                  return std::unexpected(operation_error("invalid_request", "planToken is required"));
                              }
                              std::lock_guard lock{state->mutex};
                              cleanup_session_plans(*state, Clock::now());
                              const auto found = state->plans.find(token);
                              if (found == state->plans.end() || found->second->owner_id != context.owner_id)
                                  return std::unexpected(operation_error("package_plan_not_found",
                                                                         "package import plan is absent or expired"));
                              if (found->second->claimed)
                                  return std::unexpected(operation_error(
                                      "package_plan_in_use", "package import plan is already being applied"));
                              state->plans.erase(found);
                              return Json{{"released", true}};
                          });
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("images.package_import")) {
        auto bound = registry.bind(
            "images.package_import",
            [state, &images, &journals](const Json &input, const OperationContext &context) -> Result<Json> {
                std::string token;
                try {
                    token = input.at("planToken").get<std::string>();
                } catch (const Json::exception &) {
                    return std::unexpected(operation_error("invalid_request", "planToken is required"));
                }
                auto claim = claim_session_plan(state, token, context.owner_id);
                if (!claim)
                    return std::unexpected(claim.error());
                const auto record = claim->record();
                if (!record->plan.valid()) {
                    claim->consume();
                    return std::unexpected(
                        operation_error("package_plan_conflicts", "package import plan contains unresolved conflicts"));
                }

                auto mutation = images.begin_mutation(record->image_id, context.owner_id, record->expected_revision);
                if (!mutation)
                    return std::unexpected(mutation.error());
                SessionMutationGuard mutation_guard{images, record->image_id, context.owner_id,
                                                    record->expected_revision};

                auto current_snapshot = detail::reader_sha256(*mutation->target, context.cancellation);
                if (!current_snapshot)
                    return std::unexpected(current_snapshot.error());
                if (*current_snapshot != record->plan.target_snapshot_id) {
                    claim->consume();
                    return std::unexpected(
                        operation_error("package_plan_stale", "image changed after package import planning"));
                }
                const auto packages = std::span<const axk::PortablePackage>{&record->package_snapshot->package, 1U};
                auto prepared = axk::detail::prepare_sfs_package_import_verified(
                    mutation->target, std::filesystem::path{mutation->source.relative_path}, packages, record->plan,
                    *current_snapshot, context.cancellation, context.progress);
                if (!prepared)
                    return std::unexpected(core_error(prepared.error(), mutation->source.relative_path));

                std::vector<AlterationJournalPatch> patches;
                patches.reserve(prepared->patches.size());
                for (auto &patch : prepared->patches)
                    patches.push_back({patch.offset, std::move(patch.original), std::move(patch.replacement)});
                std::optional<PreparedImageSessionCommit> prepared_commit;
                const auto validate_commit = [&]() -> Result<void> {
                    auto validation = images.prepare_mutation_commit(record->image_id, context.owner_id,
                                                                     record->expected_revision, CancellationToken{});
                    if (!validation)
                        return std::unexpected(validation.error());
                    prepared_commit.emplace(std::move(*validation));
                    return {};
                };
                if (auto applied = journals.apply(mutation->target, prepared->image_size_bytes, patches,
                                                  context.cancellation, validate_commit);
                    !applied) {
                    return std::unexpected(applied.error());
                }
                if (!prepared_commit)
                    return std::unexpected(
                        operation_error("image_operation_failed", "image mutation validation did not complete"));
                mutation->target.reset();
                auto summary = images.finalize_mutation_commit(std::move(*prepared_commit));
                mutation_guard.finish();
                auto result = session_import_result(*record, summary, !patches.empty());
                claim->consume();
                return result;
            });
        if (!bound)
            return bound;
    }

    if (!registry.is_implemented("images.package_export")) {
        auto bound = registry.bind(
            "images.package_export",
            [&sandbox, &images, &downloads](const Json &input, const OperationContext &context) -> Result<Json> {
                const auto identity = parse_session_identity(input);
                if (!identity)
                    return std::unexpected(identity.error());
                auto session = images.begin_read(identity->first, context.owner_id, identity->second);
                if (!session)
                    return std::unexpected(session.error());

                auto roots = parse_session_export_roots(input, session->object_keys_by_id);
                if (!roots)
                    return std::unexpected(roots.error());
                auto built = axk::build_portable_package(*session->media, *roots, context.cancellation);
                if (!built && built.error().code == axk::ErrorCode::object_missing &&
                    session->source.kind == ImageSourceKind::axk_object_directory) {
                    return std::unexpected(operation_error("companion_disks_required",
                                                           "Wave Data continues on another sampler disk. Add extracted "
                                                           "companion disk folders to export it.",
                                                           session->source.relative_path));
                }
                if (!built)
                    return std::unexpected(core_error(built.error(), session->source.relative_path));
                auto build = std::move(*built);
                session->lease.reset();

                Json destination;
                try {
                    destination = input.at("destination");
                } catch (const Json::exception &) {
                    return std::unexpected(operation_error("invalid_request", "destination is required"));
                }
                auto result = package_json(build.package);
                result["imageId"] = identity->first;
                result["revision"] = identity->second;
                result["sizeBytes"] = build.archive.size();
                const auto kind = destination.value("kind", std::string{});
                if (kind == "WORKSPACE") {
                    auto output = parse_file_ref(destination, "output");
                    if (!output)
                        return std::unexpected(output.error());
                    auto resolved = package_internal::resolve_filename(output->relative_path, build.required_extension);
                    if (!resolved)
                        return std::unexpected(resolved.error());
                    output->relative_path = std::move(*resolved);
                    const axk::MemoryReader archive{build.archive};
                    if (auto published = sandbox.publish_file(*output, destination.value("overwrite", false), archive);
                        !published) {
                        return std::unexpected(published.error());
                    }
                    result["destination"] = "WORKSPACE";
                    result["output"] = file_ref_json(*output);
                    result["download"] = nullptr;
                } else if (kind == "DOWNLOAD") {
                    auto filename = destination.value("filename", build.package.roots.front().display_name +
                                                                      std::string{build.required_extension});
                    if (filename.empty() || filename == "." || filename == ".." ||
                        filename.find_first_of("/\\") != std::string::npos) {
                        return std::unexpected(operation_error("invalid_request", "download filename is invalid"));
                    }
                    auto resolved = package_internal::resolve_filename(filename, build.required_extension);
                    if (!resolved)
                        return std::unexpected(resolved.error());
                    filename = std::move(*resolved);
                    auto retained =
                        downloads.retain(context.owner_id, filename, "application/octet-stream", build.archive);
                    if (!retained)
                        return std::unexpected(retained.error());
                    result["destination"] = "DOWNLOAD";
                    result["output"] = nullptr;
                    result["download"] = {
                        {"archiveId", retained->reference.archive_id},
                        {"filename", retained->filename},
                        {"sizeBytes", retained->size_bytes},
                        {"expiresInSeconds", retained->expires_in_seconds},
                        {"contentPath", "/api/v1/download-archives/" + retained->reference.archive_id + "/content"}};
                } else {
                    return std::unexpected(
                        operation_error("invalid_request", "destination kind must be WORKSPACE or DOWNLOAD"));
                }
                return result;
            });
        if (!bound)
            return bound;
    }
    return {};
}
