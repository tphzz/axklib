#include "device_content_inspection.hpp"
#include "device_text.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <memory>
#include <set>
#include <tuple>

#include <unicode/unistr.h>

#include "axklib/bytes.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/sfs.hpp"

namespace axk::app::device_detail {
namespace {
constexpr std::uint64_t read_budget = 8U * 1024U * 1024U;
constexpr std::size_t entry_budget = 128U;

bool magic(std::span<const std::byte> bytes, std::size_t offset, std::string_view expected) {
    return offset <= bytes.size() && expected.size() <= bytes.size() - offset &&
           std::memcmp(bytes.data() + offset, expected.data(), expected.size()) == 0;
}
std::uint64_t little64(const ByteReader &reader, std::size_t offset) {
    return *reader.le32(offset) | (static_cast<std::uint64_t>(*reader.le32(offset + 4U)) << 32U);
}
bool intersects(std::uint64_t first, std::uint64_t size, std::uint64_t second, std::uint64_t other_size) {
    return size != 0U && other_size != 0U && first < second + other_size && second < first + size;
}

class Inspector {
  public:
    Inspector(const RandomAccessReader &reader, const DeviceTargetDescription &target,
              const CancellationToken &cancellation, InspectionClock now)
        : reader_(reader), target_(target), cancellation_(cancellation), now_(std::move(now)), start_(now_()) {}

    Result<void> deadline() const {
        if (cancellation_.is_cancelled())
            return std::unexpected(Error{"operation_cancelled", "Device inspection cancelled"});
        if (now_() - start_ > std::chrono::seconds{10})
            return std::unexpected(Error{"device_inspection_limit", "Device inspection deadline exceeded"});
        return {};
    }

    Result<std::vector<std::byte>> read(std::uint64_t offset, std::size_t length) {
        if (auto active = deadline(); !active)
            return std::unexpected(active.error());
        if (length > read_budget - read_) {
            result.complete = false;
            return std::unexpected(Error{"device_inspection_limit", "Device inspection reached its bounded limit"});
        }
        if (offset % 512U != 0U || length % 512U != 0U || offset > reader_.size() || length > reader_.size() - offset)
            return std::unexpected(Error{"device_inspection_bounds", "Device inspection range is invalid"});
        std::vector<std::byte> bytes(length);
        read_ += length;
        if (auto loaded = reader_.read_exact_at(offset, bytes); !loaded)
            return std::unexpected(Error{"device_inspection_unreadable", loaded.error().message});
        if (auto active = deadline(); !active)
            return std::unexpected(active.error());
        for (std::size_t at = 0; at < length; at += 512U) {
            auto hash = digest(std::span{bytes}.subspan(at, 512U));
            if (!hash)
                return std::unexpected(hash.error());
            const auto found = result.guards.find(offset + at);
            if (found != result.guards.end() && found->second != *hash)
                return std::unexpected(Error{"device_changed", "Device contents changed during inspection"});
            result.guards[offset + at] = std::move(*hash);
        }
        if (auto active = deadline(); !active)
            return std::unexpected(active.error());
        return bytes;
    }

    void warning(std::string message) {
        if (std::ranges::find(result.warnings, message) == result.warnings.end())
            result.warnings.push_back(std::move(message));
    }
    void add(DetectedContent content) {
        if (result.contents.size() >= entry_budget * 4U) {
            result.complete = false;
            warning("Content inventory exceeds the 128-entry inspection limit");
            return;
        }
        if (!seen_.emplace(content.kind, content.type, content.offset, content.label).second)
            return;
        content.label = device_text(content.label);
        if (content.validated && content.size &&
            intersects(content.offset, content.size, target_.offset, target_.size_bytes) &&
            (content.offset < target_.offset || content.size > target_.offset + target_.size_bytes - content.offset))
            result.crosses_target = true;
        result.contents.push_back(std::move(content));
    }

