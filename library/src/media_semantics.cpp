#include "axklib/semantic.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/media.hpp"
#include "semantic_support.hpp"

namespace axk {
namespace {

class ValidationObjects {
  public:
    ValidationObjects(const ObjectCatalog &catalog, std::span<const MediaObjectDescriptor> descriptors) {
        for (const auto &object : catalog.objects) {
            objects_.emplace(object.key, &object);
            identities_.emplace(std::pair{object.partition.value, object.sfs_id.value}, &object);
        }
        for (const auto &descriptor : descriptors)
            descriptors_.emplace(descriptor.key, &descriptor);
    }

    const ObjectSnapshot *find(std::string_view key) const {
        const auto found = objects_.find(key);
        return found == objects_.end() ? nullptr : found->second;
    }

    const ObjectSnapshot *find(const CatalogIssue &issue) const {
        if (!issue.sfs_id)
            return nullptr;
        const auto found = identities_.find({issue.partition.value, issue.sfs_id->value});
        return found == identities_.end() ? nullptr : found->second;
    }

    const MediaObjectDescriptor *descriptor(std::string_view key) const {
        const auto found = descriptors_.find(key);
        return found == descriptors_.end() ? nullptr : found->second;
    }

    std::string volume_path(std::string_view key) const {
        const auto *object = find(key);
        if (object != nullptr && object->placement)
            return std::format("partition {}/{}", object->placement->partition.value, object->placement->volume_name);
        return std::string{key};
    }

    std::string path(std::string_view key) const {
        const auto *object = find(key);
        if (object != nullptr && object->placement) {
            const auto &placement = *object->placement;
            const auto category = [&]() -> std::string_view {
                if (placement.category_name == "SMPL")
                    return "Wave Data";
                if (placement.category_name == "SBNK")
                    return "Samples";
                if (placement.category_name == "SBAC")
                    return "Sample Banks";
                if (placement.category_name == "PROG")
                    return "Programs";
                if (placement.category_name == "SEQU")
                    return "Sequences";
                return placement.category_name;
            }();
            return std::format("{}/{}/{}", volume_path(key), category, placement.entry_name);
        }
        const auto *physical = descriptor(key);
        return physical == nullptr ? std::string{key} : physical->logical_path;
    }

    std::string scope(std::string_view key) const {
        const auto *object = find(key);
        if (object != nullptr) {
            // Relationship scopes span a partition/disc; findings remain native-volume-local.
            if (object->placement)
                return std::format("{}:p{}:volume{}", object->scope_key, object->placement->partition.value,
                                   object->placement->volume_directory.value);
            if (!object->scope_key.empty())
                return object->scope_key;
        }
        const auto *physical = descriptor(key);
        return physical != nullptr && !physical->scope_key.empty() ? physical->scope_key : std::string{key};
    }

    std::string assignment_label(const Relationship &row) const {
        const auto name =
            !row.assignment_name.empty() ? row.assignment_name : row.target_key.value_or("unnamed assignment");
        if (!row.assignment_index)
            return std::format("{}: {}", path(row.source_key), name);
        return std::format("{}: assignment {} {}", path(row.source_key), *row.assignment_index + 1U, name);
    }

