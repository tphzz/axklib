#include "axklib/application/volume_capacity.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "axklib/application/image_sessions.hpp"
#include "axklib/application/operation_registry.hpp"
#include "axklib/capacity_admission_internal.hpp"
#include "write_operations_internal.hpp"

namespace axk::app {
namespace {
nlohmann::json metric(const std::optional<std::uint64_t> &value) {
    return value ? nlohmann::json(*value) : nlohmann::json(nullptr);
}
} // namespace

nlohmann::json capacity_admission_json(const VolumeCapacityAdmission &admission) {
    auto reports = nlohmann::json::array();
    for (const auto &report : admission.reports)
        reports.push_back(volume_capacity_json(report));
    return {{"target", a_series_load_target_name(admission.target)},
            {"reports", std::move(reports)},
            {"allowed", admission.allowed}};
}

Result<VolumeCapacityPolicy> capacity_policy(const nlohmann::json &input) {
    VolumeCapacityPolicy result;
    if (!input.contains("capacityPolicy"))
        return result;
    try {
        const auto &policy = input.at("capacityPolicy");
        if (!policy.is_object() || policy.size() != 1U)
            return std::unexpected{Error{"invalid_request", "capacityPolicy requires only target"}};
        const auto target = policy.at("target").get<std::string>();
        if (target == "A3000")
            result.target = ASeriesLoadTarget::a3000;
        else if (target != "A4000_A5000")
            return std::unexpected{Error{"invalid_request", "Invalid A-Series capacity target"}};
        return result;
    } catch (const nlohmann::json::exception &) {
        return std::unexpected{Error{"invalid_request", "capacityPolicy requires only target"}};
    }
}

Result<VolumeCapacityAdmission> inspect_frozen_capacity(std::shared_ptr<const RandomAccessReader> frozen,
                                                        const VolumeCapacityAdmission &prepared,
                                                        const VolumeCapacityPolicy &policy,
                                                        const CancellationToken &cancellation) {
    if (prepared.reports.empty()) {
        VolumeCapacityAdmission unchanged;
        unchanged.target = policy.target;
        return unchanged;
    }
    OpenOptions options;
    options.cancellation = cancellation;
    const auto image = open_image(frozen, {}, options);
    if (!image)
        return std::unexpected{write_operations_internal::core_error(image.error())};
    std::vector<axk::detail::CapacityDestination> destinations;
    for (const auto &report : prepared.reports)
        destinations.push_back({report.partition, report.volume_name, report.volume_directory});
    auto current = axk::detail::inspect_capacity_destinations(*image, destinations, policy, cancellation);
    if (!current)
        return std::unexpected{write_operations_internal::core_error(current.error())};
    return *current;
}

nlohmann::json volume_capacity_json(const VolumeCapacityReport &report) {
    auto profiles = nlohmann::json::array();
    for (const auto &profile : report.profiles) {
        auto reasons = nlohmann::json::array();
        for (const auto &reason : profile.reasons)
            reasons.push_back({{"code", reason.code}, {"message", reason.message}});
        profiles.push_back({{"target", a_series_load_target_name(profile.target)},
                            {"status", volume_capacity_status_name(profile.status)},
                            {"parameterByteLimit", profile.parameter_byte_limit},
                            {"sharedObjectSlotLimit", profile.shared_object_slot_limit},
                            {"baselineBytes", profile.baseline_bytes},
                            {"baselineSlots", profile.baseline_slots},
                            {"minimumResidentBytes", metric(profile.minimum_resident_bytes)},
                            {"minimumResidentSlots", metric(profile.minimum_resident_slots)},
                            {"residentBytes", metric(profile.resident_bytes)},
                            {"peakBytes", metric(profile.peak_bytes)},
                            {"peakSlots", metric(profile.peak_slots)},
                            {"reasons", std::move(reasons)}});
    }
    auto counts = nlohmann::json::array();
    for (const auto &count : report.object_counts)
        counts.push_back({{"type", count.type}, {"count", count.count}});
    return {{"partitionIndex", report.partition.value}, {"volumeDirectoryId", report.volume_directory.value},
            {"volumeName", report.volume_name},         {"baseline", "FRESH_POWER_ON_WIPE_VOLUME_LOAD"},
            {"objectCounts", std::move(counts)},        {"profiles", std::move(profiles)}};
}

Result<void> bind_volume_capacity_operations(OperationRegistry &registry, ImageSessionManager &images) {
    return registry.bind(
        "images.volume_capacity.inspect",
        [&images](const nlohmann::json &input, const OperationContext &context) -> Result<nlohmann::json> {
            const auto image = input.at("imageId").get<std::string>();
            const auto revision = input.at("expectedRevision").get<std::uint64_t>();
            const auto scope = input.at("contentScopeId").get<std::string>();
            const auto result = images.volume_capacity(image, context.owner_id, revision, scope, context.cancellation);
            if (!result)
                return std::unexpected{result.error()};
            return nlohmann::json{{"imageId", image},
                                  {"revision", revision},
                                  {"contentScopeId", scope},
                                  {"report", volume_capacity_json(*result)}};
        });
}
} // namespace axk::app
