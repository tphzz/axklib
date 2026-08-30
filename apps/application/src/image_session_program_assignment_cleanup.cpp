#include "image_sessions_internal.hpp"

#include <charconv>
#include <format>
#include <map>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace {

using axk::app::ImageProgramAssignmentCleanupCandidate;
using axk::app::ImageProgramAssignmentCleanupInspection;
using axk::app::ImageProgramAssignmentCleanupSelection;

std::optional<std::uint8_t> program_number(const axk::app::ImageObjectItem &item) {
    const auto &value = item.entry_name.empty() ? item.name : item.entry_name;
    unsigned int number{};
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), number);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || number < 1U || number > 128U)
        return std::nullopt;
    return static_cast<std::uint8_t>(number);
}

std::string target_type(const axk::app::ImageRelationshipItem &relationship) {
    return relationship.type == "PROG_ASSIGNMENT_TO_SBAC" ? "SBAC" : "SBNK";
}

std::string unresolved_reason(const axk::app::ImageRelationshipItem &relationship) {
    if (relationship.basis.ends_with("-nonlocal"))
        return "NONLOCAL_TARGET";
    return relationship.candidate_object_ids.size() > 1U ? "AMBIGUOUS_TARGET" : "MISSING_TARGET";
}

template <typename Session>
axk::app::Result<ImageProgramAssignmentCleanupInspection>
inspect_session(const Session &session, std::uint64_t expected_revision, std::string_view content_scope_id) {
    if (session.revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if (session.format != "sfs" || !session.media) {
        return std::unexpected(
            session_error("image_mutation_unsupported", "Program assignment cleanup requires an SFS image session"));
    }
    const auto content_item = std::ranges::find(session.content, content_scope_id, &axk::app::ImageContentItem::id);
    if (content_item == session.content.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));
    if (content_item->kind != "volume" || !content_item->partition_index) {
        return std::unexpected(session_error("content_scope_invalid", "Program assignment cleanup requires a volume"));
    }
    const auto scoped = session.object_indices_by_content_scope.find(std::string{content_scope_id});
    if (scoped == session.object_indices_by_content_scope.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));

    ImageProgramAssignmentCleanupInspection result{.image_id = session.image_id,
                                                   .revision = expected_revision,
                                                   .content_scope_id = std::string{content_scope_id},
                                                   .total_candidate_count = 0U,
                                                   .candidates = {}};
    std::map<std::string, const axk::app::ImageObjectItem *, std::less<>> programs;
    for (const auto index : scoped->second) {
        if (index >= session.objects.size())
            return std::unexpected(
                session_error("image_session_invalid", "content scope references an invalid object"));
        const auto &object = session.objects[index];
        if (object.type == "PROG")
            programs.emplace(object.id, &object);
    }
    for (const auto &relationship : session.relationships) {
        const auto program = programs.find(relationship.source_object_id);
        if (program == programs.end() || relationship.assignment_state != "stored-assignment" ||
            relationship.target_object_id || !relationship.assignment_index ||
            (relationship.type != "PROG_ASSIGNMENT_TO_SBAC" && relationship.type != "PROG_ASSIGNMENT_TO_SBNK")) {
            continue;
        }
        const auto number = program_number(*program->second);
        const auto snapshot = session.snapshots_by_id.find(program->first);
        const auto *decoded = snapshot == session.snapshots_by_id.end()
                                  ? nullptr
                                  : std::get_if<axk::CurrentProg>(&snapshot->second.object.payload);
        if (!number || decoded == nullptr || *relationship.assignment_index >= axk::maximum_program_assignments)
            continue;
        result.candidates.push_back({.program_object_id = program->first,
                                     .program_number = *number,
                                     .program_name = decoded->program_name,
                                     .assignment_ordinal = static_cast<std::uint8_t>(*relationship.assignment_index),
                                     .assignment_name = relationship.assignment_name,
                                     .target_object_type = target_type(relationship),
                                     .receive_channel_display = relationship.receive_channel_display,
                                     .reason = unresolved_reason(relationship),
                                     .candidate_target_count = relationship.candidate_object_ids.size(),
                                     .default_selected = true});
    }
    std::ranges::sort(result.candidates, [](const auto &left, const auto &right) {
        return std::pair{left.program_number, left.assignment_ordinal} <
               std::pair{right.program_number, right.assignment_ordinal};
    });
    result.total_candidate_count = result.candidates.size();
    return result;
}

