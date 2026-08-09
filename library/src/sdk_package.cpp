#include "sdk_internal.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <ranges>
#include <thread>
#include <utility>

#include "axklib/media.hpp"
#include "axklib/package.hpp"
#include "axklib/utf8.hpp"

namespace axk {
using namespace sdk_internal;
namespace {
PackageRootKind internal_root_kind(package_root_kind kind) {
    switch (kind) {
    case package_root_kind::volume:
        return PackageRootKind::volume;
    case package_root_kind::program:
        return PackageRootKind::prog;
    case package_root_kind::sample_bank:
        return PackageRootKind::sbac;
    case package_root_kind::sample:
        return PackageRootKind::sbnk;
    case package_root_kind::wave_data:
        return PackageRootKind::smpl;
    }
    return PackageRootKind::volume;
}

PackageOpaqueSequenceAction internal_opaque_sequence_action(package_opaque_sequence_action action) {
    switch (action) {
    case package_opaque_sequence_action::preserve_unchanged:
        return PackageOpaqueSequenceAction::preserve_unchanged;
    case package_opaque_sequence_action::skip:
        return PackageOpaqueSequenceAction::skip;
    }
    return PackageOpaqueSequenceAction::preserve_unchanged;
}

package_opaque_sequence_action public_opaque_sequence_action(PackageOpaqueSequenceAction action) {
    switch (action) {
    case PackageOpaqueSequenceAction::preserve_unchanged:
        return package_opaque_sequence_action::preserve_unchanged;
    case PackageOpaqueSequenceAction::skip:
        return package_opaque_sequence_action::skip;
    }
    return package_opaque_sequence_action::preserve_unchanged;
}

std::string media_kind_name(MediaKind kind) {
    switch (kind) {
    case MediaKind::sfs:
        return "sfs";
    case MediaKind::fat12_floppy:
        return "fat12-floppy";
    case MediaKind::fat12_floppy_set:
        return "fat12-floppy-set";
    case MediaKind::iso9660:
        return "iso9660";
    case MediaKind::standalone_object:
        return "standalone-object";
    case MediaKind::axk_object_directory:
        return "axk-object-directory";
    case MediaKind::a3k_archive:
        return "a3k-archive";
    }
    return "unknown";
}

result<std::vector<PackageRootSelector>> internal_roots(const std::vector<package_root_selector> &roots) {
    std::vector<PackageRootSelector> result_value;
    result_value.reserve(roots.size());
    for (const auto &root : roots) {
        if (root.partition_index && *root.partition_index > std::numeric_limits<std::uint8_t>::max())
            return invalid_argument("package root partition index is out of range");
        PackageRootSelector item;
        item.kind = internal_root_kind(root.kind);
        if (root.partition_index)
            item.partition_index = static_cast<std::uint8_t>(*root.partition_index);
        item.group_name = root.group_name;
        item.volume_name = root.volume_name;
        item.object_name = root.object_name;
        item.object_key = root.object_key;
        result_value.push_back(std::move(item));
    }
    return result_value;
}

package_issue_info public_package_issue(const PackageIssue &issue) { return {issue.code, issue.message, issue.fatal}; }

package_import_warning_info public_package_warning(const PackageImportWarning &warning) {
    return {
        warning.code,          warning.message,         std::string{package_import_warning_origin_name(warning.origin)},
        warning.package_index, warning.node_id,         warning.object_type,
        warning.object_name,   warning.partition_index, warning.volume_name};
}

package_opaque_sequence_choice_info public_opaque_sequence_choice(const PackageOpaqueSequenceChoice &sequence) {
    return {sequence.package_index, sequence.node_id, sequence.name,
            sequence.action ? std::optional{public_opaque_sequence_action(*sequence.action)} : std::nullopt};
}

package_summary public_package_summary(const PortablePackage &package) {
    return {
        package.schema_version,
        package.package_id,
        std::string{package_kind_name(package.kind)},
        std::string{required_package_extension(package.kind)},
        package.source_media_kind,
        package.roots.size(),
        package.nodes.size(),
        package.relationships.size(),
        package.issues.size(),
        package.payloads_verified,
    };
}

package_action_info public_package_action(const PlannedPackageObject &object) {
    package_action_info result_value;
    result_value.action_id = object.action_id;
    result_value.package_index = object.package_index;
    result_value.root_index = object.root_index;
    result_value.package_id = object.package_id;
    result_value.node_id = object.node_id;
    result_value.object_type = object.object_type;
    result_value.source_name = object.source_name;
    result_value.destination_name = object.destination_name;
    result_value.partition_index = object.partition_index;
    result_value.group_name = object.group_name;
    result_value.volume_name = object.volume_name;
    result_value.raw_group = object.raw_group;
    result_value.raw_volume = object.raw_volume;
    result_value.actions.reserve(object.actions.size());
    for (const auto action : object.actions)
        result_value.actions.emplace_back(package_import_action_name(action));
    result_value.canonical_action_id = object.canonical_action_id;
    result_value.target_sfs_id = object.target_sfs_id;
    result_value.target_wave_data_reference_value = object.target_wave_data_reference_value;
    return result_value;
}

package_program_assignment_adjustment_info
public_program_assignment_adjustment(const PackageProgramAssignmentAdjustment &adjustment) {
    return {
        adjustment.adjustment_id,
        std::string{package_program_assignment_origin_name(adjustment.origin)},
        adjustment.package_index,
        adjustment.action_id,
        adjustment.existing_object_key,
        adjustment.program_slot,
        adjustment.program_name,
        adjustment.assignment_ordinal,
        adjustment.target_object_type,
        adjustment.target_name,
        adjustment.partition_index,
        adjustment.group_name,
        adjustment.volume_name,
        adjustment.raw_group,
        adjustment.raw_volume,
        adjustment.reason_code,
        std::string{package_program_assignment_disposition_name(adjustment.disposition)},
    };
}

package_program_slot_placement_info public_program_slot_placement(const PackageProgramSlotPlacement &placement) {
    package_program_slot_placement_info result_value;
    result_value.placement_id = placement.placement_id;
    result_value.partition_index = placement.partition_index;
    result_value.volume_name = placement.volume_name;
    result_value.mode = package_program_slot_placement_mode_name(placement.mode);
    result_value.applied = placement.applied;
    if (placement.suggested_start_slot)
        result_value.suggested_start_slot = *placement.suggested_start_slot;
    result_value.required_slot_count = placement.required_slot_count;
    result_value.available_slot_count = placement.available_slot_count;
    const auto project_ranges = [](const auto &ranges) {
        std::vector<package_program_slot_range_info> result;
        result.reserve(ranges.size());
        std::ranges::transform(ranges, std::back_inserter(result), [](const auto &range) {
            return package_program_slot_range_info{range.first, range.last};
        });
        return result;
    };
    result_value.occupied_ranges = project_ranges(placement.occupied_ranges);
    result_value.source_ranges = project_ranges(placement.source_ranges);
    result_value.destination_ranges = project_ranges(placement.destination_ranges);
    result_value.mappings.reserve(placement.mappings.size());
    std::ranges::transform(placement.mappings, std::back_inserter(result_value.mappings), [](const auto &mapping) {
        return package_program_slot_mapping_info{mapping.package_index, mapping.node_id, mapping.source_slot,
                                                 mapping.destination_slot, mapping.requires_user_action};
    });
    return result_value;
}

package_allocation_info public_package_allocation(const PackageAllocationDelta &allocation) {
    return {
        allocation.partition_index,
        allocation.group_name,
        allocation.volume_name,
        allocation.raw_group,
        allocation.raw_volume,
        allocation.inserted_object_count,
        allocation.reused_object_count,
        allocation.blocked_object_count,
        allocation.payload_clusters,
        allocation.payload_sectors,
        allocation.continuation_clusters,
        allocation.directory_growth_bytes,
        allocation.directory_growth_clusters,
        allocation.directory_continuation_clusters,
        allocation.infrastructure_clusters,
        allocation.additional_allocated_bytes,
        allocation.remaining_object_ids,
        allocation.remaining_clusters,
        allocation.projected_image_sectors,
        allocation.projected_image_size_bytes,
    };
}

} // namespace

struct portable_package::impl {
    PortablePackage package;
    std::filesystem::path path;
};

portable_package::portable_package() = default;
portable_package::~portable_package() = default;
portable_package::portable_package(portable_package &&) noexcept = default;
portable_package &portable_package::operator=(portable_package &&) noexcept = default;

result<portable_package> portable_package::open(const std::string &utf8_path, operation_context &context) {
    return protect<portable_package>([&]() -> result<portable_package> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto path = checked_path(utf8_path, "package path");
        if (!path)
            return path.error();
        auto opened = inspect_portable_package(*path, context.impl_->cancellation.token());
        if (!opened)
            return public_error(opened.error());
        portable_package result_value;
        result_value.impl_ = std::make_unique<impl>(impl{std::move(*opened), std::move(*path)});
        return result_value;
    });
}

