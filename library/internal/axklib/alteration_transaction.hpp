#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/io.hpp"
#include "axklib/package.hpp"

namespace axk::detail {

struct AlterationPatch {
    std::uint64_t offset{};
    std::vector<std::byte> original;
    std::vector<std::byte> replacement;
};

struct PreparedAlteration {
    std::filesystem::path source_path;
    std::uint64_t image_size_bytes{};
    std::vector<OperationReport> operations;
    std::vector<AlterationPatch> patches;
};

struct PreparedPackageImport {
    std::filesystem::path source_path;
    std::uint64_t image_size_bytes{};
    std::string plan_id;
    std::string source_snapshot_id;
    std::vector<PlannedPackageObject> objects;
    std::vector<PackageAllocationDelta> allocation;
    std::vector<AlterationPatch> patches;
};

[[nodiscard]] Result<PreparedAlteration> prepare_hds_alteration(std::shared_ptr<const RandomAccessReader> source,
                                                                std::filesystem::path source_path,
                                                                const AlterationManifest &manifest,
                                                                const CancellationToken &cancellation = {},
                                                                ProgressSink *progress = nullptr);

[[nodiscard]] Result<PreparedPackageImport>
prepare_sfs_package_import(std::shared_ptr<const RandomAccessReader> source, std::filesystem::path source_path,
                           std::span<const PortablePackage> packages, const PackageImportPlan &plan,
                           const CancellationToken &cancellation = {}, ProgressSink *progress = nullptr);

[[nodiscard]] Result<PreparedPackageImport>
prepare_sfs_package_import_verified(std::shared_ptr<const RandomAccessReader> source, std::filesystem::path source_path,
                                    std::span<const PortablePackage> packages, const PackageImportPlan &plan,
                                    std::string_view verified_source_snapshot_id,
                                    const CancellationToken &cancellation = {}, ProgressSink *progress = nullptr);

} // namespace axk::detail
