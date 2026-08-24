#pragma once

#include <cstdint>
#include <span>

#include "axklib/deletion.hpp"

namespace axk::detail {

struct ObjectDeletionManifestPlan {
    AlterationManifest manifest;
    std::uint64_t estimated_freed_bytes{};
    std::uint64_t estimated_freed_clusters{};
};

Result<ObjectDeletionManifestPlan> build_object_deletion_manifest(const Container &container,
                                                                  const ObjectCatalog &catalog,
                                                                  std::span<const ObjectDeletionImpact> impacts);

} // namespace axk::detail
