#include "axklib/volume_capacity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "axklib/object.hpp"
#include "axklib/volume_capacity_internal.hpp"

namespace axk {
namespace {

std::vector<VolumeCapacityReason> validate(const detail::CapacityVolume &volume) {
    auto reasons = volume.issues;
    for (const auto &object : volume.objects) {
        if (object.type == ObjectType::unknown || object.type == ObjectType::prf3)
            reasons.push_back(
                {"UNSUPPORTED_OBJECT_LAYOUT", "An object uses a layout not covered by capacity analysis."});
        if (object.type == ObjectType::sbnk && object.references.size() > 2U)
            reasons.push_back({"INCOMPLETE_SAMPLE", "A Sample does not have complete Wave Data links."});
        if ((object.type == ObjectType::sbac || object.type == ObjectType::prog) &&
            (object.counted_rows > object.physical_rows ||
             object.inactive_row_handles.size() > object.empty_counted_rows ||
             object.references.size() + object.empty_counted_rows != object.counted_rows))
            reasons.push_back({"INCOMPLETE_ASSIGNMENTS", "Counted member or assignment rows could not be resolved."});
    }
    for (const auto &object : volume.objects) {
        for (const auto &reference : object.references) {
            const bool valid = (object.type == ObjectType::sbnk && reference.type == ObjectType::smpl) ||
                               (object.type == ObjectType::sbac && reference.type == ObjectType::sbnk) ||
                               object.type == ObjectType::prog;
            if (!valid)
                reasons.push_back(
                    {"UNSUPPORTED_REFERENCE", "An object has a reference type not covered by capacity analysis."});
        }
    }
    std::ranges::sort(reasons, {}, &VolumeCapacityReason::code);
    const auto unique = std::ranges::unique(reasons, {}, &VolumeCapacityReason::code);
    reasons.erase(unique.begin(), unique.end());
    return reasons;
}

VolumeCapacityProfile profile(ASeriesLoadTarget target) {
    VolumeCapacityProfile result;
    result.target = target;
    const bool a3 = target == ASeriesLoadTarget::a3000;
    result.parameter_byte_limit = a3 ? 524288U : 786432U;
    result.shared_object_slot_limit = a3 ? 1024U : 2048U;
    result.baseline_bytes = a3 ? 87720U : 111280U;
    result.baseline_slots = a3 ? 129U : 130U;
    return result;
}

std::uint64_t body_bytes(const detail::CapacityObject &object, ASeriesLoadTarget target) {
    return target == ASeriesLoadTarget::a3000 ? object.older_body_bytes : object.encoded_body_bytes;
}

std::pair<std::uint64_t, std::uint64_t> sequence_minimum(const detail::CapacityVolume &volume,
                                                         ASeriesLoadTarget target) {
    std::map<std::array<std::byte, 16>, std::uint64_t> loaded;
    for (const auto &object : volume.objects) {
        if (object.type != ObjectType::sequ || loaded.contains(detail::capacity_filename(object)))
            continue;
        const auto bytes = body_bytes(object, target) - 8U;
        loaded.insert_or_assign(detail::capacity_registered_name(object), (bytes + 3U) & ~std::uint64_t{3U});
    }
    std::uint64_t total{};
    for (const auto &[name, bytes] : loaded) {
        (void)name;
        total += bytes;
    }
    return {total, loaded.size()};
}

} // namespace

Result<VolumeCapacityReport> detail::analyze_volume_capacity(const CapacityVolume &volume) {
    VolumeCapacityReport result{volume.partition, volume.volume_directory, volume.name, {}, {}};
    const auto issues = validate(volume);
    if (!issues.empty())
        return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                          "Cannot inspect sampler capacity: " + issues.front().message)};
    std::map<ObjectType, std::uint64_t> counts;
    for (const auto &object : volume.objects) {
        ++counts[object.type];
    }
    for (const auto type : {ObjectType::sbnk, ObjectType::sbac, ObjectType::prog, ObjectType::smpl, ObjectType::sequ})
        result.object_counts.push_back({std::string{capacity_type_name(type)}, counts[type]});
    for (const auto target : {ASeriesLoadTarget::a3000, ASeriesLoadTarget::a4000_a5000}) {
        auto row = profile(target);
        for (const auto &object : volume.objects) {
            if (object.type != ObjectType::sequ)
                continue;
            const auto bytes = body_bytes(object, target);
            if (bytes < 0x50U || bytes > object.physical_body_bytes)
                return std::unexpected{make_error(ErrorCode::object_malformed, ErrorCategory::object,
                                                  "Cannot inspect " + std::string{a_series_load_target_name(target)} +
                                                      " capacity: a Sequence body is truncated or too short")};
        }
        const auto [bytes, slots] = sequence_minimum(volume, target);
        row.minimum_resident_bytes = bytes;
        row.minimum_resident_slots = slots;
        if (const auto replay = detail::replay_native_capacity(volume, row); !replay)
            return std::unexpected{replay.error()};
        result.profiles.push_back(std::move(row));
    }
    return result;
}

std::string_view a_series_load_target_name(ASeriesLoadTarget target) noexcept {
    return target == ASeriesLoadTarget::a3000 ? "A3000" : "A4000_A5000";
}

Result<void> enforce_volume_capacity_admission(const VolumeCapacityAdmission &admission,
                                               const VolumeCapacityPolicy &policy) {
    if (admission.target != policy.target)
        return std::unexpected{make_error(ErrorCode::transaction_stale, ErrorCategory::transaction,
                                          "Sampler capacity review changed; review the import again")};
    if (!admission.allowed) {
        std::string message =
            "Import refused: a destination volume cannot be safely loaded by the selected A-Series generation";
        for (const auto &report : admission.reports) {
            const auto profile = std::ranges::find(report.profiles, policy.target, &VolumeCapacityProfile::target);
            if (profile != report.profiles.end() && profile->status != VolumeCapacityStatus::fits) {
                message += ": " + report.volume_name;
                if (!profile->reasons.empty())
                    message += " - " + profile->reasons.front().message;
                break;
            }
        }
        return std::unexpected{
            make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message))};
    }
    return {};
}

std::string_view volume_capacity_status_name(VolumeCapacityStatus status) noexcept {
    switch (status) {
    case VolumeCapacityStatus::fits:
        return "FITS";
    case VolumeCapacityStatus::does_not_fit:
        return "DOES_NOT_FIT";
    }
    return "DOES_NOT_FIT";
}

} // namespace axk
