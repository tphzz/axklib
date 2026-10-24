#include "device_sessions_internal.hpp"
#include "read_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <format>

#include "axklib/application/secure_random.hpp"
#include "private_storage.hpp"

namespace axk::app {
namespace {

DeviceSessionSummary summary(const device_detail::Connection &connection) {
    return {connection.id, connection.device.candidate, connection.device.targets,
            !connection.authorized_target.empty(), connection.invalidated};
}

Result<void> require_target(const device_detail::Connection &connection, const DeviceTargetDescription *target) {
    if (connection.invalidated)
        return std::unexpected(Error{"device_disconnected", "device session is invalidated; select it again"});
    if (!target)
        return std::unexpected(Error{"device_target_missing", "device target is unavailable"});
    if (target->size_bytes == 0U || target->size_bytes > maximum_sampler_device_bytes ||
        target->offset > connection.device.transport->size() ||
        target->size_bytes > connection.device.transport->size() - target->offset ||
        target->offset % connection.device.transport->sector_size() != 0U ||
        target->size_bytes % connection.device.transport->sector_size() != 0U)
        return std::unexpected(Error{"device_layout_invalid", "device target geometry is invalid"});
    return device_detail::verify_bound(connection);
}

} // namespace

DeviceSessionManager::DeviceSessionManager(std::filesystem::path directory, bool enabled, bool allow_writes,
                                           std::shared_ptr<DeviceProvider> provider)
    : implementation_(std::make_unique<Implementation>()) {
    implementation_->directory = std::move(directory);
    implementation_->provider = provider ? std::move(provider) : native_device_provider();
    implementation_->enabled = enabled && implementation_->provider->available();
    implementation_->allow_writes = allow_writes;
    if (implementation_->enabled) {
        implementation_->ready = detail::prepare_private_directory(implementation_->directory).has_value();
        if (implementation_->ready)
            implementation_->ready =
                detail::prepare_private_directory(implementation_->directory / "operations").has_value();
    }
}
DeviceSessionManager::~DeviceSessionManager() = default;
bool DeviceSessionManager::enabled() const noexcept { return implementation_->enabled && implementation_->ready; }
bool DeviceSessionManager::writes_supported() const noexcept { return enabled() && implementation_->allow_writes; }

Result<std::shared_ptr<device_detail::Connection>>
DeviceSessionManager::Implementation::owned(std::string_view id, std::string_view owner) const {
    const std::scoped_lock lock{mutex};
    const auto found = sessions.find(id);
    if (!enabled || !ready || owner.empty() || found == sessions.end() || found->second->owner != owner)
        return std::unexpected(Error{"device_session_missing", "owned device session is unavailable"});
    return found->second;
}

Result<std::vector<DeviceCandidate>> DeviceSessionManager::list(std::string_view owner) {
    if (!enabled() || owner.empty())
        return std::unexpected(Error{"devices_unavailable", "device access is disabled for this server"});
    const std::scoped_lock lock{implementation_->mutex};
    auto candidates = implementation_->provider->list();
    if (!candidates)
        return candidates;
    auto &owned = implementation_->candidates[std::string{owner}];
    owned.clear();
    for (const auto &candidate : *candidates)
        owned.emplace(candidate.id);
    return candidates;
}

Result<DeviceSessionSummary> DeviceSessionManager::connect(std::string_view candidate_id, std::string_view owner,
                                                           std::string_view layout, std::string_view xml) {
    if (!enabled() || owner.empty())
        return std::unexpected(Error{"devices_unavailable", "device access is disabled for this server"});
    const std::scoped_lock lock{implementation_->mutex};
    if (implementation_->sessions.size() >= 8U)
        return std::unexpected(Error{"device_capacity_exhausted", "close an owned device before opening another"});
    const auto candidates = implementation_->candidates.find(owner);
    if (candidates == implementation_->candidates.end() || !candidates->second.contains(std::string{candidate_id}))
        return std::unexpected(Error{"device_selection_stale", "refresh the device list and select again"});
    auto connected = implementation_->provider->connect(candidate_id, layout, xml);
    if (!connected)
        return std::unexpected(connected.error());
    if (!connected->transport || connected->transport->carrier_identity().empty() ||
        connected->transport->sector_size() == 0U)
        return std::unexpected(Error{"device_identity_unavailable", "device carrier identity is unavailable"});
    for (const auto &[id, existing] : implementation_->sessions) {
        static_cast<void>(id);
        if (existing->device.transport->carrier_identity() == connected->transport->carrier_identity())
            return std::unexpected(Error{"device_busy", "another session already owns this USB carrier"});
    }
    auto id = secure_random_hex(16U);
    if (!id)
        return std::unexpected(id.error());
    auto connection = std::make_shared<device_detail::Connection>();
    connection->id = *id;
    connection->owner = owner;
    connection->device = std::move(*connected);
    for (auto &target : connection->device.targets) {
        if (auto checked = require_target(*connection, &target); !checked)
            return std::unexpected(checked.error());
    }
    implementation_->sessions.emplace(connection->id, connection);
    return summary(*connection);
}

Result<DeviceSessionSummary> DeviceSessionManager::status(std::string_view id, std::string_view owner) const {
    auto connection = implementation_->owned(id, owner);
    if (!connection)
        return std::unexpected(connection.error());
    const std::scoped_lock lock{(*connection)->io};
    if (!device_detail::verify_bound(**connection)) {
        (*connection)->invalidated = true;
        (*connection)->authorized_target.clear();
    }
    return summary(**connection);
}

Result<void> DeviceSessionManager::close(std::string_view id, std::string_view owner) {
    const std::scoped_lock lock{implementation_->mutex};
    const auto found = implementation_->sessions.find(id);
    if (found == implementation_->sessions.end() || found->second->owner != owner)
        return std::unexpected(Error{"device_session_missing", "owned device session is unavailable"});
    if (found->second.use_count() > 1)
        return std::unexpected(Error{"device_busy", "close device image sessions before releasing the device"});
    implementation_->sessions.erase(found);
    return {};
}

Result<void> DeviceSessionManager::authorize_writes(std::string_view id, std::string_view target_id,
                                                    std::string_view owner, bool acknowledged,
                                                    const CancellationToken &cancellation, ProgressSink *progress) {
    if (!writes_supported() || !acknowledged)
        return std::unexpected(Error{"device_write_authorization_required",
                                     "write support and exclusive sampler/host use acknowledgement are required"});
    auto connection = implementation_->owned(id, owner);
    if (!connection)
        return std::unexpected(connection.error());
    const std::scoped_lock lock{(*connection)->io};
    const auto *target = device_detail::find_target(**connection, target_id);
    if (auto checked = require_target(**connection, target); !checked)
        return checked;
    if (target->read_only || target->sector_size != 512U || !(*connection)->metadata_guards.contains(target->id))
        return std::unexpected(Error{"device_read_only", "open a supported SFS target before authorizing writes"});
    if (auto checked = device_detail::verify_guards(
            **connection, *target, (*connection)->metadata_guards.at(target->id), {}, cancellation, progress);
        !checked)
        return checked;
    if (progress)
        progress->report({ProgressPhase::opening, 0U, std::nullopt, "Requesting write permission", {}});
    if (const auto active = cancellation.check(); !active)
        return std::unexpected(Error{"operation_cancelled", active.error().message});
    if (auto authorized = (*connection)->device.transport->authorize_writes(); !authorized) {
        (*connection)->invalidated = true;
        return authorized;
    }
    if (auto checked =
            device_detail::verify_guards(**connection, *target, (*connection)->metadata_guards.at(target->id), {},
                                         cancellation, progress, "Verifying write access");
        !checked) {
        (*connection)->invalidated = true;
        return checked;
    }
    (*connection)->authorized_target = target->id;
    return {};
}

Result<OpenedDeviceSource> DeviceSessionManager::open_source(std::string_view id, std::string_view target_id,
                                                             std::string_view owner) {
    auto connection = implementation_->owned(id, owner);
    if (!connection)
        return std::unexpected(connection.error());
    const std::scoped_lock lock{(*connection)->io};
    const auto *target = device_detail::find_target(**connection, target_id);
    if (auto checked = require_target(**connection, target); !checked)
        return std::unexpected(checked.error());
    auto source = std::make_shared<device_detail::Source>(*connection, *target);
    return OpenedDeviceSource{
        source,
        [value = *connection, revision = (*connection)->revision]() -> Result<void> {
            std::unique_lock access{value->io, std::defer_lock};
            {
                const ReadDiagnosticTimer wait{ReadMetric::connection_wait};
                access.lock();
            }
            if (value->invalidated || value->revision != revision)
                return std::unexpected(
                    Error{"device_disconnected", "device session is invalidated or its revision changed"});
            auto checked = device_detail::verify_bound(*value);
            if (!checked) {
                value->invalidated = true;
                value->authorized_target.clear();
            }
            return checked;
        },
        [source](const CancellationToken &cancellation, ProgressSink *progress) {
            return source->seal(cancellation, progress);
        },
        std::format("{}:{}", id, (*connection)->revision)};
}

Result<std::shared_ptr<MutationTarget>> DeviceSessionManager::begin_mutation(std::string_view id,
                                                                             std::string_view target_id,
                                                                             std::string_view owner,
                                                                             std::string_view operation_id) {
    auto connection = implementation_->owned(id, owner);
    if (!connection)
        return std::unexpected(connection.error());
    std::unique_lock lock{(*connection)->io};
    const auto *target = device_detail::find_target(**connection, target_id);
    if (auto checked = require_target(**connection, target); !checked)
        return std::unexpected(checked.error());
    if (!writes_supported() || (*connection)->authorized_target != target_id || target->read_only)
        return std::unexpected(
            Error{"device_write_authorization_required", "authorize writes for this owned target first"});
    if ((*connection)->mutation_active)
        return std::unexpected(Error{"device_busy", "a device mutation is already active"});
    if (operation_id.empty() || operation_id.size() > 128U)
        return std::unexpected(
            Error{"device_operation_identity_required", "a known operation identity is required before writing"});
    const auto key = std::format("{}:{}", owner, operation_id);
    const auto directory = implementation_->directory / "operations" /
                           *device_detail::digest(std::as_bytes(std::span{key.data(), key.size()}));
    const std::scoped_lock admission{implementation_->operation_admission};
    std::error_code error;
    if (std::filesystem::exists(directory, error) || error)
        return std::unexpected(
            Error{"device_operation_exists", "operation identity already exists; check status, do not resubmit"});
    auto operations = this->operations(owner);
    if (!operations)
        return std::unexpected(operations.error());
    if (operations->size() >= 4096U)
        return std::unexpected(Error{"device_operation_capacity", "device operation status retention is full"});
    for (const auto &operation : *operations) {
        if (operation.target_identity == device_detail::target_identity(**connection, *target) &&
            (operation.state == "recovery_required" || operation.state == "prepared"))
            return std::unexpected(
                Error{"device_recovery_required", "resolve the pending device operation before writing"});
    }
    if (auto prepared = detail::prepare_private_directory(directory); !prepared)
        return std::unexpected(prepared.error());
    const device_detail::Json record{{"operationId", operation_id},
                                     {"owner", owner},
                                     {"state", "no_write"},
                                     {"model", (*connection)->device.candidate.model},
                                     {"serial", (*connection)->device.candidate.serial},
                                     {"targetLabel", target->label},
                                     {"targetSize", target->size_bytes},
                                     {"targetOffset", target->offset},
                                     {"targetSectorSize", target->sector_size},
                                     {"identity", device_detail::target_identity(**connection, *target)}};
    if (auto saved = device_detail::save_json(directory / "operation.json", record, true); !saved)
        return std::unexpected(saved.error());
    auto mutation =
        std::make_shared<device_detail::Mutation>(*connection, *target, std::string{operation_id}, directory);
    if (auto verified = mutation->verify_unchanged(); !verified)
        return std::unexpected(verified.error());
    return mutation;
}
} // namespace axk::app
