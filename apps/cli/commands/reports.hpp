#pragma once

#include <vector>

#include "support.hpp"

namespace axk::cli::commands {

std::vector<ReportRow> relationship_rows(const CliLoadResult &loaded);
std::vector<ReportRow> sbac_detail_rows(const CliLoadResult &loaded);
std::vector<ReportRow> bitmap_detail_rows(const CliLoadResult &loaded);
std::vector<ReportRow> program_detail_rows(const CliLoadResult &loaded);

} // namespace axk::cli::commands