    Result<void> inspect_at(std::uint64_t base, std::uint64_t size, bool partition_tables) {
        if (!inspected_.emplace(base).second)
            return {};
        const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(4096U, size));
        if (length < 512U || length % 512U != 0U)
            return std::unexpected(Error{"device_inspection_bounds", "Device target is not sector aligned"});
        auto head = read(base, length);
        if (!head)
            return std::unexpected(head.error());
        if (magic(*head, 0, "YAMAHA_dev3")) {
            if (auto checked = sfs(base, size); !checked)
                return checked;
        } else {
            for (const auto &[offset, value, type] :
                 std::array<std::tuple<std::size_t, std::string_view, std::string_view>, 8>{
                     {{3U, "NTFS    ", "NTFS"},
                      {3U, "EXFAT   ", "exFAT"},
                      {54U, "FAT12   ", "FAT12"},
                      {54U, "FAT16   ", "FAT16"},
                      {82U, "FAT32   ", "FAT32"},
                      {3U, "-FVE-FS-", "BitLocker"},
                      {0U, "LUKS\xba\xbe", "LUKS"},
                      {32U, "NXSB", "APFS"}}}) {
                if (magic(*head, offset, value))
                    add({"filesystem-signature", std::string{type}, {}, base, size, false});
            }
            if (length >= 1082U && *ByteReader{*head}.le16(1080U) == 0xef53U)
                add({"filesystem-signature", "ext-family", {}, base, size, false});
            if (length >= 1026U && (magic(*head, 1024U, "H+") || magic(*head, 1024U, "HX")))
                add({"filesystem-signature", "HFS+", {}, base, size, false});
        }
        if (!partition_tables)
            return {};
        auto tail = read(base + size - 512U, 512U);
        if (!tail)
            return std::unexpected(tail.error());
        if (magic(*head, 512U, "EFI PART") || magic(*tail, 0, "EFI PART"))
            return gpt(base, size);
        if ((*head)[510] == std::byte{0x55} && (*head)[511] == std::byte{0xaa})
            return mbr(base, size, *head);
        if (magic(*head, 0U, "ER") || magic(*head, 512U, "PM")) {
            add({"partition-table-signature", "Apple partition map", {}, base, size, false});
            result.complete = false;
            warning("Apple partition map requires a complete native partition inventory before formatting");
        }
        return {};
    }

    ContentInspection result;

  private:
    class View final : public RandomAccessReader {
      public:
        View(Inspector &inspector, std::uint64_t offset, std::uint64_t length)
            : inspector_(inspector), offset_(offset), length_(length) {}
        std::uint64_t size() const noexcept override { return length_; }
        axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> bytes) const override {
            if (offset > length_ || bytes.size() > length_ - offset)
                return std::unexpected(
                    make_error(ErrorCode::out_of_bounds, ErrorCategory::io, "inspection view bounds"));
            const auto start = offset / 512U * 512U;
            const auto end = (offset + bytes.size() + 511U) / 512U * 512U;
            auto loaded = inspector_.read(offset_ + start, static_cast<std::size_t>(end - start));
            if (!loaded) {
                failure = loaded.error();
                return std::unexpected(
                    make_error(ErrorCode::io_read_failed, ErrorCategory::io, loaded.error().message));
            }
            std::memcpy(bytes.data(), loaded->data() + (offset - start), bytes.size());
            return {};
        }
        mutable std::optional<Error> failure;

      private:
        Inspector &inspector_;
        std::uint64_t offset_;
        std::uint64_t length_;
    };

    Result<void> sfs(std::uint64_t base, std::uint64_t size) {
        auto view = std::make_shared<View>(*this, base, size);
        auto opened = inspect_sfs_layout(*view, cancellation_);
        if (view->failure)
            return std::unexpected(*view->failure);
        const auto declared = opened ? opened->declared_size_bytes : 0U;
        if (!opened || declared > size) {
            result.complete = false;
            add({"filesystem-signature", "Yamaha SFS (damaged or inconsistent)", {}, base, size, false});
            warning("Yamaha SFS metadata is damaged or inconsistent");
            return {};
        }
        add({"filesystem", "Yamaha SFS", {}, base, declared, true});
        for (const auto &partition : opened->partitions) {
            add({"partition", "Yamaha SFS", partition.name,
                 base + static_cast<std::uint64_t>(partition.start_sector) * 512U,
                 static_cast<std::uint64_t>(partition.sector_count) * 512U, true});
        }
        return {};
    }

