#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace axk::app {

struct NativeFilesystemBindings {
    std::optional<std::string> profile;
    std::optional<std::string> empty_sfs_profile;
    std::map<std::pair<std::uint8_t, std::uint32_t>, std::vector<std::string>> record_objects;
    std::map<std::uint64_t, std::vector<std::string>> offset_objects;
    std::map<std::string, std::string> interpretations;
    std::map<std::pair<std::uint8_t, std::uint32_t>, std::string> volume_scopes;
    std::map<std::uint8_t, std::string> partition_scopes;
};

} // namespace axk::app
