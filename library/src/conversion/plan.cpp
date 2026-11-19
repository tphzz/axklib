#include "axklib/conversion/plan.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <set>
#include <utility>

namespace axk::conversion {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}

bool unique_tokens(const std::vector<std::string> &values) {
    std::set<std::string_view> seen;
    return std::ranges::all_of(values,
                               [&](const std::string &value) { return !value.empty() && seen.insert(value).second; });
}

bool valid_route(const std::optional<RouteDescriptor> &route) {
    return !route || (!route->medium.empty() && !route->target_label.empty() && !route->intents.empty() &&
                      unique_tokens(route->intents) && unique_tokens(route->editor_capabilities));
}

Profile compose(const WaveReaderAdapter &reader, const WaveWriterAdapter &writer) {
    const auto id = reader.source_format + "-to-" + writer.route_key;
    auto route = writer.route;
    if (route) {
        auto capabilities = reader.editor_capabilities;
        for (const auto &panel : route->editor_capabilities)
            if (std::ranges::find(capabilities, panel) == capabilities.end())
                capabilities.push_back(panel);
        route->editor_capabilities = std::move(capabilities);
    }
    return {id,
            reader.name + " to " + writer.name,
            reader.source_format,
            writer.destination_format,
            reader.selection_schema,
            [select = reader.select, encode = writer.encode,
             id](const Request &request, const CancellationToken &cancel,
                 ProgressSink *progress) -> Result<std::shared_ptr<const PreparedPlan>> {
                auto waves = select(request, cancel);
                if (!waves)
                    return std::unexpected(waves.error());
                if (waves->empty() || waves->size() > request.limits.maximum_actions)
                    return std::unexpected(invalid("Wave reader returned an empty or oversized selection"));
                if (auto active = cancel.check(); !active)
                    return std::unexpected(active.error());
                return encode(id, *waves, request, cancel, progress);
            },
            writer.output_kind,
            std::move(route)};
}
} // namespace

Result<std::vector<std::int16_t>> WaveTransfer::read(std::uint64_t first, std::size_t count,
                                                     const CancellationToken &cancel) const {
    if (!read_pcm || first > frame_count || count > frame_count - first ||
        count > native::SourceSet::maximum_read_bytes / sizeof(std::int16_t))
        return std::unexpected(invalid("Requested frames exceed the selected playback window or read limit"));
    if (auto active = cancel.check(); !active)
        return std::unexpected(active.error());
    auto frames = read_pcm(first, count, cancel);
    if (frames && frames->size() != count)
        return std::unexpected(invalid("Wave reader returned the wrong number of frames"));
    return frames;
}

void to_json(nlohmann::json &json, const SourceObject &object) {
    json = {{"id", object.id},
            {"name", object.name},
            {"kind", object.kind},
            {"location", object.location},
            {"selectable", object.selectable},
            {"dependencies", object.dependencies}};
}

std::string_view output_kind_name(OutputKind kind) noexcept {
    switch (kind) {
    case OutputKind::existing_image:
        return "EXISTING_IMAGE";
    case OutputKind::new_image:
        return "NEW_IMAGE";
    case OutputKind::new_image_set:
        return "NEW_IMAGE_SET";
    }
    return {};
}

void to_json(nlohmann::json &json, const NewImageSetReview &review) {
    auto members = nlohmann::json::array();
    for (const auto &member : review.members)
        members.push_back({{"filename", member.filename}, {"sizeBytes", member.size_bytes}, {"sha256", member.sha256}});
    json = {{"members", std::move(members)}};
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
    if (!valid_route(profile.route))
        return std::unexpected(
            invalid("Interactive conversion requires a target label, medium and distinct intents and panels"));
    if (profile.id.empty() || profile.source_format.empty() || profile.destination_format.empty() || !profile.prepare)
        return std::unexpected(invalid("Conversion profile requires explicit identity, formats and preparation"));
    if (std::ranges::find(profiles_, profile.id, &Profile::id) != profiles_.end())
        return std::unexpected(invalid("Conversion profile identity is already registered"));
    profiles_.push_back(std::move(profile));
    return {};
}

Result<void> Registry::add(WaveReaderAdapter reader) {
    if (!unique_tokens(reader.editor_capabilities))
        return std::unexpected(invalid("Wave reader requires distinct nonempty panel identifiers"));
    if (reader.source_format.empty() || reader.name.empty() || !reader.select || !reader.selection_schema.is_object() ||
        std::ranges::find(wave_readers_, reader.source_format, &WaveReaderAdapter::source_format) !=
            wave_readers_.end())
        return std::unexpected(invalid("Wave reader requires a unique source format, name, schema and selection"));
    std::vector<Profile> added;
    for (const auto &writer : wave_writers_) {
        auto profile = compose(reader, writer);
        if (std::ranges::find(profiles_, profile.id, &Profile::id) != profiles_.end())
            return std::unexpected(invalid("Composed conversion would duplicate a profile identity"));
        added.push_back(std::move(profile));
    }
    wave_readers_.push_back(std::move(reader));
    std::ranges::move(added, std::back_inserter(profiles_));
    return {};
}

Result<void> Registry::add(WaveWriterAdapter writer) {
    if (!valid_route(writer.route))
        return std::unexpected(invalid("Wave writer declares an incomplete or ambiguous interactive route"));
    if (writer.route_key.empty() || writer.destination_format.empty() || writer.name.empty() || !writer.encode ||
        std::ranges::find(wave_writers_, writer.route_key, &WaveWriterAdapter::route_key) != wave_writers_.end())
        return std::unexpected(
            invalid("Wave writer requires a unique route key, destination format, name and encoder"));
    std::vector<Profile> added;
    for (const auto &reader : wave_readers_) {
        auto profile = compose(reader, writer);
        if (std::ranges::find(profiles_, profile.id, &Profile::id) != profiles_.end())
            return std::unexpected(invalid("Composed conversion would duplicate a profile identity"));
        added.push_back(std::move(profile));
    }
    wave_writers_.push_back(std::move(writer));
    std::ranges::move(added, std::back_inserter(profiles_));
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
    const bool new_image = profile->output_kind != OutputKind::existing_image;
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
        if (image->extension.empty() || image->extension.size() > 8U ||
            !std::ranges::all_of(image->extension,
                                 [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'); }))
            return std::unexpected(invalid("New image requires a portable filename extension"));
        if (profile->output_kind != OutputKind::new_image || image->bytes.capacity() > plan.retained_bytes() ||
            plan.report().executable == image->bytes.empty())
            return std::unexpected(invalid("Prepared conversion image retention is invalid"));
    } else if (const auto *set = std::get_if<NewImageSet>(&plan.output())) {
        if (profile->output_kind != OutputKind::new_image_set || set->members.size() > NewImageSet::maximum_members ||
            plan.report().executable == set->members.empty())
            return std::unexpected(invalid("Prepared conversion image set is invalid"));
        if (auto checked = set->validate(plan.retained_bytes()); !checked)
            return std::unexpected(checked.error());
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
