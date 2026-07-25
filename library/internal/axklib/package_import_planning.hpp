#pragma once

#include <filesystem>
#include <memory>
#include <span>

#include "axklib/package.hpp"

namespace axk::package_import_internal {

Result<PackageImportPlan> plan_package_import_retained(std::shared_ptr<const RandomAccessReader> target_reader,
                                                       std::filesystem::path target_path, const MediaContainer &target,
                                                       std::span<const PortablePackage> packages,
                                                       const PackageImportRequest &request,
                                                       const CancellationToken &cancellation = {});

} // namespace axk::package_import_internal
