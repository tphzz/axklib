#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "axklib/application/image_sessions.hpp"
#include "axklib/catalog.hpp"

namespace axk::app::formats::a_series {

struct VolumeScopeIdentity {
    std::uint8_t partition_index{};
    std::uint32_t volume_directory_id{};
    std::string display_name;
};

struct SessionRead : ImageSessionRead {
    std::vector<const ObjectSnapshot *> catalog_objects;
    std::vector<CatalogIssue> catalog_issues;
    std::unordered_map<std::string, std::string> object_keys_by_id;
    std::unordered_map<std::string, VolumeScopeIdentity> volume_scopes_by_id;
};

class SessionReader final {
  public:
    [[nodiscard]] static Result<SessionRead> acquire(ImageSessionManager &sessions, const ImageDocumentRef &target,
                                                     std::string_view owner_id, std::uint64_t expected_revision);
};

} // namespace axk::app::formats::a_series