    Result<void> mbr(std::uint64_t base, std::uint64_t size, std::span<const std::byte> head) {
        const ByteReader reader{head};
        bool found{};
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        for (std::size_t index = 0; index < 4U; ++index) {
            const auto at = 0x1beU + index * 16U;
            const auto type = *reader.u8(at + 4U);
            const auto first = static_cast<std::uint64_t>(*reader.le32(at + 8U)) * 512U;
            const auto length = static_cast<std::uint64_t>(*reader.le32(at + 12U)) * 512U;
            if (type == 0U && length == 0U && first == 0U)
                continue;
            if (type == 0U || length == 0U || (*reader.u8(at) != 0U && *reader.u8(at) != 0x80U) || first < 512U ||
                first > size || length > size - first || std::ranges::any_of(ranges, [&](const auto &range) {
                    return first < range.second && range.first < first + length;
                })) {
                result.complete = false;
                add({"partition-table-signature", "MBR (inconsistent)", {}, base, size, false});
                warning("MBR contains malformed or overlapping partitions; inspection is incomplete");
                continue;
            }
            ranges.emplace_back(first, first + length);
            found = true;
            add({"partition", "MBR type " + std::to_string(type), {}, base + first, length, true});
            if (type == 0x05U || type == 0x0fU || type == 0x85U || type == 0xeeU) {
                result.complete = false;
                warning("Extended or protective MBR requires complete partition-table inspection");
            } else if (auto child = inspect_at(base + first, length, false); !child) {
                return child;
            }
        }
        if (found) {
            add({"partition-table", "MBR", {}, base, 512U, true});
            shared_metadata(base, 512U);
        }
        return {};
    }

    bool header_valid(std::span<const std::byte> bytes, std::uint64_t current, std::uint64_t backup) {
        if (bytes.size() != 512U || !magic(bytes, 0U, "EFI PART"))
            return false;
        const ByteReader reader{bytes};
        const auto size = *reader.le32(12U);
        if (size < 92U || size > 512U || little64(reader, 24U) != current || little64(reader, 32U) != backup)
            return false;
        std::vector<std::byte> header(bytes.begin(), bytes.begin() + size);
        std::fill(header.begin() + 16U, header.begin() + 20U, std::byte{});
        return package_internal::crc32(header) == *reader.le32(16U);
    }

    Result<void> gpt(std::uint64_t base, std::uint64_t size) {
        auto primary = read(base + 512U, 512U);
        auto backup = read(base + size - 512U, 512U);
        if (!primary || !backup)
            return std::unexpected(!primary ? primary.error() : backup.error());
        const auto last_sector = size / 512U - 1U;
        if (!header_valid(*primary, 1U, last_sector) || !header_valid(*backup, last_sector, 1U)) {
            result.complete = false;
            add({"partition-table-signature", "GPT (inconsistent)", {}, base, size, false});
            warning("GPT headers are damaged or inconsistent; a complete inventory is unavailable");
            return {};
        }
        const ByteReader first{*primary};
        const ByteReader second{*backup};
        const auto count = *first.le32(80U);
        const auto entry_size = *first.le32(84U);
        if (count > entry_budget || count == 0U || entry_size < 128U || entry_size > 4096U || entry_size % 128U != 0U ||
            *second.le32(80U) != count || *second.le32(84U) != entry_size ||
            little64(first, 40U) != little64(second, 40U) || little64(first, 48U) != little64(second, 48U) ||
            !std::equal(primary->begin() + 56U, primary->begin() + 72U, backup->begin() + 56U)) {
            result.complete = false;
            warning("GPT entry geometry exceeds inspection limits or differs between copies");
            return {};
        }
        const auto array_bytes = static_cast<std::size_t>(count) * entry_size;
        const auto rounded = (array_bytes + 511U) / 512U * 512U;
        const auto first_lba = little64(first, 72U);
        const auto second_lba = little64(second, 72U);
        const auto usable_first = little64(first, 40U);
        const auto usable_last = little64(first, 48U);
        if (first_lba < 2U || first_lba > last_sector || second_lba > last_sector ||
            rounded / 512U > last_sector - first_lba || rounded / 512U > last_sector - second_lba ||
            usable_first < first_lba + rounded / 512U || usable_first > usable_last || usable_last >= second_lba ||
            second_lba + rounded / 512U > last_sector) {
            result.complete = false;
            warning("GPT tables or usable ranges are invalid");
            return {};
        }
        auto entries = read(base + first_lba * 512U, rounded);
        auto copies = read(base + second_lba * 512U, rounded);
        if (!entries || !copies)
            return std::unexpected(!entries ? entries.error() : copies.error());
        if (*entries != *copies ||
            package_internal::crc32(std::span{*entries}.first(array_bytes)) != *first.le32(88U) ||
            *first.le32(88U) != *second.le32(88U)) {
            result.complete = false;
            warning("GPT entry arrays differ or fail their checksums");
            return {};
        }
        add({"partition-table", "GPT", {}, base, (first_lba * 512U) + rounded, true});
        add({"partition-table", "GPT backup", {}, base + second_lba * 512U, size - second_lba * 512U, true});
        std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
        for (std::size_t index = 0; index < count; ++index) {
            const auto entry = std::span{*entries}.subspan(index * entry_size, entry_size);
            if (std::ranges::all_of(entry.first(16U), [](auto byte) { return byte == std::byte{}; }))
                continue;
            const ByteReader row{entry};
            const auto begin = little64(row, 32U);
            const auto end = little64(row, 40U);
            if (begin < usable_first || end > usable_last || begin > end ||
                std::ranges::any_of(ranges,
                                    [&](const auto &range) { return begin <= range.second && range.first <= end; })) {
                result.complete = false;
                warning("GPT contains out-of-range or overlapping partitions");
                continue;
            }
            ranges.emplace_back(begin, end);
            std::u16string label;
            for (std::size_t at = 56U; at < 128U; at += 2U) {
                const auto character = *row.le16(at);
                if (character == 0U)
                    break;
                label.push_back(static_cast<char16_t>(character));
            }
            std::string name;
            icu::UnicodeString{label.data(), static_cast<std::int32_t>(label.size())}.toUTF8String(name);
            const auto start = base + begin * 512U;
            const auto length = (end - begin + 1U) * 512U;
            add({"partition", "GPT", std::move(name), start, length, true});
            if (auto child = inspect_at(start, length, false); !child)
                return child;
        }
        shared_metadata(base, (first_lba * 512U) + rounded);
        shared_metadata(base + second_lba * 512U, size - second_lba * 512U);
        return {};
    }

