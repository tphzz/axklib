#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "axklib/alteration.hpp"
#include "axklib/io.hpp"

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

[[nodiscard]] Result<PreparedAlteration> prepare_hds_alteration(std::shared_ptr<const RandomAccessReader> source,
                                                                std::filesystem::path source_path,
                                                                const AlterationManifest &manifest,
                                                                const CancellationToken &cancellation = {},
                                                                ProgressSink *progress = nullptr);

} // namespace axk::detail
