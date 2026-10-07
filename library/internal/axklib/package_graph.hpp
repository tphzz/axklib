#pragma once

#include "axklib/catalog.hpp"
#include "axklib/package.hpp"
#include "axklib/relationship.hpp"

namespace axk::package_internal {
[[nodiscard]] Result<PortablePackage> build_graph(MediaKind source_kind, const ObjectCatalog &catalog,
                                                  std::span<const PackageRootSelector> roots,
                                                  const CancellationToken &cancellation);
[[nodiscard]] Result<PortablePackage> build_graph(MediaKind source_kind, const ObjectCatalog &catalog,
                                                  const RelationshipGraph &relationships,
                                                  std::span<const PackageRootSelector> roots,
                                                  const CancellationToken &cancellation);
} // namespace axk::package_internal
