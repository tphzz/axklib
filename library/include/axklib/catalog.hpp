#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"
#include "axklib/object.hpp"
#include "axklib/sfs.hpp"

namespace axk {

struct ObjectPlacement {
    PartitionIndex partition;
    std::string partition_name;
    SfsId volume_directory;
    std::string volume_name;
    std::string category_name;
    std::string entry_name;
    std::string container_directory;
};

enum class PlacementResolution : std::uint8_t { exact, missing, ambiguous };

AXK_API std::string_view placement_resolution_name(PlacementResolution resolution) noexcept;

struct ObjectSnapshot {
    ObjectSnapshot() = default;
    ObjectSnapshot(std::string key_value, PartitionIndex partition_value, SfsId sfs_id_value,
                   std::string scope_key_value, DecodedObject object_value,
                   std::optional<ObjectPlacement> placement_value = std::nullopt,
                   std::vector<std::byte> raw_payload_value = {},
                   std::vector<ObjectPlacement> placement_candidates_value = {},
                   PlacementResolution placement_resolution_value = PlacementResolution::missing)
        : key(std::move(key_value)), partition(partition_value), sfs_id(sfs_id_value),
          scope_key(std::move(scope_key_value)), object(std::move(object_value)), placement(std::move(placement_value)),
          raw_payload(std::move(raw_payload_value)), placement_candidates(std::move(placement_candidates_value)),
          placement_resolution(placement_resolution_value) {}

    std::string key;
    PartitionIndex partition;
    SfsId sfs_id;
    std::string scope_key;
    DecodedObject object;
    std::optional<ObjectPlacement> placement;
    std::vector<std::byte> raw_payload{};
    std::vector<ObjectPlacement> placement_candidates{};
    PlacementResolution placement_resolution{PlacementResolution::missing};
};

struct CatalogIssue {
    std::string code;
    std::string message;
    PartitionIndex partition;
    std::optional<SfsId> sfs_id;
};

struct ObjectCatalog {
    std::vector<ObjectSnapshot> objects;
    std::vector<CatalogIssue> issues;
};

AXK_API Result<ObjectCatalog> build_object_catalog(const Container &container,
                                                   std::size_t maximum_object_bytes = 64U * 1024U * 1024U,
                                                   const CancellationToken &cancellation = {});

} // namespace axk
