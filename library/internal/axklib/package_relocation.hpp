#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "axklib/object.hpp"
#include "axklib/package.hpp"

namespace axk::package_internal {

struct RelocationProfile {
  std::vector<std::byte> normalized_payload;
  std::vector<PackageRelocation> relocations;
};

struct PackageNodeRelocationContext {
  std::string destination_name;
  std::optional<std::uint32_t> smpl_link_id;
  std::map<std::string, std::string, std::less<>> edge_target_names;
  std::map<std::string, std::uint32_t, std::less<>> edge_target_link_ids;
  std::vector<std::uint8_t> linked_program_numbers;
  bool grouped{};
};

[[nodiscard]] Result<RelocationProfile>
build_relocation_profile(const DecodedObject &object, std::span<const std::byte> raw_payload);

[[nodiscard]] Result<std::vector<std::byte>>
project_package_node_names(const PortablePackage &package, const PackageNode &node,
                           const PackageNodeRelocationContext &context);

[[nodiscard]] Result<std::vector<std::byte>>
relocate_package_node(const PortablePackage &package, const PackageNode &node,
                      const PackageNodeRelocationContext &context);

} // namespace axk::package_internal
