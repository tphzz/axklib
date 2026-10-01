#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"
#include "axklib/sfs.hpp"
#include "axklib/types.hpp"

namespace axk {

enum class ASeriesLoadTarget : std::uint8_t { a3000, a4000_a5000 };
enum class VolumeCapacityStatus : std::uint8_t { fits, does_not_fit };

struct VolumeCapacityReason {
    std::string code;
    std::string message;
};

struct VolumeCapacityObjectCount {
    std::string type;
    std::uint64_t count{};

    friend bool operator==(const VolumeCapacityObjectCount &, const VolumeCapacityObjectCount &) = default;
};

struct VolumeCapacityProfile {
    ASeriesLoadTarget target{ASeriesLoadTarget::a3000};
    VolumeCapacityStatus status{VolumeCapacityStatus::does_not_fit};
    std::uint64_t parameter_byte_limit{};
    std::uint64_t shared_object_slot_limit{};
    std::uint64_t baseline_bytes{};
    std::uint64_t baseline_slots{};
    std::optional<std::uint64_t> minimum_resident_bytes;
    std::optional<std::uint64_t> minimum_resident_slots;
    std::optional<std::uint64_t> resident_bytes;
    std::optional<std::uint64_t> peak_bytes;
    std::optional<std::uint64_t> peak_slots;
    std::vector<VolumeCapacityReason> reasons;
};

struct VolumeCapacityReport {
    PartitionIndex partition;
    SfsId volume_directory;
    std::string volume_name;
    std::vector<VolumeCapacityObjectCount> object_counts;
    std::vector<VolumeCapacityProfile> profiles;
};

struct VolumeCapacityPolicy {
    ASeriesLoadTarget target{ASeriesLoadTarget::a4000_a5000};
};

struct VolumeCapacityAdmission {
    ASeriesLoadTarget target{ASeriesLoadTarget::a4000_a5000};
    std::vector<VolumeCapacityReport> reports;
    bool allowed{true};
};

// A capacity failure cannot be overridden. Inspection errors are returned separately.
[[nodiscard]] AXK_API Result<void> enforce_volume_capacity_admission(const VolumeCapacityAdmission &admission,
                                                                     const VolumeCapacityPolicy &policy);

// Capacity applies to uninterrupted initialization without boot auto-load,
// completed Wipe, then full VOLUME/LOAD, not merging into arbitrary sampler RAM.
// PCM RAM and stored object format are independent.
[[nodiscard]] AXK_API Result<VolumeCapacityReport> inspect_volume_capacity(const Container &container,
                                                                           PartitionIndex partition,
                                                                           SfsId volume_directory,
                                                                           const CancellationToken &cancellation = {});

[[nodiscard]] AXK_API std::string_view a_series_load_target_name(ASeriesLoadTarget target) noexcept;
[[nodiscard]] AXK_API std::string_view volume_capacity_status_name(VolumeCapacityStatus status) noexcept;

} // namespace axk
