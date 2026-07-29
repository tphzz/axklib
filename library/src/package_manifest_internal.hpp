#pragma once

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "axklib/package.hpp"

namespace axk::package_internal {

struct ManifestArchiveEntry {
    std::uint64_t size{};
    const std::vector<std::byte> *bytes{};
};

nlohmann::json manifest_json(const PortablePackage &package, bool include_id);
std::string canonical_json(const nlohmann::json &value);
void bind_manifest_relocations(PortablePackage &package);
Result<PortablePackage> parse_package_manifest(std::span<const std::byte> manifest_bytes,
                                               const std::map<std::string, ManifestArchiveEntry, std::less<>> &entries,
                                               bool verify_payloads, std::string_view filename);

} // namespace axk::package_internal
