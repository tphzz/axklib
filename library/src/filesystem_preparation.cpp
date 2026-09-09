#include "axklib/filesystem_transaction.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace axk::detail {
Result<std::vector<FilesystemWritePatch>> normalize_filesystem_patches(const RandomAccessReader &source,
                                                                       std::span<const FilesystemWritePatch> patches,
                                                                       std::uint64_t begin, std::uint64_t size,
                                                                       const CancellationToken &cancellation) {
    if (begin > source.size() || size > source.size() - begin)
        return std::unexpected(make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                          "filesystem partition exceeds its source"));
    const auto end = begin + size;
    std::map<std::uint64_t, FilesystemWritePatch> ranges;
    for (const auto &patch : patches) {
        if (auto checked = cancellation.check(); !checked)
            return std::unexpected(checked.error());
        if (!patch.source || patch.source_offset > patch.source->size() ||
            patch.size > patch.source->size() - patch.source_offset || patch.offset < begin || patch.offset > end ||
            patch.size > end - patch.offset || patch.offset > source.size() ||
            patch.size > source.size() - patch.offset)
            return std::unexpected(make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                              "filesystem patch exceeds its input or partition boundary"));
        if (patch.size == 0U)
            continue;
        const auto patch_end = patch.offset + patch.size;
        auto it = ranges.lower_bound(patch.offset);
        if (it != ranges.begin()) {
            const auto previous = std::prev(it);
            if (previous->first + previous->second.size > patch.offset)
                it = previous;
        }
        // Later operations replace earlier writes to reused allocation. Keep
        // the remaining left/right slices without copying their payloads.
        while (it != ranges.end() && it->first < patch_end) {
            const auto old = it->second;
            it = ranges.erase(it);
            if (old.offset < patch.offset) {
                auto left = old;
                left.size = patch.offset - old.offset;
                ranges.emplace(left.offset, std::move(left));
            }
            if (old.offset + old.size > patch_end) {
                auto right = old;
                right.offset = patch_end;
                right.source_offset += patch_end - old.offset;
                right.size -= patch_end - old.offset;
                ranges.emplace(right.offset, std::move(right));
                break;
            }
        }
        ranges.emplace(patch.offset, patch);
    }
    std::vector<FilesystemWritePatch> result;
    result.reserve(ranges.size());
    for (auto &[offset, patch] : ranges) {
        static_cast<void>(offset);
        result.push_back(std::move(patch));
    }
    return result;
}

class FilesystemPreview final : public RandomAccessReader {
  public:
    FilesystemPreview(std::shared_ptr<const RandomAccessReader> source, std::vector<FilesystemWritePatch> patches)
        : source_(std::move(source)), patches_(std::move(patches)) {}
    std::uint64_t size() const noexcept override { return source_->size(); }
    Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        if (auto read = source_->read_exact_at(offset, destination); !read)
            return read;
        const auto end = offset + destination.size();
        auto it = std::ranges::upper_bound(patches_, offset, {}, &FilesystemWritePatch::offset);
        if (it != patches_.begin())
            --it;
        for (; it != patches_.end() && it->offset < end; ++it) {
            const auto from = std::max(offset, it->offset);
            const auto to = std::min(end, it->offset + it->size);
            if (from >= to)
                continue;
            auto target =
                destination.subspan(static_cast<std::size_t>(from - offset), static_cast<std::size_t>(to - from));
            if (auto read = it->source->read_exact_at(it->source_offset + from - it->offset, target); !read)
                return read;
        }
        return {};
    }

  private:
    std::shared_ptr<const RandomAccessReader> source_;
    std::vector<FilesystemWritePatch> patches_;
};

std::shared_ptr<const RandomAccessReader> filesystem_preview(std::shared_ptr<const RandomAccessReader> source,
                                                             std::vector<FilesystemWritePatch> patches) {
    return std::make_shared<FilesystemPreview>(std::move(source), std::move(patches));
}
} // namespace axk::detail