    void shared_metadata(std::uint64_t offset, std::uint64_t size) {
        if (target_.offset == 0U && target_.size_bytes == reader_.size())
            return;
        if (!intersects(offset, size, target_.offset, target_.size_bytes))
            return;
        result.shared_partition_metadata = std::ranges::any_of(result.contents, [&](const auto &content) {
            return content.kind == "partition" && content.validated &&
                   (content.offset < target_.offset || content.offset >= target_.offset + target_.size_bytes ||
                    content.size > target_.offset + target_.size_bytes - content.offset);
        });
    }

    const RandomAccessReader &reader_;
    const DeviceTargetDescription &target_;
    CancellationToken cancellation_;
    InspectionClock now_;
    std::chrono::steady_clock::time_point start_;
    std::uint64_t read_{};
    std::set<std::tuple<std::string, std::string, std::uint64_t, std::string>> seen_;
    std::set<std::uint64_t> inspected_;
};
} // namespace

Result<ContentInspection> inspect_contents(const RandomAccessReader &carrier, const DeviceTargetDescription &target,
                                           const CancellationToken &cancellation, InspectionClock now) {
    if (target.offset > carrier.size() || target.size_bytes > carrier.size() - target.offset ||
        target.size_bytes < 512U || target.offset % 512U != 0U || target.size_bytes % 512U != 0U ||
        carrier.size() % 512U != 0U)
        return std::unexpected(Error{"device_layout_invalid", "Inspection target geometry is invalid"});
    Inspector inspector{carrier, target, cancellation, std::move(now)};
    if (auto parent = inspector.inspect_at(0U, carrier.size(), true); !parent)
        return std::unexpected(parent.error());
    if (auto selected = inspector.inspect_at(target.offset, target.size_bytes, true); !selected)
        return std::unexpected(selected.error());
    if (inspector.result.contents.empty())
        inspector.warning("No supported signatures detected. Existing data may still be present; this does not imply "
                          "an empty device.");
    if (inspector.result.crosses_target || inspector.result.shared_partition_metadata)
        inspector.warning("Existing structures span targets or share partition metadata. Formatting this target could "
                          "destroy access to outside data.");
    if (auto active = inspector.deadline(); !active)
        return std::unexpected(active.error());
    return std::move(inspector.result);
}
} // namespace axk::app::device_detail
