#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>

#include "axklib/sfs.hpp"

namespace axk::detail {

class SfsRecordReader final : public RandomAccessReader {
  public:
    SfsRecordReader(std::shared_ptr<const Container> container, PartitionIndex partition, SfsId record,
                    std::uint64_t size, CancellationToken cancellation)
        : container_(std::move(container)), partition_(partition), record_(record), size_(size),
          cancellation_(std::move(cancellation)) {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return size_; }

    [[nodiscard]] Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        if (offset > size_ || destination.size() > size_ - offset) {
            return std::unexpected{
                make_error(ErrorCode::out_of_bounds, ErrorCategory::io, "SFS record read is out of bounds")};
        }
        auto bytes = container_->read_record_range(partition_, record_, offset, destination.size(), cancellation_);
        if (!bytes)
            return std::unexpected{bytes.error()};
        if (bytes->size() != destination.size()) {
            return std::unexpected{
                make_error(ErrorCode::io_short_read, ErrorCategory::io, "SFS record read was short")};
        }
        std::ranges::copy(*bytes, destination.begin());
        return {};
    }

  private:
    std::shared_ptr<const Container> container_;
    PartitionIndex partition_;
    SfsId record_;
    std::uint64_t size_{};
    CancellationToken cancellation_;
};

} // namespace axk::detail
