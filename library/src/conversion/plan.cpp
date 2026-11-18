#include "axklib/conversion/plan.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>

namespace axk::conversion {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}
} // namespace

void to_json(nlohmann::json &json, const SourceObject &object) {
    json = {{"id", object.id},
            {"name", object.name},
            {"kind", object.kind},
            {"location", object.location},
            {"selectable", object.selectable},
            {"dependencies", object.dependencies}};
}

std::uint64_t report_bytes(const Report &report) {
    std::uint64_t bytes = sizeof(PreparedPlan) + report.profile_id.capacity() +
                          report.actions.capacity() * sizeof(Action) + report.issues.capacity() * sizeof(Issue);
    for (const auto &action : report.actions) {
        bytes += action.kind.capacity() + action.name.capacity() + action.source_object.capacity() +
                 action.action.capacity() + action.details.capacity() * sizeof(Action::Detail);
        for (const auto &detail : action.details)
            bytes += detail.label.capacity() + detail.value.capacity();
    }
    for (const auto &notice : report.issues)
        bytes += notice.id.capacity() + notice.code.capacity() + notice.source_object.capacity() +
                 notice.source_parameter.capacity() + notice.target_object.capacity() +
                 notice.target_parameter.capacity() + notice.message.capacity() + notice.decision.capacity();
    bytes += report.source_objects.capacity() * sizeof(SourceObject);
    for (const auto &object : report.source_objects) {
        bytes += object.id.capacity() + object.name.capacity() + object.kind.capacity() + object.location.capacity() +
                 object.dependencies.capacity() * sizeof(std::string);
        for (const auto &id : object.dependencies)
            bytes += id.capacity();
    }
    const auto *admission = std::get_if<VolumeCapacityAdmission>(&report.destination);
    if (!admission)
        return bytes;
    bytes += admission->reports.capacity() * sizeof(VolumeCapacityReport);
    for (const auto &capacity : admission->reports) {
        bytes += capacity.volume_name.capacity() +
                 capacity.object_counts.capacity() * sizeof(VolumeCapacityObjectCount) +
                 capacity.profiles.capacity() * sizeof(VolumeCapacityProfile);
        for (const auto &object : capacity.object_counts)
            bytes += object.type.capacity();
        for (const auto &profile : capacity.profiles) {
            bytes += profile.reasons.capacity() * sizeof(VolumeCapacityReason);
            for (const auto &reason : profile.reasons)
                bytes += reason.code.capacity() + reason.message.capacity();
        }
    }
    return bytes;
}

PreparedPlan::PreparedPlan(Report report, std::vector<Patch> patches, std::uint64_t destination_size,
                           std::uint64_t retained_bytes)
    : report_(std::move(report)), output_(ExistingImagePatches{std::move(patches), destination_size}),
      retained_bytes_(retained_bytes) {}
PreparedPlan::PreparedPlan(Report report, NewImage image, std::uint64_t retained_bytes)
    : report_(std::move(report)), output_(std::move(image)), retained_bytes_(retained_bytes) {
    report_.destination = NewImageReview{std::get<NewImage>(output_).bytes.size()};
}
const Report &PreparedPlan::report() const noexcept { return report_; }
const PreparedOutput &PreparedPlan::output() const noexcept { return output_; }
std::uint64_t PreparedPlan::retained_bytes() const noexcept { return retained_bytes_; }

Result<void> PreparedPlan::acknowledge(std::span<const std::string> issue_ids) const {
    if (!report_.executable)
        return std::unexpected(invalid("Conversion plan is blocked; correct the inputs and prepare a new plan"));
    std::set<std::string, std::less<>> supplied;
    for (const auto &id : issue_ids) {
        if (!supplied.insert(id).second)
            return std::unexpected(invalid("Duplicate conversion acknowledgement"));
        const auto issue = std::ranges::find(report_.issues, id, &Issue::id);
        if (issue == report_.issues.end() || issue->decision.empty())
            return std::unexpected(invalid("Unknown or unnecessary conversion acknowledgement"));
    }
    for (const auto &issue : report_.issues)
        if (!issue.decision.empty() && !supplied.contains(issue.id))
            return std::unexpected(invalid("Resolve the required conversion mapping decisions"));
    return {};
}

Result<void> Registry::add(Profile profile) {
    if (profile.id.empty() || profile.source_format.empty() || profile.destination_format.empty() || !profile.prepare)
        return std::unexpected(invalid("Conversion profile requires explicit identity, formats and preparation"));
    if (std::ranges::find(profiles_, profile.id, &Profile::id) != profiles_.end())
        return std::unexpected(invalid("Conversion profile identity is already registered"));
    profiles_.push_back(std::move(profile));
    return {};
}

std::span<const Profile> Registry::profiles() const noexcept { return profiles_; }

