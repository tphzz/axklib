#include "device_sessions_internal.hpp"

#include <algorithm>
#include <limits>

namespace axk::app::device_detail {
namespace {
class Preview final : public RandomAccessReader {
  public:
    Preview(std::shared_ptr<MutationTarget> base, std::vector<AlterationJournalPatch> patches)
        : base_(std::move(base)), patches_(std::move(patches)) {}
    std::uint64_t size() const noexcept override { return base_->size(); }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
        if (auto read = base_->read_exact_at(offset, bytes); !read)
            return read;
        const auto end = offset + bytes.size();
        for (const auto &patch : patches_) {
            const auto first = std::max(offset, patch.offset);
            const auto last = std::min(end, patch.offset + patch.replacement.size());
            if (first >= last)
                continue;
            if (auto read = patch.replacement.read_exact_at(
                    first - patch.offset,
                    bytes.subspan(static_cast<std::size_t>(first - offset), static_cast<std::size_t>(last - first)));
                !read)
                return std::unexpected(make_error(ErrorCode::io_read_failed, ErrorCategory::io, read.error().message));
        }
        return {};
    }

  private:
    std::shared_ptr<MutationTarget> base_;
    std::vector<AlterationJournalPatch> patches_;
};
} // namespace

Result<std::vector<AlterationJournalPatch>> align_patches(const std::shared_ptr<MutationTarget> &target,
                                                          std::span<const AlterationJournalPatch> patches) {
    std::vector<AlterationJournalPatch> ordered{patches.begin(), patches.end()};
    std::ranges::sort(ordered, {}, &AlterationJournalPatch::offset);
    const auto alignment = static_cast<std::uint64_t>(target->write_alignment());
    if (alignment == 0U || target->size() % alignment != 0U)
        return std::unexpected(Error{"device_write_alignment", "invalid physical device sector size"});
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    std::uint64_t previous_end{};
    std::vector<std::byte> current(64U * 1024U), expected(current.size());
    for (const auto &patch : ordered) {
        const auto size = patch.original.size();
        if (size == 0U || size != patch.replacement.size() || patch.offset < previous_end ||
            patch.offset > target->size() || size > target->size() - patch.offset)
            return std::unexpected(Error{"device_write_range", "invalid or overlapping byte patches"});
        previous_end = patch.offset + size;
        for (std::uint64_t at = 0U; at < size;) {
            const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(current.size(), size - at));
            auto observed = std::span{current}.first(count);
            auto original = std::span{expected}.first(count);
            if (auto read = target->read_exact_at(patch.offset + at, observed); !read)
                return std::unexpected(Error{"device_changed", read.error().message});
            if (auto read = patch.original.read_exact_at(at, original); !read)
                return std::unexpected(read.error());
            if (!std::ranges::equal(observed, original))
                return std::unexpected(Error{"device_changed", "byte patches no longer match the device"});
            at += count;
        }
        const auto first = patch.offset - patch.offset % alignment;
        const auto end = previous_end + (alignment - previous_end % alignment) % alignment;
        if (!ranges.empty() && ranges.back().second >= first)
            ranges.back().second = std::max(end, ranges.back().second);
        else
            ranges.emplace_back(first, end);
    }
    auto preview = std::make_shared<Preview>(target, std::move(ordered));
    std::vector<AlterationJournalPatch> aligned;
    for (const auto &[first, end] : ranges)
        aligned.push_back({first, {target, first, end - first}, {preview, first, end - first}});
    return aligned;
}
} // namespace axk::app::device_detail
