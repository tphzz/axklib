#pragma once

#include <filesystem>
#include <string>

#include "axklib/application/contracts.hpp"
#include "axklib/io.hpp"

namespace axk::app::detail {

[[nodiscard]] Result<std::string> reader_sha256(const RandomAccessReader &reader,
                                                const CancellationToken &cancellation = {});
[[nodiscard]] Result<std::string> file_sha256(const std::filesystem::path &path,
                                              const CancellationToken &cancellation = {});

} // namespace axk::app::detail
