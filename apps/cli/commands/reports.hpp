#pragma once

#include <filesystem>
#include <span>
#include <vector>

#include "support.hpp"

#include "axklib/sfs.hpp"

namespace axk::cli::commands {

std::vector<ReportRow> relationship_rows(const CliLoadResult &loaded);
std::vector<ReportRow> sbac_detail_rows(const CliLoadResult &loaded);
std::vector<ReportRow> bitmap_detail_rows(const CliLoadResult &loaded);
std::vector<ReportRow> program_detail_rows(const CliLoadResult &loaded);
std::vector<ReportRow> program_ignored_detail_rows(const CliLoadResult &loaded);
std::vector<ReportRow>
allocation_mismatch_rows(const std::filesystem::path &path,
                         std::span<const Partition> partitions);

} // namespace axk::cli::commands
