#pragma once

#include <cstdint>
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
Result<void> resize_temporary_file(const std::filesystem::path &path, std::uint64_t size);
Result<void> write_temporary_file_at(const std::filesystem::path &path, std::uint64_t offset,
                                     std::span<const std::byte> bytes);
Result<void> flush_file_to_disk(const std::filesystem::path &path);
void discard_temporary_file(const std::filesystem::path &path) noexcept;
Result<void> publish_temporary_file(const std::filesystem::path &temporary, const std::filesystem::path &output,
                                    bool overwrite);

} // namespace axk::detail
