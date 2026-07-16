#pragma once

#include <filesystem>

#include "axklib/error.hpp"

namespace axk::detail {

Result<void> flush_file_to_disk(const std::filesystem::path &path);
Result<void> publish_temporary_file(const std::filesystem::path &temporary, const std::filesystem::path &output,
                                    bool overwrite);

} // namespace axk::detail
