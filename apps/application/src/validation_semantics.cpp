#include "validation_operations_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "axklib/semantic.hpp"
#include "axklib/utf8.hpp"

namespace axk::app::validation_operations_internal {

const axk::ObjectSnapshot *catalog_object(const ValidationSource &source, std::string_view key) {
    const auto found = std::ranges::find(source.catalog.objects, key, &axk::ObjectSnapshot::key);
    return found == source.catalog.objects.end() ? nullptr : &*found;
}

const axk::MediaObjectDescriptor *media_object(const ValidationSource &source, std::string_view key) {
    const auto found = std::ranges::find(source.objects, key, &axk::MediaObjectDescriptor::key);
    return found == source.objects.end() ? nullptr : &*found;
}

std::string public_object_key(const ValidationSource &source, std::string_view native_key) {
    if (source.media.kind() == axk::MediaKind::sfs)
        return std::string{native_key};
    const auto *object = media_object(source, native_key);
    if (object == nullptr)
        return std::string{native_key};
    const auto filename = axk::text::path_to_utf8(source.path.filename());
    if (source.media.kind() == axk::MediaKind::fat12_floppy)
        return std::format("{}:{}", filename, object->logical_path);
    if (source.media.kind() == axk::MediaKind::iso9660)
        return std::format("{}:iso9660:{}", filename, object->logical_path);
    return std::format("{}:standalone-object", filename);
}

axk::ReportRow media_validation_issue(const ValidationSource &source, std::string severity, std::string code,
                                      std::string message, std::string scope, std::string sampler_path,
                                      std::string object_key, std::string quality, std::string basis,
                                      std::string recommended_next_check = {}) {
    return {{"severity", std::move(severity)},
            {"code", std::move(code)},
            {"message", std::move(message)},
            {"scope", std::move(scope)},
            {"source_path", axk::text::path_to_utf8(source.path)},
            {"sampler_path", std::move(sampler_path)},
            {"object_key", std::move(object_key)},
            {"quality", std::move(quality)},
            {"basis", std::move(basis)},
            {"recommended_next_check", std::move(recommended_next_check)}};
}

std::string media_object_report_path(const ValidationSource &source, std::string_view object_key) {
    if (const auto *item = catalog_object(source, object_key); item != nullptr && item->placement) {
        const auto &placement = *item->placement;
        const auto category = [&]() -> std::string_view {
            if (placement.category_name == "SMPL")
                return "Wave Data";
            if (placement.category_name == "SBNK")
                return "Samples";
            if (placement.category_name == "SBAC")
                return "Sample Banks";
            if (placement.category_name == "SEQU")
                return "Sequences";
            if (placement.category_name == "PROG")
                return "Programs";
            return placement.category_name;
        }();
        std::string path = std::format("partition {}", placement.partition.value);
        for (const auto &component :
             {std::string_view{placement.volume_name}, category, std::string_view{placement.entry_name}}) {
            if (!component.empty())
                path += std::format("/{}", component);
        }
        return path;
    }
    const auto *object = media_object(source, object_key);
    return object == nullptr ? public_object_key(source, object_key) : object->logical_path;
}

std::string media_object_group_path(const ValidationSource &source, std::string_view object_key) {
    auto path = media_object_report_path(source, object_key);
    std::ranges::replace(path, '\\', '/');
    const auto filename_separator = path.rfind('/');
    if (filename_separator == std::string::npos)
        return path;
    const auto category_separator = path.rfind('/', filename_separator - 1U);
    if (category_separator == std::string::npos)
        return path;
    const auto category = path.substr(category_separator + 1U, filename_separator - category_separator - 1U);
    static constexpr std::array object_categories{"PROG", "SBAC", "SBNK", "SMPL", "SEQU", "PRF3"};
    if (std::ranges::find(object_categories, category) == object_categories.end())
        return path;
    return path.substr(0U, category_separator);
}

std::string active_program_assignment_label(const ValidationSource &source, const axk::Relationship &row) {
    const auto assignment_name =
        !row.assignment_name.empty() ? row.assignment_name : row.target_key.value_or("unnamed assignment");
    if (!row.assignment_index)
        return std::format("{}: {}", media_object_report_path(source, row.source_key), assignment_name);
    return std::format("{}: assignment {} {}", media_object_report_path(source, row.source_key),
                       *row.assignment_index + 1U, assignment_name);
}

std::string relationship_issue_path(const ValidationSource &source, const axk::Relationship &row) {
    if (row.type.starts_with("PROG_ASSIGNMENT_"))
        return active_program_assignment_label(source, row);
    return media_object_report_path(source, row.source_key);
}

std::pair<std::string, std::string> ambiguous_relationship_message(const axk::Relationship &row) {
    if (row.basis == "assignment-visible-off-same-volume-sbac-diagnostic" ||
        row.basis == "assignment-visible-off-same-volume-sbnk-diagnostic") {
        const auto target =
            row.basis == "assignment-visible-off-same-volume-sbac-diagnostic" ? "Sample Bank (SBAC)" : "Sample (SBNK)";
        return {std::format("Visible/off Program assignment row names a {} with one same-volume diagnostic candidate "
                            "plus other duplicate-name candidates; this is decoded Program inventory, not active "
                            "Program content loss.",
                            target),
                "Use relationships.csv candidate fields when auditing off rows; the same-volume candidate is "
                "diagnostic only and must not create an active Program child."};
    }
    if (row.assignment_state == axk::AssignmentState::visible_off)
        return {"Visible/off Program assignment row has multiple possible local targets; this is decoded Program "
                "inventory, not active Program content loss.",
                "Use relationships.csv candidate fields only when auditing off rows; do not treat this warning as a "
                "missing active Program child."};
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
    if (row.basis.starts_with("sbnk-program-link-bitmap-")) {
        std::string message;
        if (row.basis.find("disambiguates-ambiguous-direct-assignment") != std::string::npos)
            message =
                "Sample (SBNK) Program-link bitmap points to one Program from an ambiguous direct-assignment set.";
        else if (row.basis.find("known-direct-assignment-missing-bitmap") != std::string::npos)
            message = "Known direct Program assignment is missing from the Sample (SBNK) Program-link bitmap.";
        else if (row.basis.find("nondefault-flag-direct-assignment-without-bitmap") != std::string::npos)
            message = "Nondefault direct Program assignment is missing from the Sample (SBNK) Program-link bitmap.";
        else
            message = "Sample (SBNK) Program-link bitmap differs from resolved direct Program assignments.";
        return {std::move(message), "Use this as bitmap consistency data only; do not treat it as Program content loss "
                                    "unless another public rule proves the bitmap is authoritative."};
    }
    if (row.type == "SBNK_PROGRAM_BITMAP_TO_PROG")
        return {"Sample (SBNK) Program-link bitmap maps to multiple possible Program slots.",
                "Use this as bitmap consistency data only until the Program target is disambiguated."};
    return {"Relationship has ambiguous candidate targets.",
            "Inspect candidate set before using for authoritative placement."};
}

std::string tentative_relationship_code(const axk::Relationship &row) {
    if (row.assignment_state == axk::AssignmentState::visible_off)
        return "REL_VISIBLE_OFF_ASSIGNMENT_DIAGNOSTIC";
    if (row.basis.starts_with("sbnk-program-link-bitmap-"))
        return "REL_PROGRAM_LINK_BITMAP_DIAGNOSTIC";
    if (row.basis == "sbnk-member-cache-only-name-mismatch")
        return "REL_SBNK_MEMBER_CACHE_DIAGNOSTIC";
    return "REL_AMBIGUOUS_TARGET";
}

std::pair<std::string, std::string> missing_relationship_message(const axk::Relationship &row) {
    if (row.assignment_state == axk::AssignmentState::active)
        return {"Active Program assignment references a missing local target.",
                "Inspect the Program assignment and source object group; user-facing info may show an unresolved "
                "placeholder instead of a normal Program child."};
    if (row.assignment_state == axk::AssignmentState::visible_off) {
        const auto expected = row.type == "PROG_ASSIGNMENT_TO_SBAC" ? "Sample Bank (SBAC)" : "Sample (SBNK)";
        return {std::format("Visible/off Program assignment row names a missing local {} target; this is decoded "
                            "Program inventory, not active Program content loss.",
                            expected),
                "Keep this row as diagnostic/off-row data unless sampler-visible checks prove it should become an "
                "active assignment."};
    }
    if (row.assignment_state == axk::AssignmentState::source_load)
        return {"Source-load Program assignment row has no resolved local target.",
                "Keep the selector as diagnostic source data until sampler-loaded placement or another public rule "
                "proves a target."};
    if (row.type.starts_with("SBNK_") && row.type.ends_with("_TO_SMPL"))
        return {"Sample (SBNK) link does not resolve to a Wave Data (SMPL) target.",
                "Inspect the object group before treating this Sample as complete."};
    return {"Relationship target could not be resolved.",
            "Inspect the relationship row and decoded source object before treating the target as present."};
}

std::string missing_relationship_code(const axk::Relationship &row) {
    if (row.assignment_state == axk::AssignmentState::visible_off)
        return "REL_VISIBLE_OFF_ASSIGNMENT_DIAGNOSTIC";
    if (row.assignment_state == axk::AssignmentState::active)
        return "REL_ACTIVE_ASSIGNMENT_MISSING_TARGET";
    return "REL_MISSING_TARGET";
}

std::vector<axk::ReportRow> validate_media_details(const ValidationSource &source, bool include_object_checks) {
    std::vector<axk::ReportRow> issues;
    if (include_object_checks) {
        for (const auto &issue : source.media.validation_issues()) {
            issues.push_back(media_validation_issue(source, "error", issue.code, issue.message, "container",
                                                    issue.sampler_path, {}, "Confirmed", issue.basis,
                                                    issue.recommended_next_check));
        }
        for (const auto &object : source.catalog.objects) {
            const auto *descriptor = media_object(source, object.key);
            const auto required =
                static_cast<std::uint64_t>(object.object.header.header_size) + object.object.header.payload_bytes_0x1c;
            if (descriptor == nullptr || required <= descriptor->size)
                continue;
            issues.push_back(media_validation_issue(
                source, "error", "OBJECT_PAYLOAD_TRUNCATED",
                std::format("Object header requires {} bytes but payload has {} bytes.", required, descriptor->size),
                "object", {}, public_object_key(source, object.key), "Known", "validation"));
        }
    }

    std::map<std::string, std::vector<std::string>> group_members;
    for (const auto &row : source.graph.relationships) {
        if (row.type == "SBAC_SLOT_TO_SBNK" && row.target_key &&
            (row.quality == axk::RelationshipQuality::known || row.quality == axk::RelationshipQuality::likely)) {
            group_members[row.source_key].push_back(*row.target_key);
        }
    }
    std::map<std::string, std::vector<const axk::Relationship *>> reachable;
    for (const auto &row : source.graph.relationships) {
        if (!row.target_key ||
            (row.assignment_state != axk::AssignmentState::active &&
             row.assignment_state != axk::AssignmentState::source_load) ||
            (row.quality != axk::RelationshipQuality::known && row.quality != axk::RelationshipQuality::likely)) {
            continue;
        }
        if (row.type == "PROG_ASSIGNMENT_TO_SBNK") {
            reachable[*row.target_key].push_back(&row);
        } else if (row.type == "PROG_ASSIGNMENT_TO_SBAC") {
            if (const auto members = group_members.find(*row.target_key); members != group_members.end()) {
                for (const auto &member : members->second)
                    reachable[member].push_back(&row);
            }
        }
    }
    using MemberGroup = std::pair<std::string, bool>;
    std::map<MemberGroup, std::vector<const axk::Relationship *>> grouped_members;
    std::map<MemberGroup, std::set<std::string>> grouped_active_labels;
    std::set<std::string> covered_relationships;
    for (const auto &row : source.graph.relationships) {
        if ((row.type != "SBNK_LEFT_MEMBER_TO_SMPL" && row.type != "SBNK_RIGHT_MEMBER_TO_SMPL") ||
            row.quality != axk::RelationshipQuality::unknown)
            continue;
        const auto active = reachable.find(row.source_key);
        const MemberGroup group{media_object_group_path(source, row.source_key), active != reachable.end()};
        grouped_members[group].push_back(&row);
        if (active != reachable.end()) {
            for (const auto *program_row : active->second)
                grouped_active_labels[group].insert(active_program_assignment_label(source, *program_row));
        }
        covered_relationships.insert(row.key);
    }
    for (const auto &[group, rows] : grouped_members) {
        std::set<std::string> source_keys;
        for (const auto *row : rows)
            source_keys.insert(public_object_key(source, row->source_key));
        const auto member_count = rows.size();
        const auto bank_count = source_keys.size();
        if (group.second) {
            std::string active_summary;
            const auto &labels = grouped_active_labels[group];
            std::size_t index{};
            for (const auto &label : labels) {
                if (index == 4U)
                    break;
                if (!active_summary.empty())
                    active_summary += "; ";
                active_summary += label;
                ++index;
            }
            if (labels.size() > 4U)
                active_summary += std::format("; +{} more", labels.size() - 4U);
            issues.push_back(media_validation_issue(
                source, "error", "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING",
                std::format("{} Sample-to-Wave-Data link(s) across {} Sample(s) do not resolve to Wave Data objects "
                            "and are reachable from active Program assignments.",
                            member_count, bank_count),
                "relationship", std::format("{} | {}", group.first, active_summary), *source_keys.begin(), "Unknown",
                "SBNK member target aggregation",
                "Treat the affected Program/Sample path as incomplete until the missing Wave Data objects are found "
                "or the source is confirmed partially loadable."));
        } else {
            issues.push_back(media_validation_issue(
                source, "warning", "REL_SBNK_MEMBER_TARGET_MISSING",
                std::format("{} Sample-to-Wave-Data link(s) across {} Sample(s) do not resolve to Wave Data objects.",
                            member_count, bank_count),
                "relationship", group.first, *source_keys.begin(), "Unknown", "SBNK member target aggregation",
                "Inspect the Sample-to-Wave-Data links before treating this object set as complete."));
        }
    }
    for (const auto &row : source.graph.relationships) {
        if (covered_relationships.contains(row.key))
            continue;
        if (row.quality == axk::RelationshipQuality::tentative) {
            auto [message, next_check] = ambiguous_relationship_message(row);
            issues.push_back(media_validation_issue(
                source, "warning", tentative_relationship_code(row), std::move(message), "relationship",
                relationship_issue_path(source, row), public_object_key(source, row.source_key), "Tentative", row.basis,
                std::move(next_check)));
        } else if (row.quality == axk::RelationshipQuality::unknown) {
            auto [message, next_check] = missing_relationship_message(row);
            issues.push_back(media_validation_issue(
                source, "warning", missing_relationship_code(row), std::move(message), "relationship",
                relationship_issue_path(source, row), public_object_key(source, row.source_key), "Unknown", row.basis,
                std::move(next_check)));
        }
    }
    std::ranges::sort(issues, {}, [](const axk::ReportRow &row) {
        const auto value = [&](std::string_view key) -> std::string {
            const auto found = std::ranges::find(row, key, &axk::ReportRow::value_type::first);
            return found == row.end() ? std::string{} : std::get<std::string>(found->second.value);
        };
        return std::tuple{value("code"), value("object_key"), value("message")};
    });
    return issues;
}

} // namespace axk::app::validation_operations_internal
