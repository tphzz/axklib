#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

#include "axklib/package.hpp"

namespace axk::package_internal {

nlohmann::json manifest_json(const PortablePackage &package, bool include_id);
std::string canonical_json(const nlohmann::json &value);
void bind_manifest_relocations(PortablePackage &package);

} // namespace axk::package_internal
