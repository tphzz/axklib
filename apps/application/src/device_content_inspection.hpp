#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "device_sessions_internal.hpp"

namespace axk::app::device_detail {

struct DetectedContent {
    std::string kind;
    std::string type;
    std::string label;
    std::uint64_t offset{};
    std::uint64_t size{};
    bool validated{};
};

struct ContentInspection {
    std::vector<DetectedContent> contents;
    std::vector<std::string> warnings;
    Guards guards;
    bool complete{true};
    bool crosses_target{};
    bool shared_partition_metadata{};
};
using InspectionClock = std::function<std::chrono::steady_clock::time_point()>;

// Physical offsets are carrier-relative, independently of the future SFS view.
[[nodiscard]] Result<ContentInspection> inspect_contents(const RandomAccessReader &carrier,
                                                         const DeviceTargetDescription &target,
                                                         const CancellationToken &cancellation = {},
                                                         InspectionClock now = std::chrono::steady_clock::now);
} // namespace axk::app::device_detail
