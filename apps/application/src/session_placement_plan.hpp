#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "axklib/application/contracts.hpp"
#include "axklib/application/image_sessions.hpp"
#include "axklib/types.hpp"

namespace axk::app::detail {

struct PlacementRepairScope {
    std::uint8_t partition_index{};
    std::optional<std::string> volume_name;
};

struct PlacementRepairDestination {
    std::string volume_name;
    bool creates_volume{};
    std::vector<SfsId> object_sfs_ids;
    std::map<std::string, std::size_t> object_type_counts;
};

struct PlacementRepairBlocker {
    std::string code;
    std::string message;
    std::size_t object_count{};
};

struct PlacementRepairPlan {
    PlacementRepairScope scope;
    std::vector<PlacementRepairDestination> destinations;
    std::vector<PlacementRepairBlocker> blockers;
    std::size_t blocked_object_count{};

    [[nodiscard]] std::size_t repair_object_count() const noexcept;
    [[nodiscard]] bool can_repair() const noexcept { return repair_object_count() != 0U; }
};

[[nodiscard]] Result<PlacementRepairPlan>
plan_placement_repair(const ImageSessionRead &session, PlacementRepairScope scope,
                      std::optional<std::string> recovery_volume_name = std::nullopt);

} // namespace axk::app::detail
