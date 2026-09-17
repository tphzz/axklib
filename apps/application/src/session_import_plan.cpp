#include "session_import_plan.hpp"

#include <filesystem>
#include <utility>

#include "axklib/application/secure_random.hpp"
#include "axklib/package_import_planning.hpp"

namespace axk::app::package_operations_internal {
Result<Json> store_session_import_plan(const std::shared_ptr<SessionPackageOperationState> &state, const Json &input,
                                       const OperationContext &context, const ImageSessionRead &session,
                                       const std::shared_ptr<const VerifiedPackageSet> &package_set,
                                       const std::optional<std::string> &replace_plan_token,
                                       Clock::time_point operation_started) {
    const auto parsed_identity = parse_session_identity(input);
    if (!parsed_identity)
        return std::unexpected(parsed_identity.error());
    const auto &identity = *parsed_identity;
    const auto diagnostic = [&](std::string_view phase, Clock::time_point started, const Json &details) {
        if (!context.diagnostic)
            return;
        auto event = details;
        event["event"] = "package_import_plan_phase";
        event["requestId"] = context.request_id;
        event["phase"] = phase;
        event["durationMs"] = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
        context.diagnostic(event);
    };
    std::size_t package_object_count{};
    for (const auto &package : package_set->packages)
        package_object_count += package.nodes.size();
    auto preparation = prepare_session_import(input, package_set->packages, session.volume_scopes_by_id);
    if (!preparation)
        return std::unexpected(preparation.error());

    const auto planning_started = Clock::now();
    const auto packages = std::span<const axk::PortablePackage>{package_set->packages};
    axk::package_import_internal::RetainedPackageImportStats planning_stats;
    const auto fingerprint = session.content_fingerprint(context.cancellation);
    if (!fingerprint)
        return std::unexpected(fingerprint.error());
    const axk::package_import_internal::RetainedPackageImportTarget target{
        session.reader,          std::filesystem::path{session.source.relative_path},
        session.media,           *fingerprint,
        session.catalog_objects, session.catalog_issues,
        &planning_stats,         true};
    auto plan = axk::package_import_internal::plan_package_import_retained(target, packages, preparation->request,
                                                                           context.cancellation);
    if (!plan)
        return std::unexpected(core_error(plan.error(), session.source.relative_path));
    diagnostic("planning", planning_started,
               {{"imageId", identity.first},
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
        SessionPackagePlanRecord{*token, context.owner_id, now + state->retention, identity.first, identity.second,
                                 package_set, std::move(*plan), false});
    {
        std::lock_guard lock{state->mutex};
        cleanup_session_plans(*state, now);
        if (replace_plan_token) {
            const auto found = state->plans.find(*replace_plan_token);
            if (found == state->plans.end() || found->second->owner_id != context.owner_id || found->second->claimed ||
                found->second->package_set != package_set) {
                return std::unexpected(
                    operation_error("package_plan_stale", "replacement package import plan changed while replanning"));
            }
        } else if (state->plans.size() >= state->maximum_plans) {
            return std::unexpected(operation_error("package_plan_capacity", "too many package import plans are active",
                                                   std::nullopt, true));
        }
        if (!replace_plan_token) {
            const auto retained = retained_session_package_bytes(*state);
            if (retained > state->maximum_retained_package_bytes ||
                package_set->retained_payload_bytes > state->maximum_retained_package_bytes - retained) {
                return std::unexpected(operation_error("package_plan_capacity",
                                                       "retained package import payload budget is exhausted",
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
               {{"imageId", identity.first},
                {"replacement", replace_plan_token.has_value()},
                {"retainedPackageBytes", package_set->retained_payload_bytes}});
    auto result = plan_json(record->plan, record->token, static_cast<std::uint64_t>(state->retention.count() * 60));
    result["imageId"] = record->image_id;
    result["revision"] = record->expected_revision;
    result["packages"] = session_package_summaries(packages, preparation->destination_volume_names);
    diagnostic("total", operation_started,
               {{"imageId", identity.first},
                {"revision", identity.second},
                {"cacheHit", replace_plan_token.has_value()},
                {"imageBytes", session.reader->size()},
                {"packageCount", package_set->packages.size()},
                {"packageObjectCount", package_object_count},
                {"packagePayloadBytes", package_set->retained_payload_bytes},
                {"targetPayloadBytesRead", planning_stats.target_payload_bytes_read},
                {"targetPayloadObjectsRead", planning_stats.target_payload_objects_read}});
    return result;
}
} // namespace axk::app::package_operations_internal
