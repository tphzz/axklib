#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "axklib/object.hpp"
#include "axklib/volume_capacity.hpp"

namespace axk::detail {

inline std::string_view capacity_type_name(ObjectType type) {
    switch (type) {
    case ObjectType::smpl:
        return "SMPL";
    case ObjectType::sbnk:
        return "SBNK";
    case ObjectType::sbac:
        return "SBAC";
    case ObjectType::prog:
        return "PROG";
    case ObjectType::sequ:
        return "SEQU";
    case ObjectType::prf3:
        return "PRF3";
    case ObjectType::unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

struct CapacityReference {
    ObjectType type{ObjectType::unknown};
    std::array<std::byte, 16> name{};
    std::uint32_t raw_handle{};

    friend bool operator==(const CapacityReference &, const CapacityReference &) = default;
};

struct CapacityRow {
    ObjectType type{ObjectType::unknown};
    std::array<std::byte, 16> name{};
    std::uint32_t raw_handle{};
};

struct CapacityObject {
    ObjectType type{ObjectType::unknown};
    std::array<std::byte, 16> name{};
    std::optional<std::array<std::byte, 16>> filesystem_name;
    std::uint32_t selector{};
    std::uint32_t encoded_body_bytes{};
    std::uint32_t older_body_bytes{};
    std::uint64_t physical_body_bytes{std::numeric_limits<std::uint64_t>::max()};
    std::size_t counted_rows{};
    std::size_t physical_rows{};
    std::size_t empty_counted_rows{};
    std::vector<std::uint32_t> inactive_row_handles;
    bool has_left_wave{true};
    bool bank_member{};
    bool canonical_row_handles{true};
    std::vector<CapacityReference> references;
    std::vector<CapacityRow> rows;
};

inline std::array<std::byte, 16> capacity_filename(const CapacityObject &object) {
    return object.filesystem_name.value_or(object.name);
}

inline std::array<std::byte, 16> capacity_registered_name(const CapacityObject &object) {
    auto name = object.name.front() == std::byte{} ? capacity_filename(object) : object.name;
    for (auto &byte : name) {
        const auto value = std::to_integer<unsigned>(byte);
        if (value < 0x20U || value > 0x7eU || value == 0x5cU)
            byte = std::byte{'_'};
    }
    return name;
}

struct CapacityVolume {
    PartitionIndex partition;
    SfsId volume_directory;
    std::string name;
    std::vector<CapacityObject> objects;
    std::vector<VolumeCapacityReason> issues;
};

[[nodiscard]] Result<VolumeCapacityReport> analyze_volume_capacity(const CapacityVolume &volume);
[[nodiscard]] Result<void> replay_native_capacity(const CapacityVolume &volume, VolumeCapacityProfile &profile);

} // namespace axk::detail
