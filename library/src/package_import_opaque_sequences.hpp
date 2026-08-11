#pragma once

#include <span>

#include "axklib/catalog.hpp"
#include "axklib/package.hpp"

namespace axk::package_import_internal {

bool is_importable_opaque_sequence(const PortablePackage &package, const PackageNode &node);
bool skip_opaque_sequence(const PortablePackage &package, const PackageImportPolicy &policy, std::size_t package_index,
                          const PackageNode &node);
void validate_opaque_sequence_policy(std::span<const PortablePackage> packages, const PackageImportPolicy &policy,
                                     PackageImportPlan &plan);
void append_sfs_catalog_issues(PackageImportPlan &plan, std::span<const CatalogIssue> issues,
                               std::span<const ObjectSnapshot *const> objects);
void reject_preserved_target_object_use(PackageImportPlan &plan);

} // namespace axk::package_import_internal
