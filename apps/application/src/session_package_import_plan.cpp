#include "package_operations_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace axk::app::package_operations_internal {
namespace {

bool valid_volume_name(std::string_view name) {
    return !name.empty() && name.size() <= 16U && name.front() != ' ' && name.back() != ' ' &&
           std::ranges::all_of(name, [](unsigned char character) { return character >= 0x20U && character <= 0x7eU; });
}

std::string unique_volume_name(std::string_view requested, std::set<std::string, std::less<>> &reserved) {
    if (!reserved.contains(requested)) {
        auto result = std::string{requested};
        reserved.insert(result);
        return result;
    }
    for (std::size_t suffix = 2U; suffix <= 999U; ++suffix) {
        const auto suffix_text = std::format(" {}", suffix);
        const auto base_size = std::min(requested.size(), 16U - suffix_text.size());
        auto candidate = std::string{requested.substr(0U, base_size)} + suffix_text;
        if (!reserved.contains(candidate)) {
            reserved.insert(candidate);
            return candidate;
        }
    }
    return {};
}

Result<std::uint8_t> partition_index(const Json &destination) {
    const auto value = destination.at("partitionIndex").get<std::uint32_t>();
    if (value > std::numeric_limits<std::uint8_t>::max())
        return std::unexpected(operation_error("invalid_request", "partition index is out of range"));
    return static_cast<std::uint8_t>(value);
}

Result<void> append_policy(const Json &input, std::size_t package_count, axk::PackageImportPolicy &policy) {
    const auto valid_index = [package_count](std::size_t index) { return index < package_count; };
    if (input.contains("renames")) {
        for (const auto &rename : input.at("renames")) {
            const auto package_index = rename.at("packageIndex").get<std::size_t>();
            if (!valid_index(package_index))
                return std::unexpected(operation_error("invalid_request", "package rename index is out of range"));
            policy.renames.push_back({package_index, rename.at("nodeId").get<std::string>(),
                                      rename.at("destinationName").get<std::string>()});
        }
    }
    if (input.contains("programSlotAssignments")) {
        for (const auto &assignment : input.at("programSlotAssignments")) {
            const auto package_index = assignment.at("packageIndex").get<std::size_t>();
            const auto slot = assignment.at("destinationSlot").get<std::uint32_t>();
            if (!valid_index(package_index) || slot < 1U || slot > 128U) {
                return std::unexpected(operation_error("invalid_request", "Program slot assignment is out of range"));
            }
            policy.program_slot_assignments.push_back(
                {package_index, assignment.at("nodeId").get<std::string>(), static_cast<std::uint8_t>(slot)});
        }
    }
    if (input.contains("opaqueSequenceDecisions")) {
        for (const auto &decision : input.at("opaqueSequenceDecisions")) {
            const auto package_index = decision.at("packageIndex").get<std::size_t>();
            const auto action = decision.at("action").get<std::string>();
            if (!valid_index(package_index) || (action != "preserve-unchanged" && action != "skip")) {
                return std::unexpected(operation_error("invalid_request", "opaque Sequence decision is invalid"));
            }
            policy.opaque_sequence_decisions.push_back({package_index, decision.at("nodeId").get<std::string>(),
                                                        action == "preserve-unchanged"
                                                            ? axk::PackageOpaqueSequenceAction::preserve_unchanged
                                                            : axk::PackageOpaqueSequenceAction::skip});
        }
    }
    return {};
}

} // namespace

