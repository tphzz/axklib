#include "rawdisk/slice.hpp"
#include "detail.hpp"

#include <limits>

namespace rawdisk {
namespace {

using detail::fail;

class Slice final : public BlockDevice {
  public:
    Slice(std::shared_ptr<BlockDevice> parent, std::uint64_t first, std::uint64_t count, std::uint32_t ss,
          std::uint32_t ratio, SliceOptions options)
        : parent_(std::move(parent)), first_(first), count_(count), ss_(ss), ratio_(ratio), parent_ss_(ss / ratio),
          options_(std::move(options)) {}

    std::uint32_t sector_size() const noexcept override { return ss_; }
    std::uint64_t sector_count() const noexcept override { return count_; }
    bool writable() const noexcept override { return options_.writable; }

  protected:
    Result<void> check_write(std::uint64_t lba, std::uint64_t sectors) override {
        const Range w{first_ + lba * ratio_, first_ + (lba + sectors) * ratio_};
        for (const Range &p : options_.protected_ranges)
            if (w.overlaps(p))
                return fail(Errc::layout_conflict);
        return {};
    }
    Result<void> read_raw(std::uint64_t off, std::span<std::byte> out) override {
        return parent_->read(first_ + off / parent_ss_, out);
    }
    Result<void> write_raw(std::uint64_t off, std::span<const std::byte> in) override {
        return parent_->write(first_ + off / parent_ss_, in);
    }
    Result<void> flush_raw() override { return parent_->flush(); }

  private:
    std::shared_ptr<BlockDevice> parent_;
    std::uint64_t first_, count_;
    std::uint32_t ss_, ratio_, parent_ss_;
    SliceOptions options_;
};

} // namespace

Result<std::shared_ptr<BlockDevice>> make_slice(std::shared_ptr<BlockDevice> parent, std::uint64_t first,
                                                std::uint64_t count, std::uint32_t sector_size,
                                                SliceOptions options) noexcept {
    return detail::guard([&]() -> Result<std::shared_ptr<BlockDevice>> {
        if (!parent || count == 0)
            return fail(Errc::invalid_argument);
        const std::uint32_t pss = parent->sector_size();
        if (pss == 0 || sector_size < pss || sector_size > 65536 || sector_size % pss != 0)
            return fail(Errc::invalid_argument);
        const std::uint32_t ratio = sector_size / pss;
        const std::uint64_t n = parent->sector_count();
        if (count > std::numeric_limits<std::uint64_t>::max() / ratio)
            return fail(Errc::out_of_range);
        const std::uint64_t span = count * ratio;
        if (first > n || span > n - first)
            return fail(Errc::out_of_range);
        if (options.writable && !parent->writable())
            return fail(Errc::read_only_handle);
        return std::shared_ptr<BlockDevice>(
            std::make_shared<Slice>(std::move(parent), first, count, sector_size, ratio, std::move(options)));
    });
}

} // namespace rawdisk
