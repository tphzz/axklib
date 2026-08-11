#pragma once

#include <filesystem>
#include <vector>

#include "axklib/error.hpp"

namespace axk::cli::commands {

Result<std::vector<std::filesystem::path>> expand_cli_paths(const std::vector<std::filesystem::path> &inputs,
                                                            bool include_object_directories = false);
int report_failure(const Error &error);

} // namespace axk::cli::commands
