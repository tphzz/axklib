#include "package_import_internal.hpp"

#include <algorithm>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <tuple>

#include "axklib/package_archive.hpp"

namespace axk {
namespace {

Error planner_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

std::string digest_text(std::string_view value) {
    return package_internal::hex_digest(package_internal::sha256(std::as_bytes(std::span{value})));
}

void append_field(std::string &target, std::string_view value) { target += std::format("{}:{};", value.size(), value); }

template <typename Integer> void append_integer(std::string &target, Integer value) {
    append_field(target, std::to_string(value));
}

bool valid_digest(std::string_view value) {
    return value.size() == 64U && std::ranges::all_of(value, [](unsigned char byte) {
               return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
           });
}

} // namespace

namespace package_import_internal {

std::string plan_identity(const PackageImportPlan &plan) {
    std::string source;
    append_field(source, plan.schema_version);
    append_integer(source, static_cast<std::uint8_t>(plan.target_kind));
    append_field(source, plan.target_snapshot_id);
    append_field(source, plan.policy_digest);
    for (const auto &package_id : plan.package_ids)
        append_field(source, package_id);
    for (const auto &destination : plan.destinations) {
        append_integer(source, destination.partition_index);
        append_field(source, destination.group_name);
        append_field(source, destination.volume_name);
        append_field(source, destination.raw_group);
        append_field(source, destination.raw_volume);
        append_integer(source, destination.create);
        for (const auto id : destination.infrastructure_sfs_ids)
            append_integer(source, id);
        append_integer(source, destination.infrastructure_clusters);
        append_integer(source, destination.root_directory_growth_bytes);
    }
    for (const auto &object : plan.objects) {
        append_field(source, object.action_id);
        append_integer(source, object.package_index);
        append_integer(source, object.root_index);
        append_field(source, object.package_id);
        append_field(source, object.node_id);
        append_field(source, object.object_type);
        append_field(source, object.source_name);
        append_field(source, object.destination_name);
        append_field(source, object.normalized_sha256);
        append_integer(source, object.partition_index);
        append_field(source, object.group_name);
        append_field(source, object.volume_name);
        append_field(source, object.raw_group);
        append_field(source, object.raw_volume);
        for (const auto action : object.actions)
            append_field(source, package_import_action_name(action));
        append_field(source, object.canonical_action_id.value_or(""));
        append_field(source, object.existing_object_key.value_or(""));
        append_integer(source, object.target_sfs_id.value_or(0U));
        append_integer(source, object.target_link_id.value_or(0U));
        for (const auto number : object.target_program_numbers)
            append_integer(source, number);
        append_integer(source, object.target_sample_bank_member);
        append_integer(source, object.payload_clusters);
        append_integer(source, object.payload_sectors);
        append_integer(source, object.continuation_clusters);
    }
    for (const auto &delta : plan.allocation) {
        append_integer(source, delta.partition_index);
        append_field(source, delta.group_name);
        append_field(source, delta.volume_name);
        append_field(source, delta.raw_group);
        append_field(source, delta.raw_volume);
        append_integer(source, delta.inserted_object_count);
        append_integer(source, delta.reused_object_count);
        append_integer(source, delta.payload_clusters);
        append_integer(source, delta.payload_sectors);
        append_integer(source, delta.continuation_clusters);
        append_integer(source, delta.directory_growth_bytes);
        append_integer(source, delta.remaining_object_ids);
        append_integer(source, delta.remaining_clusters);
        append_integer(source, delta.projected_image_sectors);
        append_integer(source, delta.projected_image_size_bytes);
    }
    for (const auto &warning : plan.warnings) {
        append_field(source, warning.code);
        append_field(source, warning.message);
        append_integer(source, warning.fatal);
    }
    for (const auto &conflict : plan.conflicts) {
        append_field(source, conflict.code);
        append_field(source, conflict.message);
        append_field(source, conflict.package_id);
        append_field(source, conflict.node_id);
        append_integer(source, conflict.package_index.value_or(0U));
        append_integer(source, conflict.root_index.value_or(0U));
        append_integer(source, conflict.partition_index.value_or(0U));
        append_field(source, conflict.group_name);
        append_field(source, conflict.volume_name);
        append_field(source, conflict.raw_group);
        append_field(source, conflict.raw_volume);
    }
    return digest_text(source);
}

} // namespace package_import_internal

Result<void> verify_package_import_plan(const PackageImportPlan &plan) {
    if (plan.schema_version != "1.0" || !valid_digest(plan.target_snapshot_id) || !valid_digest(plan.policy_digest) ||
        !valid_digest(plan.plan_id)) {
        return std::unexpected{planner_error("package import plan identity fields are invalid")};
    }
    if (std::ranges::any_of(plan.warnings, &PackageIssue::fatal))
        return std::unexpected{planner_error("package import plan contains a fatal warning")};

    std::set<std::tuple<std::uint8_t, std::string, std::string, std::string, std::string>> destination_keys;
    for (const auto &destination : plan.destinations) {
        const auto has_infrastructure = !destination.infrastructure_sfs_ids.empty() ||
                                        destination.infrastructure_clusters != 0U ||
                                        destination.root_directory_growth_bytes != 0U;
        const auto valid_creation =
            !destination.create ||
            (plan.target_kind == MediaKind::sfs && destination.infrastructure_sfs_ids.size() == 6U &&
             destination.infrastructure_clusters == 12U && destination.root_directory_growth_bytes == 32U) ||
            (plan.target_kind == MediaKind::iso9660 && !has_infrastructure);
        if (destination.volume_name.empty() ||
            !destination_keys
                 .emplace(destination.partition_index, destination.group_name, destination.volume_name,
                          destination.raw_group, destination.raw_volume)
                 .second ||
            !valid_creation || (!destination.create && has_infrastructure)) {
            return std::unexpected{planner_error("package import plan contains an invalid destination action")};
        }
    }

    std::set<std::string, std::less<>> action_ids;
    std::map<std::string, const PlannedPackageObject *, std::less<>> actions;
    for (const auto &object : plan.objects) {
        if (plan.valid() && std::ranges::find_if(plan.destinations, [&](const auto &destination) {
                                return destination.partition_index == object.partition_index &&
                                       destination.group_name == object.group_name &&
                                       destination.volume_name == object.volume_name &&
                                       destination.raw_group == object.raw_group &&
                                       destination.raw_volume == object.raw_volume;
                            }) == plan.destinations.end()) {
            return std::unexpected{planner_error("package import action has no planned destination")};
        }
        if (object.package_index >= plan.package_ids.size() ||
            object.package_id != plan.package_ids[object.package_index] || object.actions.empty() ||
            !valid_digest(object.action_id) || !valid_digest(object.normalized_sha256) ||
            !action_ids.emplace(object.action_id).second) {
            return std::unexpected{planner_error("package import plan contains an invalid action")};
        }
        const auto inserts = std::ranges::contains(object.actions, PackageImportObjectAction::insert);
        const auto reuses = std::ranges::contains(object.actions, PackageImportObjectAction::reuse);
        const auto conflicts = std::ranges::contains(object.actions, PackageImportObjectAction::conflict);
        const std::set<PackageImportObjectAction> unique_actions(object.actions.begin(), object.actions.end());
        const auto sorted_programs = std::ranges::is_sorted(object.target_program_numbers);
        const std::set<std::uint8_t> unique_programs(object.target_program_numbers.begin(),
                                                     object.target_program_numbers.end());
        if (unique_actions.size() != object.actions.size() || (inserts && reuses) ||
            (plan.valid() && !inserts && !reuses) || (plan.valid() && conflicts) ||
            (!sorted_programs || unique_programs.size() != object.target_program_numbers.size()) ||
            std::ranges::any_of(object.target_program_numbers,
                                [](const auto number) { return number < 1U || number > 128U; }) ||
            (object.object_type != "SBNK" &&
             (!object.target_program_numbers.empty() || object.target_sample_bank_member)) ||
            (plan.target_kind == MediaKind::iso9660
                 ? (object.payload_clusters != 0U || object.continuation_clusters != 0U ||
                    (inserts && !conflicts && object.payload_sectors == 0U))
                 : object.payload_sectors != 0U) ||
            (inserts && !conflicts &&
             ((plan.target_kind == MediaKind::sfs && !object.target_sfs_id) ||
              (object.object_type == "SMPL" && !object.target_link_id))) ||
            (reuses && !conflicts && !object.existing_object_key && !object.canonical_action_id)) {
            return std::unexpected{planner_error("package import plan action decision is "
                                                 "incomplete or contradictory")};
        }
        actions.emplace(object.action_id, &object);
    }
    for (const auto &object : plan.objects) {
        if (!object.canonical_action_id)
            continue;
        const auto canonical = actions.find(*object.canonical_action_id);
        if (canonical == actions.end() ||
            !std::ranges::contains(canonical->second->actions, PackageImportObjectAction::insert) ||
            object.partition_index != canonical->second->partition_index ||
            object.group_name != canonical->second->group_name ||
            object.volume_name != canonical->second->volume_name || object.raw_group != canonical->second->raw_group ||
            object.raw_volume != canonical->second->raw_volume ||
            object.object_type != canonical->second->object_type ||
            object.destination_name != canonical->second->destination_name ||
            object.normalized_sha256 != canonical->second->normalized_sha256 ||
            object.target_sfs_id != canonical->second->target_sfs_id ||
            object.target_link_id != canonical->second->target_link_id ||
            object.target_program_numbers != canonical->second->target_program_numbers ||
            object.target_sample_bank_member != canonical->second->target_sample_bank_member) {
            return std::unexpected{planner_error("package import plan canonical reuse binding is invalid")};
        }
    }
    for (const auto &allocation : plan.allocation) {
        const auto iso = plan.target_kind == MediaKind::iso9660;
        if ((iso && (allocation.payload_clusters != 0U || allocation.continuation_clusters != 0U ||
                     allocation.projected_image_sectors == 0U ||
                     allocation.projected_image_size_bytes != allocation.projected_image_sectors * 2048U)) ||
            (!iso && (allocation.payload_sectors != 0U || allocation.projected_image_sectors != 0U ||
                      allocation.projected_image_size_bytes != 0U))) {
            return std::unexpected{planner_error("package import plan allocation units do not "
                                                 "match the target media")};
        }
    }
    if (plan.plan_id != package_import_internal::plan_identity(plan))
        return std::unexpected{planner_error("package import plan identity does not match its actions")};
    return {};
}

} // namespace axk