result<package_export_summary> portable_package::export_from(const std::string &utf8_source_path,
                                                             const std::vector<package_root_selector> &roots,
                                                             const std::string &utf8_output_path,
                                                             const write_options &options, operation_context &context) {
    return protect<package_export_summary>([&]() -> result<package_export_summary> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto source = checked_path(utf8_source_path, "package source path");
        if (!source)
            return source.error();
        auto output = checked_path(utf8_output_path, "package output path");
        if (!output)
            return output.error();
        auto selectors = internal_roots(roots);
        if (!selectors)
            return selectors.error();
        auto media = open_media(*source, context.impl_->cancellation.token());
        if (!media)
            return public_error(media.error());
        context.impl_->report(
            {ProgressPhase::exporting, 0U, 1U, "exporting portable package", text::path_to_utf8(*output)});
        auto published = export_portable_package(*media, *selectors, *output, options.overwrite,
                                                 context.impl_->cancellation.token());
        if (!published)
            return public_error(published.error());
        context.impl_->report({ProgressPhase::exporting, 1U, 1U, "exported portable package",
                               text::path_to_utf8(published->output_path)});
        return package_export_summary{
            text::path_to_utf8(published->output_path),
            published->package_id,
            std::string{package_kind_name(published->kind)},
            std::string{required_package_extension(published->kind)},
            published->size_bytes,
        };
    });
}

