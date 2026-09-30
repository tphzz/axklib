#pragma once

#include <cstdint>
#include <memory>

#include <nlohmann/json.hpp>

#include "axklib/application/contracts.hpp"
#include "axklib/export.hpp"
#include "axklib/io.hpp"
#include "axklib/volume_capacity.hpp"

namespace axk::app {
class OperationRegistry;
class ImageSessionManager;

[[nodiscard]] AXK_API nlohmann::json volume_capacity_json(const VolumeCapacityReport &report);
[[nodiscard]] AXK_API nlohmann::json capacity_admission_json(const VolumeCapacityAdmission &admission);
[[nodiscard]] AXK_API Result<VolumeCapacityPolicy> capacity_policy(const nlohmann::json &input);
[[nodiscard]] AXK_API Result<VolumeCapacityAdmission>
inspect_frozen_capacity(std::shared_ptr<const RandomAccessReader> frozen, const VolumeCapacityAdmission &prepared,
                        const VolumeCapacityPolicy &policy, const CancellationToken &cancellation = {});
[[nodiscard]] AXK_API Result<void> bind_volume_capacity_operations(OperationRegistry &registry,
                                                                   ImageSessionManager &images);
} // namespace axk::app
