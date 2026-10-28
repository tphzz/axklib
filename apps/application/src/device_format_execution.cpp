#include "device_format_internal.hpp"

#include <algorithm>
#include <format>

#include "private_storage.hpp"

namespace axk::app {
using namespace format_detail;

Result<std::shared_ptr<void>> DeviceFormatService::admit(const Json &input, const OperationContext &context) {
    if (!state_->devices.writes_supported() || context.mutation_id.empty() || context.mutation_id.size() > 128U ||
        !input.at("exclusiveUseAcknowledged").get<bool>() || !input.at("dataLossAcknowledged").get<bool>())
        return std::unexpected(
            Error{"device_format_confirmation", "Explicit format consent and a known operation identity are required"});
    const auto session = input.at("sessionId").get<std::string>();
    auto connection = state_->devices.implementation_->owned(session, context.owner_id);
    if (!connection)
        return std::unexpected(connection.error());
    const std::scoped_lock io{(*connection)->io};
    if ((*connection).use_count() > 2U)
        return std::unexpected(Error{"device_busy", "Eject device image sessions before formatting"});
    if (auto fresh = require_fresh(**connection); !fresh)
        return std::unexpected(fresh.error());
    const std::scoped_lock store{state_->mutex};
    const auto found = state_->plans.find(session);
    if (found == state_->plans.end() || found->second->token != input.at("planToken").get<std::string>())
        return std::unexpected(Error{"device_plan_stale", "Review the selected device again"});
    const auto plan = found->second;
    if (plan->inspection->owner != context.owner_id || plan->expires <= state_->now() || plan->consumed ||
        plan->revoked || plan->inspection->connection.lock() != *connection ||
        input.at("confirmationText").get<std::string>() !=
            plan->inspection->wire.at("confirmationText").get<std::string>())
        return std::unexpected(
            Error{"device_format_confirmation", "The reviewed device or typed confirmation is stale or incorrect"});
    const auto *target = find_target(**connection, plan->inspection->target.id);
    if (!target || target->read_only || target->sector_size != 512U ||
        target->offset != plan->inspection->target.offset || target->size_bytes != plan->inspection->target.size_bytes)
        return std::unexpected(Error{"device_changed", "The reviewed target geometry changed"});
    if (auto safe = require_safe(plan->inspection->safety); !safe)
        return std::unexpected(safe.error());
    auto &implementation = *state_->devices.implementation_;
    const std::scoped_lock admission{implementation.operation_admission};
    const auto fence = carrier_fence(**connection);
    if (auto clear = require_clear_carrier(implementation.directory / "operations", fence); !clear)
        return std::unexpected(clear.error());
    const auto key = context.owner_id + ':' + context.mutation_id;
    const auto directory =
        implementation.directory / "operations" / *digest(std::as_bytes(std::span{key.data(), key.size()}));
    std::error_code error;
    if (std::filesystem::exists(directory, error) || error)
        return std::unexpected(
            Error{"device_operation_exists", "Check the existing operation status; do not resubmit"});
    if (auto created = detail::prepare_private_directory(directory); !created)
        return std::unexpected(created.error());
    if (auto saved = save_json(directory / "physical-guards.json", guard_json(plan->guards), true); !saved)
        return std::unexpected(saved.error());
    Json record{{"owner", context.owner_id},
                {"operationId", context.mutation_id},
                {"kind", "format"},
                {"claimPolicy", "require_unmounted"},
                {"state", "no_write"},
                {"fenced", false},
                {"carrierFence", fence},
                {"recoveryMembership", plan->inspection->safety.recovery_membership},
                {"model", (*connection)->device.candidate.model},
                {"serial", (*connection)->device.candidate.serial},
                {"targetLabel", target->label},
                {"targetSize", target->size_bytes},
                {"targetOffset", target->offset},
                {"targetSectorSize", target->sector_size},
                {"operationSize", plan->size},
                {"identity", target_identity(**connection, *target)}};
    if (auto saved = save_json(directory / "operation.json", record, true); !saved)
        return std::unexpected(saved.error());
    auto lease = std::make_shared<Lease>();
    lease->connection = *connection;
    lease->plan = plan;
    lease->operation = context.mutation_id;
    lease->directory = directory;
    const auto retained = *connection;
    const auto operation = context.mutation_id;
    // The runtime releases this outside its own mutex, including queue rejection
    // and queued cancellation. Durable fencing prevents a delayed request writing.
    lease->release = [retained, operation, directory]() noexcept {
        const std::scoped_lock lock{retained->io};
        try {
            auto retained_record = load_json(directory / "operation.json");
            if (retained_record && retained_record->value("state", "") == "no_write") {
                (*retained_record)["fenced"] = true;
                if (!save_json(directory / "operation.json", *retained_record, false))
                    retained->invalidated = true;
            }
            if (!retained_record || retained_record->value("state", "") != "committed")
                retained->invalidated = true;
        } catch (...) {
            retained->invalidated = true;
        }
        if (retained->initialization_operation == operation)
            retained->initialization_operation.clear();
    };
    state_->pending[key] = lease;
    plan->consumed = true;
    (*connection)->initialization_operation = context.mutation_id;
    return std::static_pointer_cast<void>(lease);
}

Result<Json> DeviceFormatService::execute(const Json &, const OperationContext &context) {
    std::shared_ptr<Lease> lease;
    {
        const std::scoped_lock store{state_->mutex};
        const auto found = state_->pending.find(context.owner_id + ':' + context.mutation_id);
        if (found != state_->pending.end())
            lease = found->second.lock();
    }
    if (!lease)
        return std::unexpected(Error{"device_format_admission", "No admitted format operation owns this request"});
    auto connection = lease->connection;
    const std::scoped_lock io{connection->io};
    const auto plan = lease->plan;
    const auto *target = find_target(*connection, plan->inspection->target.id);
    if (!target || connection->initialization_operation != context.mutation_id)
        return std::unexpected(Error{"device_changed", "The format target is no longer owned"});
    const auto consent = [&]() -> Result<void> {
        if (auto bound = verify_bound(*connection); !bound)
            return bound;
        auto record = load_json(lease->directory / "operation.json");
        if (!record || record->value("fenced", false))
            return std::unexpected(Error{"device_operation_resolved", "This operation was fenced by status review"});
        if (plan->expires <= state_->now() || plan->revoked)
            return std::unexpected(Error{"device_plan_stale", "Format review expired before writing"});
        if (context.cancellation.is_cancelled())
            return std::unexpected(Error{"operation_cancelled", "Format request cancelled before writing"});
        return {};
    };
    if (auto checked = consent(); !checked)
        return std::unexpected(checked.error());
    if (auto checked = verify_physical_guards(*connection, plan->guards, 0U, {}, context.cancellation); !checked)
        return std::unexpected(checked.error());
    if (context.progress)
        context.progress->report({ProgressPhase::opening, 0U, {}, "Requesting non-dismounting format access", {}});
    if (auto claim = connection->device.transport->authorize_initialization(plan->inspection->safety); !claim)
        return std::unexpected(claim.error());
    // A late authorization reply must not revive cancelled or expired consent.
    if (auto checked = consent(); !checked)
        return std::unexpected(checked.error());
    if (auto checked = connection->device.transport->verify_initialization_claim(); !checked)
        return std::unexpected(checked.error());
    if (auto checked = verify_physical_guards(*connection, plan->guards, 0U, {}, context.cancellation); !checked)
        return std::unexpected(checked.error());
    auto initialization = plan_hds_initialization(plan->size, plan->partitions, context.cancellation);
    if (!initialization)
        return std::unexpected(Error{"device_format_layout", initialization.error().message});
    auto mutation =
        std::make_shared<Mutation>(connection, *target, lease->operation, lease->directory, false, plan->size);
    for (const auto &[offset, hash] : plan->guards)
        if (offset >= target->offset && offset - target->offset < plan->size)
            mutation->guards.emplace(offset - target->offset, hash);
    std::vector<AlterationJournalPatch> patches;
    for (auto &write : initialization->writes) {
        std::vector<std::byte> original(write.bytes.size());
        if (auto read = mutation->read_exact_at(write.offset, original); !read)
            return std::unexpected(Error{"device_read_failed", read.error().message});
        patches.push_back({write.offset, std::move(original), write.bytes});
    }
    if (context.progress)
        context.progress->report(
            {ProgressPhase::writing, 0U, {}, "Journaling and initializing Yamaha SFS metadata", {}});
    const auto prewrite = [&](std::shared_ptr<const RandomAccessReader> anticipated) -> Result<void> {
        if (auto checked = consent(); !checked)
            return checked;
        if (auto checked = connection->device.transport->verify_initialization_claim(); !checked)
            return checked;
        if (auto checked = verify_physical_guards(*connection, plan->guards, 0U, {}, context.cancellation); !checked)
            return checked;
        if (auto checked = check_initialized(std::move(anticipated), *initialization); !checked)
            return checked;
        return consent();
    };
    auto applied = state_->devices.apply(
        mutation, patches, context.cancellation, [&] { return check_initialized(mutation, *initialization); }, [] {},
        prewrite, consent);
    if (!applied)
        return std::unexpected(applied.error());
    return Json{{"operationId", lease->operation},
                {"sessionId", connection->id},
                {"targetId", target->id},
                {"sizeBytes", plan->size},
                {"partitionCount", plan->partitions}};
}
} // namespace axk::app
