#include "device_format_internal.hpp"

#include <algorithm>

#include "axklib/application/secure_random.hpp"

namespace axk::app {
using namespace format_detail;

DeviceFormatService::DeviceFormatService(DeviceSessionManager &devices, Now now)
    : state_(std::make_shared<State>(devices, std::move(now))) {}

Result<Json> DeviceFormatService::inspect(const Json &input, const OperationContext &context) {
    const auto session = input.at("sessionId").get<std::string>();
    const auto target_id = input.at("targetId").get<std::string>();
    auto connection = state_->devices.implementation_->owned(session, context.owner_id);
    if (!connection)
        return std::unexpected(connection.error());
    const std::scoped_lock lock{(*connection)->io};
    if ((*connection).use_count() > 2U)
        return std::unexpected(Error{"device_busy", "Eject the current device workspace before preparing a format"});
    if (auto fresh = require_fresh(**connection); !fresh)
        return std::unexpected(fresh.error());
    const auto *target = find_target(**connection, target_id);
    if (!target)
        return std::unexpected(Error{"device_target_missing", "Select a current device target"});
    {
        const std::scoped_lock store{state_->mutex};
        if (auto old = state_->plans.find(session); old != state_->plans.end())
            old->second->revoked = true;
        state_->inspections.erase(session);
    }
    auto inspected = (*connection)->device.transport->inspect_write_safety();
    if (!inspected)
        return std::unexpected(inspected.error());
    if (context.progress)
        context.progress->report({ProgressPhase::reading, 0U, {}, "Inspecting existing device contents", {}});
    auto contents = inspect_contents(*(*connection)->device.transport, *target, context.cancellation);
    if (!contents)
        return std::unexpected(contents.error());
    auto token = secure_random_hex(16U);
    if (!token)
        return std::unexpected(token.error());
    const auto size = std::min(target->size_bytes, maximum_hds_size);
    auto options = hds_creation_partition_options(size);
    Json partition_options = Json::array();
    if (options)
        for (const auto &option : *options)
            partition_options.push_back(
                {{"partitionCount", option.partition_count},
                 {"partitionSizeBytes", option.partitions.front().filesystem_sector_count * 512U},
                 {"unusedTailBytes", option.unused_tail_sectors * 512U}});
    Json mounts = Json::array();
    for (const auto &volume : inspected->mounted)
        mounts.push_back({{"device", device_text(volume.device)},
                          {"mountPoint", device_text(volume.mount_point)},
                          {"filesystem", device_text(volume.filesystem)},
                          {"label", device_text(volume.label)}});
    Json detected = Json::array();
    for (const auto &entry : contents->contents)
        detected.push_back({{"kind", entry.kind},
                            {"type", entry.type},
                            {"label", entry.label},
                            {"offsetBytes", entry.offset},
                            {"sizeBytes", entry.size},
                            {"validation", entry.validated ? "STRUCTURE_VALIDATED" : "SIGNATURE_ONLY"}});
    std::vector<std::string> blocked;
    {
        const std::scoped_lock admission{state_->devices.implementation_->operation_admission};
        if (auto clear = require_clear_carrier(state_->devices.implementation_->directory / "operations",
                                               carrier_fence(**connection));
            !clear)
            blocked.push_back(clear.error().message);
    }
    if (!state_->devices.writes_supported())
        blocked.push_back("This platform is read-only for physical devices.");
    if (target->read_only || target->sector_size != 512U)
        blocked.push_back("The selected target does not support 512-byte sector writes.");
    if (!inspected->complete)
        blocked.push_back("Native device safety inspection is incomplete.");
    if (!inspected->non_dismounting_claim_supported)
        blocked.push_back("A complete non-dismounting carrier claim cannot be established.");
    if (!inspected->mounted.empty())
        blocked.push_back("Unmount all volumes on this USB carrier using the operating system, then inspect again.");
    if (!contents->complete)
        blocked.push_back("Existing partition or filesystem inspection is incomplete.");
    if (contents->crosses_target || contents->shared_partition_metadata)
        blocked.push_back(
            "Existing structures cross the selected target boundary or share outside partition metadata.");
    if (!options || options->empty())
        blocked.push_back("The target capacity cannot contain a supported SFS layout.");
    if (inspected->identity.device_path.empty())
        blocked.push_back("The native device identifier is unavailable.");
    auto warnings = contents->warnings;
    warnings.insert(warnings.end(), inspected->notices.begin(), inspected->notices.end());
    if ((*connection)->device.candidate.serial.empty() || inspected->identity.usb_serial.empty())
        warnings.push_back("The reader may not report the inserted card's identity. Check the physical device/card; "
                           "USB descriptors can identify only the reader.");
    auto candidate = (*connection)->device.candidate;
    candidate.identity = inspected->identity;
    Json wire{{"inspectionToken", *token},
              {"sessionId", session},
              {"candidate", candidate_json(candidate)},
              {"target", target_json(*target, candidate)},
              {"contents", detected},
              {"mountedVolumes", mounts},
              {"warnings", warnings},
              {"blockedReasons", blocked},
              {"complete", inspected->complete && contents->complete},
              {"formattedSizeBytes", size},
              {"unusedSuffixBytes", target->size_bytes - size},
              {"partitionOptions", partition_options},
              {"confirmationText", device_confirmation(candidate, *target, "ERASE")}};
    auto record =
        std::make_shared<Inspection>(Inspection{*token, context.owner_id, session, *target, *inspected, *connection,
                                                std::move(*contents), wire, state_->now() + std::chrono::minutes{15}});
    const std::scoped_lock store{state_->mutex};
    state_->prune();
    if (state_->inspections.size() >= 8U && !state_->inspections.contains(session))
        return std::unexpected(Error{"device_plan_capacity", "Close an unused device inspection first"});
    state_->inspections[session] = std::move(record);
    return wire;
}

Result<Json> DeviceFormatService::plan(const Json &input, const OperationContext &context) {
    const auto session = input.at("sessionId").get<std::string>();
    auto connection = state_->devices.implementation_->owned(session, context.owner_id);
    if (!connection)
        return std::unexpected(connection.error());
    const std::scoped_lock lock{(*connection)->io};
    if (auto fresh = require_fresh(**connection); !fresh)
        return std::unexpected(fresh.error());
    std::shared_ptr<Inspection> inspected;
    {
        const std::scoped_lock store{state_->mutex};
        state_->prune();
        const auto found = state_->inspections.find(session);
        if (found == state_->inspections.end() || found->second->owner != context.owner_id ||
            found->second->token != input.at("inspectionToken").get<std::string>())
            return std::unexpected(Error{"device_inspection_stale", "Inspect the selected device again"});
        inspected = found->second;
        if (!inspected->wire.at("blockedReasons").empty())
            return std::unexpected(
                Error{"device_format_blocked", "Resolve the device safety warnings before formatting"});
    }
    auto safety = (*connection)->device.transport->inspect_write_safety();
    if (!safety)
        return std::unexpected(safety.error());
    if (auto safe = require_safe(*safety); !safe)
        return std::unexpected(safe.error());
    if (safety->membership != inspected->safety.membership ||
        safety->identity.device_path != inspected->safety.identity.device_path)
        return std::unexpected(
            Error{"device_inspection_stale", "Device safety state changed; inspect the device again"});
    const auto count = input.at("partitionCount").get<std::uint8_t>();
    const auto size = std::min(inspected->target.size_bytes, maximum_hds_size);
    auto initialization = plan_hds_initialization(size, count, context.cancellation);
    if (!initialization)
        return std::unexpected(Error{"device_format_layout", initialization.error().message});
    if (auto checked = verify_physical_guards(**connection, inspected->contents.guards, 0U, {}, context.cancellation);
        !checked)
        return std::unexpected(checked.error());
    auto guards = read_initialization_guards(**connection, inspected->target, *initialization,
                                             inspected->contents.guards, context.cancellation);
    if (!guards)
        return std::unexpected(guards.error());
    auto token = secure_random_hex(16U);
    if (!token)
        return std::unexpected(token.error());
    auto plan = std::make_shared<Plan>(Plan{*token, inspected, std::move(*guards), size, count,
                                            state_->now() + std::chrono::minutes{15}, false, false});
    {
        const std::scoped_lock store{state_->mutex};
        if (auto old = state_->plans.find(session); old != state_->plans.end())
            old->second->revoked = true;
        state_->plans[session] = plan;
    }
    return Json{
        {"planToken", *token}, {"inspection", inspected->wire}, {"partitionCount", count}, {"expiresInSeconds", 900U}};
}
} // namespace axk::app