result<package_summary> portable_package::summary() const {
    return protect<package_summary>([&]() -> result<package_summary> {
        if (!impl_)
            return invalid_argument("portable package is not open");
        return public_package_summary(impl_->package);
    });
}

result<std::vector<package_issue_info>> portable_package::issues() const {
    return protect<std::vector<package_issue_info>>([&]() -> result<std::vector<package_issue_info>> {
        if (!impl_)
            return invalid_argument("portable package is not open");
        std::vector<package_issue_info> result_value;
        result_value.reserve(impl_->package.issues.size());
        std::ranges::transform(impl_->package.issues, std::back_inserter(result_value),
                               [](const auto &issue) { return public_package_issue(issue); });
        return result_value;
    });
}

result<void> portable_package::verify(operation_context &context) const {
    return protect<void>([&]() -> result<void> {
        if (!impl_)
            return invalid_argument("portable package is not open");
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        if (const auto checked = context.impl_->cancellation.token().check(); !checked)
            return public_error(checked.error());
        auto verified = open_portable_package(impl_->path, context.impl_->cancellation.token());
        if (!verified)
            return public_error(verified.error());
        if (verified->package_id != impl_->package.package_id)
            return invalid_argument("portable package changed after it was opened");
        return {};
    });
}

struct package_import_plan::impl {
    std::filesystem::path target_path;
    std::vector<PortablePackage> packages;
    PackageImportPlan plan;
    std::thread::id owner;
};

package_import_plan::package_import_plan() = default;
package_import_plan::~package_import_plan() = default;
package_import_plan::package_import_plan(package_import_plan &&) noexcept = default;
package_import_plan &package_import_plan::operator=(package_import_plan &&) noexcept = default;

