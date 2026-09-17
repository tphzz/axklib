#pragma once

#include "axklib/catalog.hpp"
#include "axklib/package.hpp"

namespace axk::package_internal {
[[nodiscard]] Result<PortablePackage> build_graph(MediaKind source_kind, const ObjectCatalog &catalog,
                                                  std::span<const PackageRootSelector> roots,
                                                  const CancellationToken &cancellation);
} // namespace axk::package_internal
