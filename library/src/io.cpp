#include "axklib/io.hpp"

#include <algorithm>
#include <fstream>
#include <limits>

#include "axklib/bytes.hpp"
#include "axklib/utf8.hpp"

namespace axk {

CancellationToken::CancellationToken() : state_{std::make_shared<std::atomic_bool>(false)} {}

CancellationToken::CancellationToken(std::shared_ptr<std::atomic_bool> state) noexcept
    : state_{std::move(state)} {}

bool CancellationToken::is_cancelled() const noexcept {
  return state_->load(std::memory_order_acquire);
}

Result<void> CancellationToken::check() const {
  if (is_cancelled()) {
    return std::unexpected{make_error(ErrorCode::operation_cancelled, ErrorCategory::cancelled,
                                      "operation cancelled")};
  }
  return {};
}

CancellationSource::CancellationSource() : state_{std::make_shared<std::atomic_bool>(false)} {}

CancellationToken CancellationSource::token() const noexcept { return CancellationToken{state_}; }

void CancellationSource::cancel() const noexcept { state_->store(true, std::memory_order_release); }

void CancellationSource::reset() const noexcept { state_->store(false, std::memory_order_release); }

MemoryReader::MemoryReader(std::vector<std::byte> bytes) : bytes_{std::move(bytes)} {}

std::uint64_t MemoryReader::size() const noexcept {
  return static_cast<std::uint64_t>(bytes_.size());
}

Result<void> MemoryReader::read_exact_at(std::uint64_t offset,
                                         std::span<std::byte> destination) const {
  const auto end = checked_add(offset, destination.size());
  if (!end) {
    return std::unexpected{end.error()};
  }
  if (*end > bytes_.size()) {
    ErrorContext context;
    context.raw_offset = offset;
    return std::unexpected{make_error(ErrorCode::io_short_read, ErrorCategory::io,
                                      "memory read exceeds available data", std::move(context))};
  }
  const auto begin_index = static_cast<std::size_t>(offset);
  std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(begin_index), destination.size(),
              destination.begin());
  return {};
}

FileReader::FileReader(std::filesystem::path path, std::uint64_t size) noexcept
    : path_{std::move(path)}, size_{size} {}

Result<std::shared_ptr<FileReader>> FileReader::open(const std::filesystem::path &path) {
  std::error_code error;
  const auto file_size = std::filesystem::file_size(path, error);
  if (error) {
    ErrorContext context;
    context.source_path = text::path_to_utf8(path);
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "cannot open input file: " + error.message(),
                                      std::move(context))};
  }
  if (file_size > std::numeric_limits<std::uint64_t>::max()) {
    ErrorContext context;
    context.source_path = text::path_to_utf8(path);
    return std::unexpected{make_error(ErrorCode::io_unsupported_size, ErrorCategory::io,
                                      "input file size exceeds the supported 64-bit range",
                                      std::move(context))};
  }
  return std::shared_ptr<FileReader>{new FileReader{path, static_cast<std::uint64_t>(file_size)}};
}

std::uint64_t FileReader::size() const noexcept { return size_; }

Result<void> FileReader::read_exact_at(std::uint64_t offset,
                                       std::span<std::byte> destination) const {
  const auto end = checked_add(offset, destination.size());
  if (!end) {
    return std::unexpected{end.error()};
  }
  if (*end > size_) {
    ErrorContext context;
    context.source_path = text::path_to_utf8(path_);
    context.raw_offset = offset;
    return std::unexpected{make_error(ErrorCode::io_short_read, ErrorCategory::io,
                                      "file read exceeds available data", std::move(context))};
  }
  if (offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
    return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::io,
                                      "file offset exceeds stream implementation range")};
  }
  if (destination.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
    return std::unexpected{make_error(ErrorCode::invalid_argument, ErrorCategory::io,
                                      "read size exceeds stream implementation range")};
  }

  std::scoped_lock lock{mutex_};
  std::ifstream input{path_, std::ios::binary};
  if (!input) {
    ErrorContext context;
    context.source_path = text::path_to_utf8(path_);
    return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                      "cannot open input file for reading", std::move(context))};
  }
  input.seekg(static_cast<std::streamoff>(offset));
  input.read(reinterpret_cast<char *>(destination.data()),
             static_cast<std::streamsize>(destination.size()));
  if (input.gcount() != static_cast<std::streamsize>(destination.size())) {
    ErrorContext context;
    context.source_path = text::path_to_utf8(path_);
    context.raw_offset = offset;
    return std::unexpected{make_error(ErrorCode::io_read_failed, ErrorCategory::io,
                                      "failed to read requested bytes", std::move(context))};
  }
  return {};
}

Result<void> FileReader::read_exact_at(std::uint64_t offset, std::span<std::byte> destination,
                                       const CancellationToken &cancellation) const {
  if (const auto before = cancellation.check(); !before) {
    return before;
  }
  if (const auto read = read_exact_at(offset, destination); !read) {
    return read;
  }
  return cancellation.check();
}

} // namespace axk
