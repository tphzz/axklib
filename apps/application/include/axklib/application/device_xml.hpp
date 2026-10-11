#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/application/contracts.hpp"

namespace axk::app {
struct DeviceXmlTarget {
    std::string id;
    std::string label;
    std::uint64_t size_bytes{};
    std::uint32_t sector_size{};
    bool enabled{};
    bool eligible{};
    bool read_only{true};
    std::vector<std::string> notices;
};
// Pure XML inspection: no provider, host state, device enumeration or physical I/O.
[[nodiscard]] Result<std::vector<DeviceXmlTarget>> inspect_scsi2sd_xml(std::string_view xml);
} // namespace axk::app
