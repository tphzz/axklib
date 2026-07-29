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

axk::app::Result<void> axk::app::bind_session_write_operations(OperationRegistry &registry, const Sandbox &sandbox,
                                                               UploadStore &uploads, ImageSessionManager &images,
                                                               AlterationJournalStore &journals) {
    const auto alter_session = [&sandbox, &uploads, &images,
                                &journals](const Json &input, const OperationContext &context) -> Result<Json> {
        const auto operation_started = Clock::now();
        const auto diagnostic = [&](std::string_view phase, Clock::time_point started,
                                    const Json &details = Json::object()) {
            if (!context.diagnostic)
                return;
            auto event = details;
            event["event"] = "image_mutation_phase";
            event["requestId"] = context.request_id;
            event["phase"] = phase;
            event["durationMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
            event["strategy"] = "journaled-in-place";
            context.diagnostic(event);
        };

        std::string image_id;
        std::uint64_t expected_revision{};
        try {
            image_id = input.at("imageId").get<std::string>();
            expected_revision = input.at("expectedRevision").get<std::uint64_t>();
        } catch (const Json::exception &) {
            return std::unexpected(operation_error("invalid_request", "imageId and expectedRevision are required"));
        }

        const auto admission_started = Clock::now();
        auto mutation = images.begin_mutation(image_id, context.owner_id, expected_revision);
        if (!mutation)
            return std::unexpected(mutation.error());
        bool mutation_finished{};
        struct AbortGuard {
            ImageSessionManager &images;
            std::string_view image_id;
            std::string_view owner_id;
            std::uint64_t revision;
            bool &finished;
            ~AbortGuard() {
                if (!finished)
                    images.abort_mutation(image_id, owner_id, revision);
            }
        } guard{images, image_id, context.owner_id, expected_revision, mutation_finished};
        diagnostic("admission", admission_started, {{"imageId", image_id}, {"revision", expected_revision}});

        const auto manifest_started = Clock::now();
        auto document = load_manifest(input, context, sandbox, uploads);
        if (!document)
            return std::unexpected(document.error());
        auto manifest = axk::parse_alteration_manifest(document->json.dump());
        if (!manifest)
            return std::unexpected(core_error(manifest.error(), mutation->source.relative_path));
        const auto required_paths = external_paths(*manifest);
        if (auto admitted = require_bound_inputs(required_paths, document->bound_input_paths); !admitted)
            return std::unexpected(admitted.error());
        auto fingerprints = fingerprint_files(document->observed_paths, context.cancellation);
        if (!fingerprints)
            return std::unexpected(fingerprints.error());
        diagnostic("manifest", manifest_started,
                   {{"imageId", image_id},
                    {"operationCount", manifest->operations.size()},
                    {"inputCount", fingerprints->size()}});

        const auto planning_started = Clock::now();
        auto prepared =
            axk::detail::prepare_hds_alteration(mutation->target, std::filesystem::path{mutation->source.relative_path},
                                                *manifest, context.cancellation, context.progress);
        if (!prepared)
            return std::unexpected(core_error(prepared.error(), mutation->source.relative_path));
        std::uint64_t patch_bytes{};
        std::vector<AlterationJournalPatch> patches;
        patches.reserve(prepared->patches.size());
        for (auto &patch : prepared->patches) {
            if (patch.replacement.size() > std::numeric_limits<std::uint64_t>::max() - patch_bytes)
                return std::unexpected(operation_error("image_operation_failed", "alteration patch size overflow"));
            patch_bytes += patch.replacement.size();
            patches.push_back({patch.offset, std::move(patch.original), std::move(patch.replacement)});
        }
        diagnostic("planning", planning_started,
                   {{"imageId", image_id},
                    {"imageBytes", prepared->image_size_bytes},
                    {"patchCount", patches.size()},
                    {"patchBytes", patch_bytes}});

        const auto verification_started = Clock::now();
        if (auto verified = verify_fingerprints(*fingerprints, context.cancellation); !verified)
            return std::unexpected(verified.error());
        if (auto verified =
                verify_sandbox_files(document->file_inputs, document->file_input_sha256, sandbox, context.cancellation);
            !verified) {
            return std::unexpected(verified.error());
        }
        diagnostic("input-verification", verification_started, {{"imageId", image_id}});

        if (context.progress) {
            context.progress->report(
                {axk::ProgressPhase::publishing, 0U, 1U, "Committing image changes", std::nullopt});
        }
        const auto journal_started = Clock::now();
        std::optional<PreparedImageSessionCommit> prepared_commit;
        const auto validate_commit = [&]() -> Result<void> {
            auto validation =
                images.prepare_mutation_commit(image_id, context.owner_id, expected_revision, CancellationToken{});
            if (!validation)
                return std::unexpected(validation.error());
            prepared_commit.emplace(std::move(*validation));
            return {};
        };
        if (auto applied = journals.apply(mutation->target, prepared->image_size_bytes, patches, context.cancellation,
                                          validate_commit);
            !applied) {
            return std::unexpected(applied.error());
        }
        if (!prepared_commit)
            return std::unexpected(
                operation_error("image_operation_failed", "image mutation validation did not complete"));
        diagnostic("journal-commit", journal_started,
                   {{"imageId", image_id},
                    {"patchCount", patches.size()},
                    {"patchBytes", patch_bytes},
                    {"cleanupPending", !journals.storage_ready()}});

        const auto refresh_started = Clock::now();
        mutation->target.reset();
        auto summary = images.finalize_mutation_commit(std::move(*prepared_commit));
        mutation_finished = true;
        diagnostic("session-refresh", refresh_started,
                   {{"imageId", image_id}, {"revision", summary.revision}, {"objectCount", summary.object_count}});
        if (context.progress) {
            context.progress->report({axk::ProgressPhase::publishing, 1U, 1U, "Image changes committed", std::nullopt});
        }

        auto operations = Json::array();
        for (const auto &operation : prepared->operations)
            operations.push_back(operation_report_json(operation, document->logical_input_paths));
        const auto issue_count =
            summary.validation.info_count + summary.validation.warning_count + summary.validation.error_count;
        diagnostic("total", operation_started,
                   {{"imageId", image_id},
                    {"revision", summary.revision},
                    {"imageBytes", prepared->image_size_bytes},
                    {"patchCount", patches.size()},
                    {"patchBytes", patch_bytes}});
        return Json{{"schemaVersion", "1.0"},
                    {"kind", "ALTERATION"},
                    {"imageId", image_id},
                    {"revision", summary.revision},
                    {"summary", alteration_summary(prepared->operations)},
                    {"objectCount", summary.object_count},
                    {"validation", {{"valid", summary.validation.valid()}, {"issueCount", issue_count}}},
                    {"warnings", Json::array()},
                    {"applied", !patches.empty()},
                    {"operations", std::move(operations)}};
    };
    if (!registry.is_implemented("images.alter")) {
        auto bound = registry.bind("images.alter", alter_session);
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("images.deletion.inspect")) {
        auto bound = registry.bind(
            "images.deletion.inspect", [&images](const Json &input, const OperationContext &context) -> Result<Json> {
                try {
                    const auto image_id = input.at("imageId").get<std::string>();
                    const auto revision = input.at("expectedRevision").get<std::uint64_t>();
                    const auto targets = input.at("targetObjectIds").get<std::vector<std::string>>();
                    const auto cleanup = input.at("cleanupObjectIds").get<std::vector<std::string>>();
                    auto plan = images.plan_deletion(image_id, context.owner_id, revision, targets, cleanup);
                    if (!plan)
                        return std::unexpected(plan.error());
                    return deletion_inspection_json(plan->inspection);
                } catch (const Json::exception &) {
                    return std::unexpected(operation_error(
                        "invalid_request",
                        "imageId, expectedRevision, targetObjectIds, and cleanupObjectIds are required"));
                }
            });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("images.deletion.orphans.inspect")) {
        auto bound =
            registry.bind("images.deletion.orphans.inspect",
                          [&images](const Json &input, const OperationContext &context) -> Result<Json> {
                              try {
                                  const auto image_id = input.at("imageId").get<std::string>();
                                  const auto revision = input.at("expectedRevision").get<std::uint64_t>();
                                  const auto content_scope_id = input.at("contentScopeId").get<std::string>();
                                  auto inspection = images.inspect_wave_data_orphans(image_id, context.owner_id,
                                                                                     revision, content_scope_id);
                                  if (!inspection)
                                      return std::unexpected(inspection.error());
                                  return wave_data_orphan_inspection_json(*inspection);
                              } catch (const Json::exception &) {
                                  return std::unexpected(operation_error(
                                      "invalid_request", "imageId, expectedRevision, and contentScopeId are required"));
                              }
                          });
        if (!bound)
            return bound;
    }
    if (!registry.is_implemented("images.delete")) {
        auto bound = registry.bind(
            "images.delete",
            [&images, alter_session](const Json &input, const OperationContext &context) -> Result<Json> {
                std::string image_id;
                std::uint64_t revision{};
                std::vector<std::string> targets;
                std::vector<std::string> cleanup;
                try {
                    image_id = input.at("imageId").get<std::string>();
                    revision = input.at("expectedRevision").get<std::uint64_t>();
                    targets = input.at("targetObjectIds").get<std::vector<std::string>>();
                    cleanup = input.at("cleanupObjectIds").get<std::vector<std::string>>();
                } catch (const Json::exception &) {
                    return std::unexpected(operation_error(
                        "invalid_request",
                        "imageId, expectedRevision, targetObjectIds, and cleanupObjectIds are required"));
                }
                auto plan = images.plan_deletion(image_id, context.owner_id, revision, targets, cleanup);
                if (!plan)
                    return std::unexpected(plan.error());
                if (!plan->inspection.can_apply) {
                    const auto message = plan->inspection.blockers.empty() ? "object deletion is blocked"
                                                                           : plan->inspection.blockers.front().message;
                    return std::unexpected(operation_error("deletion_blocked", message));
                }
                auto altered = alter_session({{"imageId", image_id},
                                              {"expectedRevision", revision},
                                              {"manifest", {{"inline", deletion_manifest_json(plan->manifest)}}},
                                              {"inputBindings", Json::array()}},
                                             context);
                if (!altered)
                    return std::unexpected(altered.error());
                (*altered)["kind"] = "DELETION";
                (*altered)["deletedObjectIds"] = plan->inspection.selected_object_ids;
                std::vector<std::string> blocked;
                for (const auto &impact : plan->inspection.impacts) {
                    if (impact.status == "BLOCKED")
                        blocked.push_back(impact.object_id);
                }
                (*altered)["blockedObjectIds"] = std::move(blocked);
                (*altered)["freedClusters"] = plan->inspection.estimated_freed_clusters;
                return altered;
            });
        if (!bound)
            return bound;
    }
    return {};
}
