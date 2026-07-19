#pragma once

#include <filesystem>
#include <functional>
#include <span>

#include "axklib/error.hpp"

namespace axk::detail {

using TemporaryFileSink = std::function<Result<void>(std::span<const std::byte>)>;
using TemporaryFileProducer = std::function<Result<void>(const TemporaryFileSink &)>;

Result<std::filesystem::path> reserve_temporary_file(const std::filesystem::path &output);
Result<std::filesystem::path> write_temporary_file(const std::filesystem::path &output,
                                                   const TemporaryFileProducer &producer);
Result<void> flush_file_to_disk(const std::filesystem::path &path);
Result<void> publish_temporary_file(const std::filesystem::path &temporary, const std::filesystem::path &output,
                                    bool overwrite);

} // namespace axk::detail
