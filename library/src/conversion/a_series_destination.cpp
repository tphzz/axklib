#include "a_series_destination.hpp"

#include <utility>

namespace axk::conversion {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::invalid_argument, ErrorCategory::object, std::move(message));
}
} // namespace
Result<a_series::authoring::Destination> volume_destination(const nlohmann::json &selection) {
    try {
        if (!selection.is_object() || selection.size() != 3U)
            return std::unexpected(invalid("Expected destination partition, volume kind and name"));
        a_series::authoring::Destination destination;
        const auto &partition = selection.at("partitionIndex");
        if (!partition.is_number_integer() || partition < 0 || partition > 7)
            return std::unexpected(invalid("Destination partition index must be between 0 and 7"));
        const auto &kind = selection.at("kind");
        if (kind != "EXISTING_VOLUME" && kind != "CREATE_VOLUME")
            return std::unexpected(invalid("Select an existing or new destination volume"));
        destination = {partition.get<std::uint8_t>(), selection.at("volumeName").get<std::string>(),
                       kind == "CREATE_VOLUME"};
        if (destination.volume_name.empty())
            return std::unexpected(invalid("Conversion requires a destination volume name"));
        return destination;
    } catch (const nlohmann::json::exception &) {
        return std::unexpected(invalid("Malformed conversion destination"));
    }
}
Result<std::shared_ptr<const PreparedPlan>> finish_volume_plan(const Request &request,
                                                               const a_series::authoring::Destination &destination,
                                                               const a_series::authoring::Bundle &bundle, Report report,
                                                               const CancellationToken &cancellation,
                                                               ProgressSink *progress) {
    const auto policy = request.capacity_policy.value_or(VolumeCapacityPolicy{});
    auto authored = a_series::authoring::prepare(request.destination.reader, request.destination.label, destination,
                                                 bundle, policy, cancellation, progress);
    if (!authored)
        return std::unexpected(authored.error());
    const auto &admission = report.destination.emplace<VolumeCapacityAdmission>(std::move(authored->capacity));
    if (const auto admitted = enforce_volume_capacity_admission(admission, policy); !admitted) {
        report.executable = false;
        report.issues.push_back({"issue-" + std::to_string(report.issues.size() + 1U),
                                 "conversion.capacity",
                                 Impact::blocker,
                                 {},
                                 {},
                                 {},
                                 {},
                                 admitted.error().message});
    }
    auto retained_bytes = report_bytes(report);
    if (retained_bytes > request.limits.maximum_bytes || report.issues.size() > request.limits.maximum_issues)
        return std::unexpected(invalid("Conversion capacity report exceeds its retention limit"));
    if (!report.executable)
        return std::make_shared<const PreparedPlan>(std::move(report), std::vector<Patch>{}, authored->image_size_bytes,
                                                    retained_bytes);
    std::vector<Patch> patches;
    patches.reserve(authored->patches.size());
    const auto patch_overhead = patches.capacity() * sizeof(Patch);
    if (patch_overhead > request.limits.maximum_bytes - retained_bytes)
        return std::unexpected(invalid("Conversion patch list exceeds its retention limit"));
    retained_bytes += patch_overhead;
    for (auto &patch : authored->patches) {
        const auto bytes = patch.original.capacity() + patch.replacement.capacity();
        if (bytes > request.limits.maximum_bytes - retained_bytes)
            return std::unexpected(invalid("Conversion patches exceed their retained memory limit"));
        retained_bytes += bytes;
        patches.push_back({patch.offset, std::move(patch.original), std::move(patch.replacement)});
    }
    return std::make_shared<const PreparedPlan>(std::move(report), std::move(patches), authored->image_size_bytes,
                                                retained_bytes);
}
} // namespace axk::conversion
