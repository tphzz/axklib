#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>

#include "axklib/volume_capacity.hpp"
#include <vector>

namespace axk::detail {
struct CapacityDestination {
    PartitionIndex partition;
    std::string name;
    std::optional<SfsId> directory{};
};

[[nodiscard]] Result<VolumeCapacityAdmission>
inspect_capacity_destinations(const Container &overlay, std::span<const CapacityDestination> destinations,
                              const VolumeCapacityPolicy &policy, const CancellationToken &cancellation = {});

} // namespace axk::detail
