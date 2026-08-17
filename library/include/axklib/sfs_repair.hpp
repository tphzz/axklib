#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include "axklib/io.hpp"
#include "axklib/publication.hpp"
#include "axklib/sfs.hpp"

namespace axk {

struct SfsExtentLayoutRepairTarget {
    PartitionIndex partition;
    SfsId record;
    std::vector<Extent> source_extents;
    std::vector<Extent> replacement_extents;
    std::vector<std::uint32_t> source_continuation_clusters;
    std::vector<std::uint32_t> replacement_continuation_clusters;
    std::uint64_t logical_size{};
};

struct SfsExtentLayoutRepairPlan {
    std::vector<SfsExtentLayoutRepairTarget> targets;
};

struct SfsExtentLayoutRepairResult {
    std::filesystem::path source_path;
    std::filesystem::path output_path;
    std::vector<SfsExtentLayoutRepairTarget> repairs;
    PublicationOutcome publication;
};

[[nodiscard]] AXK_API Result<SfsExtentLayoutRepairPlan> inspect_sfs_extent_layout_repair(const Container &container);

[[nodiscard]] AXK_API Result<SfsExtentLayoutRepairResult>
repair_sfs_extent_layout(const std::filesystem::path &source_path, const std::filesystem::path &output_path,
                         const CancellationToken &cancellation = {}, ProgressSink *progress = nullptr,
                         bool overwrite = false);

} // namespace axk