result<package_import_plan> package_import_plan::create(const std::string &utf8_target_path,
                                                        const std::vector<std::string> &utf8_package_paths,
                                                        const package_import_request &request,
                                                        operation_context &context) {
    return protect<package_import_plan>([&]() -> result<package_import_plan> {
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        auto target = checked_path(utf8_target_path, "package import target path");
        if (!target)
            return target.error();
        if (utf8_package_paths.empty())
            return invalid_argument("at least one package path is required");

        std::vector<PortablePackage> packages;
        packages.reserve(utf8_package_paths.size());
        for (const auto &path_value : utf8_package_paths) {
            auto path = checked_path(path_value, "package path");
            if (!path)
                return path.error();
            auto package = open_portable_package(*path, context.impl_->cancellation.token());
            if (!package)
                return public_error(package.error());
            packages.push_back(std::move(*package));
        }

        PackageImportRequest internal_request;
        internal_request.root_destinations.reserve(request.root_destinations.size());
        for (const auto &destination : request.root_destinations) {
            if (destination.package_index > std::numeric_limits<std::size_t>::max() ||
                destination.root_index > std::numeric_limits<std::size_t>::max() ||
                (destination.partition_index &&
                 *destination.partition_index > std::numeric_limits<std::uint8_t>::max())) {
                return invalid_argument("package destination index is out of range");
            }
            PackageRootDestination item;
            item.package_index = static_cast<std::size_t>(destination.package_index);
            item.root_index = static_cast<std::size_t>(destination.root_index);
            if (destination.partition_index)
                item.partition_index = static_cast<std::uint8_t>(*destination.partition_index);
            item.group_name = destination.group_name;
            item.volume_name = destination.volume_name;
            item.raw_group = destination.raw_group;
            item.raw_volume = destination.raw_volume;
            item.create_destination = destination.create_destination;
            internal_request.root_destinations.push_back(std::move(item));
        }
        internal_request.policy.renames.reserve(request.renames.size());
        for (const auto &rename : request.renames) {
            if (rename.package_index > std::numeric_limits<std::size_t>::max())
                return invalid_argument("package rename index is out of range");
            internal_request.policy.renames.push_back(
                {static_cast<std::size_t>(rename.package_index), rename.node_id, rename.destination_name});
        }
        internal_request.policy.program_slot_assignments.reserve(request.program_slot_assignments.size());
        for (const auto &assignment : request.program_slot_assignments) {
            if (assignment.package_index > std::numeric_limits<std::size_t>::max())
                return invalid_argument("package Program slot assignment index is out of range");
            if (assignment.destination_slot < 1U || assignment.destination_slot > 128U)
                return invalid_argument("package Program destination slot must be between 1 and 128");
            internal_request.policy.program_slot_assignments.push_back(
                {static_cast<std::size_t>(assignment.package_index), assignment.node_id,
                 static_cast<std::uint8_t>(assignment.destination_slot)});
        }
        internal_request.policy.opaque_sequence_decisions.reserve(request.opaque_sequence_decisions.size());
        for (const auto &decision : request.opaque_sequence_decisions) {
            if (decision.package_index > std::numeric_limits<std::size_t>::max())
                return invalid_argument("package opaque Sequence decision index is out of range");
            internal_request.policy.opaque_sequence_decisions.push_back(
                {static_cast<std::size_t>(decision.package_index), decision.node_id,
                 internal_opaque_sequence_action(decision.action)});
        }

        auto planned = plan_package_import(*target, packages, internal_request, context.impl_->cancellation.token());
        if (!planned)
            return public_error(planned.error());
        package_import_plan result_value;
        result_value.impl_ = std::make_unique<impl>(impl{
            std::move(*target),
            std::move(packages),
            std::move(*planned),
            std::this_thread::get_id(),
        });
        return result_value;
    });
}

result<package_import_summary> package_import_plan::summary() const {
    return protect<package_import_summary>([&]() -> result<package_import_summary> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        return package_import_summary{
            impl_->plan.schema_version,
            impl_->plan.plan_id,
            media_kind_name(impl_->plan.target_kind),
            impl_->plan.target_snapshot_id,
            impl_->plan.package_ids.size(),
            impl_->plan.destinations.size(),
            impl_->plan.objects.size(),
            impl_->plan.program_assignment_adjustments.size(),
            impl_->plan.conflicts.size(),
            impl_->plan.warnings.size(),
            impl_->plan.valid(),
        };
    });
}

result<std::vector<package_import_warning_info>> package_import_plan::warnings() const {
    return protect<std::vector<package_import_warning_info>>([&]() -> result<std::vector<package_import_warning_info>> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        std::vector<package_import_warning_info> result_value;
        result_value.reserve(impl_->plan.warnings.size());
        std::ranges::transform(impl_->plan.warnings, std::back_inserter(result_value),
                               [](const auto &warning) { return public_package_warning(warning); });
        return result_value;
    });
}