  private:
    std::map<std::string_view, const ObjectSnapshot *, std::less<>> objects_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, const ObjectSnapshot *> identities_;
    std::map<std::string_view, const MediaObjectDescriptor *, std::less<>> descriptors_;
};

std::pair<std::string, std::string> tentative_message(const Relationship &row) {
    if (row.type == "PROG_ASSIGNMENT_TO_SBAC")
        return {"Program assignment to a Sample Bank (SBAC) has multiple possible targets.",
                "Verify the sampler-visible Program assignment and Sample Bank target before promotion."};
    if (row.type == "PROG_ASSIGNMENT_TO_SBNK")
        return {"Direct Program assignment has multiple possible Sample (SBNK) targets.",
                "Verify the sampler-visible Program assignment target before promotion."};
    if (row.type == "SBAC_SLOT_TO_SBNK")
        return {"Sample Bank (SBAC) slot has multiple possible Sample (SBNK) targets.",
                "Inspect duplicate same-name Sample candidates before using this slot as authoritative."};
    if (row.basis == "sbnk-member-cache-only-name-mismatch")
        return {"Sample (SBNK) cached reference metadata matches Wave Data (SMPL), but the authoritative "
                "member name does not.",
                "Treat the cached value as diagnostic only; resolve or repair the member by its local name."};
    if (row.type.starts_with("SBNK_") && row.type.ends_with("_TO_SMPL"))
        return {"Sample (SBNK) link has multiple possible Wave Data (SMPL) targets.",
                "Inspect candidate Wave Data objects before treating this Sample link as exact."};
    return {"Relationship has ambiguous candidate targets.",
            "Inspect candidate set before using for authoritative placement."};
}

std::pair<std::string, std::string> missing_message(const Relationship &row) {
    if (row.assignment_state == AssignmentState::stored_assignment)
        return {"Stored Program assignment row references a missing exact local target and is not an effective "
                "assignment.",
                "Preserve the unresolved row data and do not redirect it to a similar name. A volume package "
                "may retain it without a dependency edge."};
    if (row.assignment_state == AssignmentState::source_load_assignment)
        return {"Source-load Program assignment row has no resolved local target.",
                "Keep the selector as diagnostic source data until sampler-loaded placement or another public rule "
                "proves a target."};
    if (row.type.starts_with("SBNK_") && row.type.ends_with("_TO_SMPL"))
        return {"Sample (SBNK) link does not resolve to a Wave Data (SMPL) target.",
                "Inspect the object group before treating this Sample as complete."};
    return {"Relationship target could not be resolved.",
            "Inspect the relationship row and decoded source object before treating the target as present."};
}

void append_relationship_issues(ValidationReport &report, const ObjectCatalog &catalog, const RelationshipGraph &graph,
                                const ValidationObjects &objects) {
    std::map<std::string, std::vector<std::string>> bank_members;
    for (const auto &row : graph.relationships) {
        if (row.type == "SBAC_SLOT_TO_SBNK" && row.target_key &&
            (row.quality == RelationshipQuality::known || row.quality == RelationshipQuality::likely))
            bank_members[row.source_key].push_back(*row.target_key);
    }
    std::map<std::string, std::vector<const Relationship *>> reachable;
    for (const auto &row : graph.relationships) {
        if (!is_effective_program_assignment(row))
            continue;
        if (row.type == "PROG_ASSIGNMENT_TO_SBNK") {
            reachable[*row.target_key].push_back(&row);
        } else if (row.type == "PROG_ASSIGNMENT_TO_SBAC") {
            if (const auto found = bank_members.find(*row.target_key); found != bank_members.end()) {
                for (const auto &member : found->second)
                    reachable[member].push_back(&row);
            }
        }
    }
    using MemberGroup = std::pair<std::string, bool>;
    std::map<MemberGroup, std::vector<const Relationship *>> missing_members;
    std::map<MemberGroup, std::set<std::string>> active_labels;
    for (const auto &row : graph.relationships) {
        if ((row.type == "SBNK_LEFT_MEMBER_TO_SMPL" || row.type == "SBNK_RIGHT_MEMBER_TO_SMPL") &&
            row.quality == RelationshipQuality::unknown) {
            const auto active = reachable.find(row.source_key);
            const MemberGroup group{objects.scope(row.source_key), active != reachable.end()};
            missing_members[group].push_back(&row);
            if (active != reachable.end()) {
                for (const auto *assignment : active->second)
                    active_labels[group].insert(objects.assignment_label(*assignment));
            }
            continue;
        }
        if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG" || row.type == "SBAC_PROGRAM_BITMAP_TO_PROG")
            continue;
        const auto path =
            row.type.starts_with("PROG_ASSIGNMENT_") ? objects.assignment_label(row) : objects.path(row.source_key);
        if (row.quality == RelationshipQuality::tentative) {
            auto [message, next] = tentative_message(row);
            report.issues.push_back({row.basis == "sbnk-member-cache-only-name-mismatch"
                                         ? "REL_SBNK_MEMBER_CACHE_DIAGNOSTIC"
                                         : "REL_AMBIGUOUS_TARGET",
                                     ValidationSeverity::warning, std::move(message), path, row.source_key,
                                     "relationship", "Tentative", row.basis, std::move(next)});
        } else if (row.quality == RelationshipQuality::unknown) {
            auto [message, next] = missing_message(row);
            report.issues.push_back({row.assignment_state == AssignmentState::stored_assignment
                                         ? "REL_PROGRAM_STORED_ROW_TARGET_MISSING"
                                         : "REL_MISSING_TARGET",
                                     ValidationSeverity::warning, std::move(message), path, row.source_key,
                                     "relationship", "Unknown", row.basis, std::move(next)});
        }
    }
    for (const auto &[group, rows] : missing_members) {
        std::set<std::string> samples;
        for (const auto *row : rows)
            samples.insert(row->source_key);
        std::string path = objects.volume_path(*samples.begin());
        std::string message =
            std::format("{} Sample-to-Wave-Data link(s) across {} Sample(s) do not resolve to Wave Data objects",
                        rows.size(), samples.size());
        if (group.second) {
            message += " and are reachable from active Program assignments.";
            path += " | ";
            std::size_t index{};
            for (const auto &label : active_labels[group]) {
                if (index == 4U)
                    break;
                if (index != 0U)
                    path += "; ";
                path += label;
                ++index;
            }
            if (active_labels[group].size() > 4U)
                path += std::format("; +{} more", active_labels[group].size() - 4U);
        } else {
            message += '.';
        }
        report.issues.push_back(
            {group.second ? "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING" : "REL_SBNK_MEMBER_TARGET_MISSING",
             group.second ? ValidationSeverity::error : ValidationSeverity::warning, std::move(message),
             std::move(path), *samples.begin(), "relationship", "Unknown", "SBNK member target aggregation",
             group.second
                 ? "Treat the affected Program/Sample path as incomplete until the missing Wave Data objects are found "
                   "or the source is confirmed partially loadable."
                 : "Inspect the Sample-to-Wave-Data links before treating this object set as complete."});
    }
    for (auto &issue : validate_program_bitmaps(catalog, graph)) {
        issue.sampler_path = objects.path(issue.object_key);
        issue.basis = "program-link bitmap cross-check";
        issue.recommended_next_check = "Compare the stored Program links with the resolved assignments in this volume. "
                                       "Affected mutations are blocked; checking integrity does not change the image.";
        report.issues.push_back(std::move(issue));
    }
}

void complete_validation(ValidationReport &report, const ObjectCatalog &catalog, const RelationshipGraph &graph,
                         const ValidationObjects &objects) {
    report.coverage = {};
    report.coverage.object_count = catalog.objects.size();
    report.coverage.relationship_count = graph.relationships.size();
    for (const auto &object : catalog.objects) {
        if (object.placement)
            ++report.coverage.exact_placement_count;
        else
            ++report.coverage.unresolved_placement_count;
    }
    for (const auto &row : graph.relationships) {
        switch (row.quality) {
        case RelationshipQuality::known:
            ++report.coverage.known_relationship_count;
            break;
        case RelationshipQuality::likely:
            ++report.coverage.likely_relationship_count;
            break;
        case RelationshipQuality::tentative:
            ++report.coverage.tentative_relationship_count;
            break;
        case RelationshipQuality::unknown:
            ++report.coverage.unknown_relationship_count;
            break;
        }
    }
    append_relationship_issues(report, catalog, graph, objects);
    std::ranges::sort(report.issues, {}, [](const ValidationIssue &issue) {
        return std::tie(issue.code, issue.object_key, issue.message);
    });
}

} // namespace

ValidationReport validate_semantics(const Container &container, const ObjectCatalog &catalog,
                                    const RelationshipGraph &graph) {
    auto report = semantic_detail::validate_sfs_structure(container, catalog);
    complete_validation(report, catalog, graph, ValidationObjects{catalog, {}});
    return report;
}

ValidationReport validate_semantics(const MediaContainer &container, const MediaInventory &inventory,
                                    const RelationshipGraph &graph) {
    ValidationReport report;
    const ValidationObjects objects{inventory.catalog, inventory.objects};
    if (const auto *sfs = std::get_if<Container>(&container.storage())) {
        report = semantic_detail::validate_sfs_structure(*sfs, inventory.catalog);
    } else {
        for (const auto &issue : container.validation_issues()) {
            report.issues.push_back({issue.code,
                                     issue.severity,
                                     issue.message,
                                     issue.sampler_path,
                                     {},
                                     "container",
                                     "Confirmed",
                                     issue.basis,
                                     issue.recommended_next_check});
        }
        for (const auto &issue : inventory.catalog.issues) {
            const auto *object = objects.find(issue);
            report.issues.push_back(
                {issue.code, ValidationSeverity::error, issue.message,
                 object == nullptr ? std::format("partition {}", issue.partition.value) : objects.path(object->key),
                 object == nullptr ? std::string{} : object->key, "object"});
        }
    }
    for (const auto &object : inventory.catalog.objects) {
        const auto *physical = objects.descriptor(object.key);
        const auto required =
            static_cast<std::uint64_t>(object.object.header.header_size) + object.object.header.payload_bytes_0x1c;
        if (physical != nullptr && required > physical->size) {
            report.issues.push_back(
                {"OBJECT_PAYLOAD_TRUNCATED", ValidationSeverity::error,
                 std::format("Object header requires {} bytes but payload has {} bytes.", required, physical->size),
                 objects.path(object.key), object.key, "object"});
        }
    }
    complete_validation(report, inventory.catalog, graph, objects);
    return report;
}

} // namespace axk