std::pair<std::string_view, std::uint8_t> selection_key(const ImageProgramAssignmentCleanupSelection &selection) {
    return {selection.program_object_id, selection.assignment_ordinal};
}

} // namespace

axk::app::Result<axk::app::ImageProgramAssignmentCleanupInspection>
axk::app::ImageSessionManager::inspect_program_assignment_cleanup(std::string_view image_id, std::string_view owner_id,
                                                                  std::uint64_t expected_revision,
                                                                  std::string_view content_scope_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    return inspect_session(**session, expected_revision, content_scope_id);
}

axk::app::Result<axk::app::ImageProgramAssignmentCleanupPlan>
axk::app::ImageSessionManager::plan_program_assignment_cleanup(
    std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
    std::string_view content_scope_id, const std::vector<ImageProgramAssignmentCleanupSelection> &selections) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    auto inspection = inspect_session(**session, expected_revision, content_scope_id);
    if (!inspection)
        return std::unexpected(inspection.error());
    const auto content_item = std::ranges::find((*session)->content, content_scope_id, &ImageContentItem::id);
    if (content_item == (*session)->content.end() || !content_item->partition_index)
        return std::unexpected(session_error("content_scope_invalid", "Program assignment cleanup requires a volume"));
    if (selections.empty()) {
        return std::unexpected(
            session_error("program_assignment_cleanup_invalid", "select at least one assignment to clean"));
    }

    std::map<std::pair<std::string_view, std::uint8_t>, const ImageProgramAssignmentCleanupCandidate *> candidates;
    for (const auto &candidate : inspection->candidates)
        candidates.emplace(std::pair{std::string_view{candidate.program_object_id}, candidate.assignment_ordinal},
                           &candidate);
    std::set<std::pair<std::string, std::uint8_t>> selected;
    for (const auto &selection : selections) {
        if (!selected.emplace(selection.program_object_id, selection.assignment_ordinal).second) {
            return std::unexpected(session_error("program_assignment_cleanup_invalid",
                                                 "Program assignment cleanup selections must be unique"));
        }
        if (!candidates.contains(selection_key(selection))) {
            return std::unexpected(session_error("program_assignment_cleanup_stale",
                                                 "a selected assignment is no longer unresolved", true));
        }
    }

    axk::AlterationManifest manifest{std::string{axk::alteration_manifest_schema_version}, {}};
    std::vector<ImageProgramAssignmentCleanupSelection> ordered;
    for (const auto &candidate : inspection->candidates) {
        if (!selected.contains({candidate.program_object_id, candidate.assignment_ordinal}))
            continue;
        ordered.push_back({candidate.program_object_id, candidate.assignment_ordinal});
        auto operation = std::ranges::find_if(manifest.operations, [&](const axk::AlterationOperation &row) {
            const auto *cleanup = std::get_if<axk::ClearProgramAssignmentsOperation>(&row.data);
            return cleanup != nullptr && cleanup->program_number == candidate.program_number;
        });
        if (operation == manifest.operations.end()) {
            manifest.operations.push_back(
                {std::format("clean-program-{:03}", candidate.program_number),
                 axk::ClearProgramAssignmentsOperation{axk::PartitionIndex{*content_item->partition_index},
                                                       content_item->name,
                                                       candidate.program_number,
                                                       {candidate.assignment_ordinal}}});
        } else {
            std::get<axk::ClearProgramAssignmentsOperation>(operation->data)
                .assignment_ordinals.push_back(candidate.assignment_ordinal);
        }
    }
    return ImageProgramAssignmentCleanupPlan{std::move(*inspection), std::move(ordered), std::move(manifest)};
}
