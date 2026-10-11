#pragma once

#include "axklib/application/device_sessions.hpp"
#include "rawdisk/block_device.hpp"

#include <algorithm>
#include <expected>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace axk::app::device_detail {

inline Result<std::vector<DeviceCandidate>>
refresh_candidates(std::map<std::string, rawdisk::DeviceInfo, std::less<>> &current,
                   std::span<const rawdisk::DeviceInfo> devices,
                   const std::function<Result<std::string>()> &generate_id) {
    std::map<std::string, rawdisk::DeviceInfo, std::less<>> next;
    std::vector<DeviceCandidate> result;
    for (const auto &device : devices) {
        if (device.carrier_id.empty() || device.connection_id.empty())
            continue;
        const auto found = std::ranges::find_if(current, [&](const auto &entry) { return entry.second == device; });
        auto id = found == current.end() ? generate_id() : Result<std::string>{found->first};
        if (!id)
            return std::unexpected(id.error());
        if (id->empty() || (found == current.end() && current.contains(*id)) || !next.emplace(*id, device).second)
            return std::unexpected(Error{"device_identity_unavailable", "device candidate identity is not unique"});
        result.push_back({*id, device.model, device.serial, device.size_bytes, device.sector_size});
    }
    current = std::move(next);
    return result;
}

} // namespace axk::app::device_detail
