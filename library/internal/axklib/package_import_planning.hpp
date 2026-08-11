#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

#include "axklib/package.hpp"

namespace axk::package_import_internal {

struct RetainedPackageImportStats {
    std::uint64_t target_payload_bytes_read{};
    std::size_t target_payload_objects_read{};
};

struct RetainedPackageImportTarget {
    std::shared_ptr<const RandomAccessReader> reader;
    std::filesystem::path path;
    const MediaContainer *media{};
    std::string snapshot_id;
    std::span<const ObjectSnapshot *const> catalog_objects;
    std::span<const CatalogIssue> catalog_issues;
    RetainedPackageImportStats *stats{};
    bool packages_verified{};
};

Result<PackageImportPlan> plan_package_import_retained(const RetainedPackageImportTarget &target,
                                                       std::span<const PortablePackage> packages,
                                                       const PackageImportRequest &request,
                                                       const CancellationToken &cancellation = {});

} // namespace axk::package_import_internal