Result<std::shared_ptr<const PreparedPlan>> Registry::prepare(std::string_view profile_id, const Request &request,
                                                              const CancellationToken &cancellation,
                                                              ProgressSink *progress) const {
    const auto profile = std::ranges::find(profiles_, profile_id, &Profile::id);
    if (profile == profiles_.end())
        return std::unexpected(invalid("Unknown directional conversion profile"));
    if (!request.source || request.source->summary().format_id != profile->source_format ||
        request.destination.format_id != profile->destination_format)
        return std::unexpected(invalid("Conversion source or destination does not match the selected profile"));
    const bool new_image = profile->output_kind == OutputKind::new_image;
    if (new_image ? (request.destination.reader || !request.destination.label.empty() ||
                     !request.destination.selection.is_null() || request.capacity_policy.has_value())
                  : !request.destination.reader)
        return std::unexpected(invalid("Conversion destination inputs do not match the profile output"));
    if (request.limits.maximum_bytes == 0U || request.limits.maximum_actions == 0U ||
        request.limits.maximum_issues == 0U)
        return std::unexpected(invalid("Conversion resource limits must be positive"));
    if (!request.source->sources() || (request.fidelity != Fidelity::best && request.fidelity != Fidelity::strict))
        return std::unexpected(invalid("Conversion source set or fidelity is invalid"));
    if (const auto active = request.source->sources()->verify(cancellation); !active)
        return std::unexpected(active.error());
    auto prepared = profile->prepare(request, cancellation, progress);
    if (!prepared)
        return std::unexpected(prepared.error());
    if (!*prepared || (*prepared)->report().profile_id != profile->id ||
        (*prepared)->report().fidelity != request.fidelity ||
        (*prepared)->retained_bytes() > request.limits.maximum_bytes ||
        (*prepared)->report().actions.size() > request.limits.maximum_actions ||
        (*prepared)->report().issues.size() > request.limits.maximum_issues)
        return std::unexpected(invalid("Prepared conversion violates its profile or resource limits"));
    const auto &plan = **prepared;
    std::set<std::string, std::less<>> issue_ids;
    for (const auto &issue : plan.report().issues) {
        if (issue.impact != Impact::exact && issue.impact != Impact::representation && issue.impact != Impact::loss &&
            issue.impact != Impact::uncertainty && issue.impact != Impact::blocker)
            return std::unexpected(invalid("Conversion issue has an invalid impact"));
        if (issue.id.empty() || !issue_ids.insert(issue.id).second)
            return std::unexpected(invalid("Conversion issues require unique identities"));
        if (!issue.decision.empty() && issue.impact != Impact::uncertainty)
            return std::unexpected(invalid("Only uncertain mappings can request a decision"));
        if (plan.report().executable && (issue.impact == Impact::blocker ||
                                         (request.fidelity == Fidelity::strict &&
                                          (issue.impact == Impact::loss || issue.impact == Impact::uncertainty))))
            return std::unexpected(invalid("Executable conversion violates its fidelity policy"));
    }
    if (const auto *image = std::get_if<NewImage>(&plan.output())) {
        if (!new_image || image->bytes.capacity() > plan.retained_bytes() ||
            plan.report().executable == image->bytes.empty())
            return std::unexpected(invalid("Prepared conversion image retention is invalid"));
    } else {
        const auto &edit = std::get<ExistingImagePatches>(plan.output());
        if (new_image || !std::holds_alternative<VolumeCapacityAdmission>(plan.report().destination) ||
            edit.destination_size != request.destination.reader->size())
            return std::unexpected(invalid("Prepared conversion destination does not match the requested edit"));
        std::uint64_t end{}, bytes{};
        for (const auto &patch : edit.patches) {
            if (patch.original.empty() || patch.original.size() != patch.replacement.size() || patch.offset < end ||
                patch.offset > edit.destination_size || patch.original.size() > edit.destination_size - patch.offset ||
                patch.original.size() > (std::numeric_limits<std::uint64_t>::max() - bytes) / 2U)
                return std::unexpected(invalid("Prepared conversion contains invalid or overlapping patches"));
            end = patch.offset + patch.original.size();
            bytes += patch.original.size() * 2U;
        }
        if (bytes > plan.retained_bytes() || (!plan.report().executable && !edit.patches.empty()))
            return std::unexpected(invalid("Prepared conversion patch retention is invalid"));
    }
    if (const auto active = request.source->sources()->verify(cancellation); !active)
        return std::unexpected(active.error());
    return prepared;
}

std::string_view impact_name(Impact impact) noexcept {
    switch (impact) {
    case Impact::exact:
        return "EXACT";
    case Impact::representation:
        return "REPRESENTATION";
    case Impact::loss:
        return "LOSS";
    case Impact::uncertainty:
        return "UNCERTAINTY";
    case Impact::blocker:
        return "BLOCKER";
    }
    return "BLOCKER";
}

} // namespace axk::conversion
