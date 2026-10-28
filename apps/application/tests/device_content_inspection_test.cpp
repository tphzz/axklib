#include "device_content_inspection.hpp"

#include <algorithm>
#include <memory>

#include <gtest/gtest.h>

#include "axklib/package_archive.hpp"

namespace {
void little32(std::vector<std::byte> &data, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0; index < 4U; ++index)
        data[offset + index] = static_cast<std::byte>(value >> (index * 8U));
}
void text(std::vector<std::byte> &data, std::size_t offset, std::string_view value) {
    std::ranges::transform(value, data.begin() + static_cast<std::ptrdiff_t>(offset),
                           [](char c) { return static_cast<std::byte>(c); });
}
std::vector<std::byte> gpt_disk() {
    std::vector<std::byte> bytes(128U * 512U);
    bytes[1024U] = std::byte{1};
    little32(bytes, 1024U + 32U, 16U);
    little32(bytes, 1024U + 40U, 31U);
    std::copy_n(bytes.begin() + 1024U, 512U, bytes.begin() + 126U * 512U);
    for (const auto sector : {1U, 127U}) {
        const auto base = sector * 512U;
        text(bytes, base, "EFI PART");
        little32(bytes, base + 8U, 0x00010000U);
        little32(bytes, base + 12U, 92U);
        little32(bytes, base + 24U, sector);
        little32(bytes, base + 32U, sector == 1U ? 127U : 1U);
        little32(bytes, base + 40U, 3U);
        little32(bytes, base + 48U, 125U);
        little32(bytes, base + 72U, sector == 1U ? 2U : 126U);
        little32(bytes, base + 80U, 4U);
        little32(bytes, base + 84U, 128U);
        little32(bytes, base + 88U, axk::package_internal::crc32(std::span{bytes}.subspan(1024U, 512U)));
        little32(bytes, base + 16U, axk::package_internal::crc32(std::span{bytes}.subspan(base, 92U)));
    }
    return bytes;
}
} // namespace

TEST(DeviceContentInspection, DetectsPartitionsAndSignaturesWithoutCallingUnrecognizedMediaEmpty) {
    std::vector<std::byte> bytes(2U * 1024U * 1024U, std::byte{});
    bytes[510] = std::byte{0x55};
    bytes[511] = std::byte{0xaa};
    bytes[0x1c2] = std::byte{7};
    little32(bytes, 0x1c6, 1024U);
    little32(bytes, 0x1ca, 2048U);
    text(bytes, 1024U * 512U + 3U, "NTFS    ");
    axk::MemoryReader reader(std::move(bytes));
    const axk::app::DeviceTargetDescription whole{"disk", "Disk", 0U, reader.size(), 512U, false, {}};
    const auto inspected = axk::app::device_detail::inspect_contents(reader, whole);
    ASSERT_TRUE(inspected);
    EXPECT_TRUE(inspected->complete);
    EXPECT_FALSE(inspected->crosses_target);
    EXPECT_FALSE(inspected->guards.empty());
    EXPECT_TRUE(std::ranges::any_of(inspected->contents, [](const auto &entry) {
        return entry.kind == "partition" && entry.offset == 1024U * 512U && entry.validated;
    }));
    EXPECT_TRUE(std::ranges::any_of(inspected->contents,
                                    [](const auto &entry) { return entry.type == "NTFS" && !entry.validated; }));
    axk::MemoryReader unknown(std::vector<std::byte>(2U * 1024U * 1024U, std::byte{0x42}));
    const auto unrecognized = axk::app::device_detail::inspect_contents(unknown, whole);
    ASSERT_TRUE(unrecognized);
    EXPECT_TRUE(unrecognized->complete);
    EXPECT_TRUE(unrecognized->contents.empty());
    EXPECT_FALSE(unrecognized->warnings.empty());
}