Result<SessionImportPreparation>
prepare_session_import(const Json &input, std::span<const axk::PortablePackage> packages,
                       const std::unordered_map<std::string, ImageVolumeScopeIdentity> &volume_scopes_by_id) {
    SessionImportPreparation result;
    try {
        const auto &destination = input.at("destination");
        const auto kind = destination.at("kind").get<std::string>();
        auto partition = partition_index(destination);
        if (!partition)
            return std::unexpected(partition.error());
        if (kind == "EXISTING_VOLUME") {
            const auto volume_name = destination.at("volumeName").get<std::string>();
            if (volume_name.empty())
                return std::unexpected(operation_error("invalid_request", "destination volume name is required"));
            result.destination_volume_names.assign(packages.size(), volume_name);
            for (std::size_t package_index = 0U; package_index < packages.size(); ++package_index) {
                for (std::size_t root_index = 0U; root_index < packages[package_index].roots.size(); ++root_index) {
                    result.request.root_destinations.push_back(
                        {package_index, root_index, *partition, {}, volume_name, {}, {}, false});
                }
            }
        } else if (kind == "CREATE_VOLUME") {
            if (packages.size() != 1U)
                return std::unexpected(
                    operation_error("invalid_request", "CREATE_VOLUME requires exactly one import source"));
            const auto volume_name = destination.at("volumeName").get<std::string>();
            if (!valid_volume_name(volume_name))
                return std::unexpected(operation_error("invalid_request", "destination volume name is invalid"));
            if (std::ranges::any_of(volume_scopes_by_id, [&](const auto &entry) {
                    return entry.second.partition_index == *partition && entry.second.display_name == volume_name;
                })) {
                return std::unexpected(
                    operation_error("package_destination_conflict", "destination volume name already exists"));
            }
            result.destination_volume_names.assign(1U, volume_name);
            for (std::size_t root_index = 0U; root_index < packages.front().roots.size(); ++root_index) {
                result.request.root_destinations.push_back({0U, root_index, *partition, {}, volume_name, {}, {}, true});
            }
        } else if (kind == "CREATE_VOLUMES_FROM_HINTS") {
            std::map<std::size_t, std::string> overrides;
            for (const auto &override_value : destination.value("volumeNameOverrides", Json::array())) {
                const auto package_index = override_value.at("packageIndex").get<std::size_t>();
                const auto volume_name = override_value.at("volumeName").get<std::string>();
                if (package_index >= packages.size() || !valid_volume_name(volume_name) ||
                    !overrides.emplace(package_index, volume_name).second) {
                    return std::unexpected(operation_error(
                        "invalid_request", "volume name overrides must be unique valid package mappings"));
                }
            }
            std::set<std::string, std::less<>> reserved;
            for (const auto &[id, scope] : volume_scopes_by_id) {
                static_cast<void>(id);
                if (scope.partition_index == *partition)
                    reserved.insert(scope.display_name);
            }
            for (const auto &[package_index, volume_name] : overrides) {
                static_cast<void>(package_index);
                if (!reserved.insert(volume_name).second) {
                    return std::unexpected(
                        operation_error("package_destination_conflict", "destination volume name already exists"));
                }
            }
            result.destination_volume_names.reserve(packages.size());
            for (std::size_t package_index = 0U; package_index < packages.size(); ++package_index) {
                const auto &package = packages[package_index];
                if (package.kind != axk::PackageKind::volume || package.roots.size() != 1U ||
                    package.roots.front().kind != axk::PackageRootKind::volume ||
                    !valid_volume_name(package.roots.front().display_name)) {
                    return std::unexpected(operation_error("package_volume_hint_invalid",
                                                           "each batch package must contain one valid Volume root"));
                }
                auto volume_name = overrides.contains(package_index)
                                       ? overrides.at(package_index)
                                       : unique_volume_name(package.roots.front().display_name, reserved);
                if (volume_name.empty()) {
                    return std::unexpected(operation_error("package_destination_conflict",
                                                           "no unique destination volume name is available"));
                }
                result.destination_volume_names.push_back(volume_name);
                result.request.root_destinations.push_back(
                    {package_index, 0U, *partition, {}, std::move(volume_name), {}, {}, true});
            }
        } else {
            return std::unexpected(operation_error("invalid_request", "package destination kind is unsupported"));
        }
        if (auto policy = append_policy(input, packages.size(), result.request.policy); !policy)
            return std::unexpected(policy.error());
    } catch (const Json::exception &) {
        return std::unexpected(operation_error("invalid_request", "package destination mappings are malformed"));
    }
    return result;
}

Json session_package_summaries(std::span<const axk::PortablePackage> packages,
                               std::span<const std::string> destination_volume_names) {
    auto result = Json::array();
    for (std::size_t package_index = 0U; package_index < packages.size(); ++package_index) {
        const auto &package = packages[package_index];
        std::uint64_t payload_bytes{};
        std::map<std::string, std::size_t> counts;
        for (const auto &node : package.nodes) {
            payload_bytes += node.payload_size_bytes;
            ++counts[node.object_type];
        }
        result.push_back(
            {{"packageIndex", package_index},
             {"packageId", package.package_id},
             {"sourceVolumeName", package.roots.empty() ? std::string{} : package.roots.front().display_name},
             {"destinationVolumeName", destination_volume_names[package_index]},
             {"objectCount", package.nodes.size()},
             {"payloadBytes", payload_bytes},
             {"objectCounts",
              {{"programs", counts["PROG"]},
               {"sampleBanks", counts["SBAC"]},
               {"samples", counts["SBNK"]},
               {"waveData", counts["SMPL"]},
               {"sequences", counts["SEQU"]}}}});
    }
    return result;
}

} // namespace axk::app::package_operations_internal
