#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace axk {

struct SystemDiskParameters {
    std::optional<std::uint8_t> scsi_id;
    // Indexed by SCSI ID; the sampler's own ID cannot be mounted.
    std::array<std::optional<bool>, 8> scsi_mounts;
    // Master, slave. Absent on A3000.
    std::array<std::optional<bool>, 2> ide_mounts;
    // One-based. Reading can report 99, but writing requires 1..98 because
    // System Load resets the stored representation of 99 to 1.
    std::optional<std::uint8_t> top_partition;

    friend bool operator==(const SystemDiskParameters &, const SystemDiskParameters &) = default;
};

struct DecodedSystemDisk {
    SystemDiskParameters parameters;
    std::uint32_t seed_accumulator{};
    std::vector<std::byte> raw_bytes;

    friend bool operator==(const DecodedSystemDisk &, const DecodedSystemDisk &) = default;
};

} // namespace axk