TEST(DeviceContentInspection, DetectsAnOverlappingPartitionThatStartsOutsideTheSelectedTarget) {
    std::vector<std::byte> bytes(2U * 1024U * 1024U, std::byte{});
    bytes[510] = std::byte{0x55};
    bytes[511] = std::byte{0xaa};
    bytes[0x1c2] = std::byte{7};
    little32(bytes, 0x1c6, 1024U);
    little32(bytes, 0x1ca, 2048U);
    axk::MemoryReader reader(std::move(bytes));
    const axk::app::DeviceTargetDescription slice{"0", "SCSI 0", 1024U * 1024U, 512U * 1024U, 512U, false, {}};
    const auto inspected = axk::app::device_detail::inspect_contents(reader, slice);
    ASSERT_TRUE(inspected);
    EXPECT_TRUE(inspected->crosses_target);
}

TEST(DeviceContentInspection, CancellationAndInvalidBoundsCannotProduceSuccessfulUnknownResults) {
    axk::MemoryReader reader(std::vector<std::byte>(1024U * 1024U));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const axk::app::DeviceTargetDescription target{"disk", "Disk", 0U, reader.size(), 512U, false, {}};
    EXPECT_FALSE(axk::app::device_detail::inspect_contents(reader, target, cancellation.token()));
    auto invalid = target;
    invalid.offset = reader.size();
    EXPECT_FALSE(axk::app::device_detail::inspect_contents(reader, invalid));
}

TEST(DeviceContentInspection, MalformedAndOverlappingMbrEntriesBlockFormatting) {
    for (const auto second_start : {0U, 1025U, 8192U}) {
        std::vector<std::byte> bytes(2U * 1024U * 1024U);
        bytes[510] = std::byte{0x55};
        bytes[511] = std::byte{0xaa};
        bytes[0x1c2] = std::byte{7};
        little32(bytes, 0x1c6, 1024U);
        little32(bytes, 0x1ca, 2048U);
        bytes[0x1d2] = std::byte{7};
        little32(bytes, 0x1d6, second_start);
        little32(bytes, 0x1da, 1024U);
        axk::MemoryReader reader(std::move(bytes));
        const axk::app::DeviceTargetDescription whole{"disk", "Disk", 0U, reader.size(), 512U, false, {}};
        const auto inspected = axk::app::device_detail::inspect_contents(reader, whole);
        ASSERT_TRUE(inspected);
        EXPECT_FALSE(inspected->complete);
        EXPECT_FALSE(inspected->warnings.empty());
    }
}

TEST(DeviceContentInspection, BackupGptMetadataCannotBeFormattedThroughAnotherScsiSlice) {
    axk::MemoryReader reader(gpt_disk());
    const axk::app::DeviceTargetDescription whole{"disk", "Disk", 0U, reader.size(), 512U, false, {}};
    const auto carrier = axk::app::device_detail::inspect_contents(reader, whole);
    ASSERT_TRUE(carrier);
    EXPECT_TRUE(carrier->complete);
    const axk::app::DeviceTargetDescription slice{"1", "SCSI 1", 120U * 512U, 8U * 512U, 512U, false, {}};
    const auto inspected = axk::app::device_detail::inspect_contents(reader, slice);
    ASSERT_TRUE(inspected);
    EXPECT_TRUE(inspected->shared_partition_metadata);
    EXPECT_TRUE(std::ranges::any_of(inspected->contents, [](const auto &entry) { return entry.type == "GPT backup"; }));
}

TEST(DeviceContentInspection, AReadCompletingAfterTheDeadlineIsNotSuccessful) {
    axk::MemoryReader reader(std::vector<std::byte>(1024U * 1024U));
    const axk::app::DeviceTargetDescription whole{"disk", "Disk", 0U, reader.size(), 512U, false, {}};
    auto tick = std::chrono::steady_clock::time_point{};
    const auto inspected = axk::app::device_detail::inspect_contents(reader, whole, {}, [&] {
        const auto current = tick;
        tick += std::chrono::seconds{6};
        return current;
    });
    ASSERT_FALSE(inspected);
    EXPECT_EQ(inspected.error().code, "device_inspection_limit");
}