result<std::vector<package_opaque_sequence_choice_info>> package_import_plan::opaque_sequences() const {
    return protect<std::vector<package_opaque_sequence_choice_info>>(
        [&]() -> result<std::vector<package_opaque_sequence_choice_info>> {
            if (!impl_)
                return invalid_argument("package import plan is not initialized");
            std::vector<package_opaque_sequence_choice_info> result_value;
            result_value.reserve(impl_->plan.opaque_sequences.size());
            std::ranges::transform(impl_->plan.opaque_sequences, std::back_inserter(result_value),
                                   public_opaque_sequence_choice);
            return result_value;
        });
}

result<std::vector<package_conflict_info>> package_import_plan::conflicts() const {
    return protect<std::vector<package_conflict_info>>([&]() -> result<std::vector<package_conflict_info>> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        std::vector<package_conflict_info> result_value;
        result_value.reserve(impl_->plan.conflicts.size());
        for (const auto &conflict : impl_->plan.conflicts) {
            result_value.push_back({
                conflict.code,
                conflict.message,
                conflict.package_index,
                conflict.root_index,
                conflict.package_id,
                conflict.node_id,
                conflict.partition_index,
                conflict.group_name,
                conflict.volume_name,
                conflict.raw_group,
                conflict.raw_volume,
            });
        }
        return result_value;
    });
}

result<std::vector<package_action_info>> package_import_plan::actions() const {
    return protect<std::vector<package_action_info>>([&]() -> result<std::vector<package_action_info>> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        std::vector<package_action_info> result_value;
        result_value.reserve(impl_->plan.objects.size());
        std::ranges::transform(impl_->plan.objects, std::back_inserter(result_value), public_package_action);
        return result_value;
    });
}

result<std::vector<package_program_assignment_adjustment_info>> package_import_plan::adjustments() const {
    return protect<std::vector<package_program_assignment_adjustment_info>>(
        [&]() -> result<std::vector<package_program_assignment_adjustment_info>> {
            if (!impl_)
                return invalid_argument("package import plan is not initialized");
            std::vector<package_program_assignment_adjustment_info> result_value;
            result_value.reserve(impl_->plan.program_assignment_adjustments.size());
            std::ranges::transform(impl_->plan.program_assignment_adjustments, std::back_inserter(result_value),
                                   public_program_assignment_adjustment);
            return result_value;
        });
}

result<std::vector<package_program_slot_placement_info>> package_import_plan::program_slot_placements() const {
    return protect<std::vector<package_program_slot_placement_info>>(
        [&]() -> result<std::vector<package_program_slot_placement_info>> {
            if (!impl_)
                return invalid_argument("package import plan is not initialized");
            std::vector<package_program_slot_placement_info> result_value;
            result_value.reserve(impl_->plan.program_slot_placements.size());
            std::ranges::transform(impl_->plan.program_slot_placements, std::back_inserter(result_value),
                                   public_program_slot_placement);
            return result_value;
        });
}

result<std::vector<package_allocation_info>> package_import_plan::allocation() const {
    return protect<std::vector<package_allocation_info>>([&]() -> result<std::vector<package_allocation_info>> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        std::vector<package_allocation_info> result_value;
        result_value.reserve(impl_->plan.allocation.size());
        std::ranges::transform(impl_->plan.allocation, std::back_inserter(result_value), public_package_allocation);
        return result_value;
    });
}

result<package_import_result> package_import_plan::apply(const std::string &utf8_output_path,
                                                         const write_options &options, operation_context &context) {
    return protect<package_import_result>([&]() -> result<package_import_result> {
        if (!impl_)
            return invalid_argument("package import plan is not initialized");
        if (!context.impl_)
            return invalid_argument("operation context is not initialized");
        if (impl_->owner != std::this_thread::get_id())
            return invalid_argument("package import plan used from a different thread");
        auto output = checked_path(utf8_output_path, "package import output path");
        if (!output)
            return output.error();
        auto applied =
            apply_package_import(impl_->target_path, impl_->packages, impl_->plan, *output, options.overwrite,
                                 context.impl_->cancellation.token(), context.impl_.get());
        if (!applied)
            return public_error(applied.error());
        return package_import_result{
            text::path_to_utf8(applied->output_path),
            applied->plan_id,
            applied->source_snapshot_id,
            applied->output_snapshot_id,
            applied->objects.size(),
            applied->program_assignment_adjustments.size(),
            applied->applied,
        };
    });
}

} // namespace axk
