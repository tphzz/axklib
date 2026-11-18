#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/filesystems/fat/floppy_writer.hpp"
#include "axklib/media.hpp"

namespace {
using axk::filesystems::build_floppy;
using axk::filesystems::FloppyFile;
constexpr auto dd = axk::filesystems::FloppyLayout::dd_720k;

TEST(FormatGeneralization, W2) {
    std::vector<std::byte> first(1025, std::byte{0x42});
    first.back() = std::byte{0xda};
    const std::vector<std::byte> second{std::byte{0xde}, std::byte{0xad}};
    const std::array files{FloppyFile{"FIRST.W01", std::span{first}}, FloppyFile{"SECOND.W02", std::span{second}}};
    auto image = build_floppy(dd, files);
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->size(), std::size_t{737280});
    const std::span bytes{*image};
    constexpr std::array<std::uint8_t, 17> bpb{0, 2, 2, 1, 0, 2, 112, 0, 0xa0, 5, 0xf9, 3, 0, 9, 0, 2, 0};
    EXPECT_TRUE(std::ranges::equal(bytes.subspan(11, bpb.size()), std::as_bytes(std::span{bpb})));
    const auto fat = bytes.subspan(512, 1536);
    EXPECT_TRUE(std::ranges::equal(fat, bytes.subspan(2048, 1536)));
    EXPECT_EQ(fat[0], std::byte{0xf9});
    EXPECT_EQ(bytes[3584 + 11], std::byte{});
    EXPECT_EQ(bytes[3584 + 32 + 11], std::byte{});
    EXPECT_EQ(bytes[3584 + 64], std::byte{});
    const auto parsed = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(*image)), "result.img");
    ASSERT_TRUE(parsed) << parsed.error().message;
    ASSERT_FALSE(parsed->yamaha_catalog());
    ASSERT_EQ(parsed->files().size(), std::size_t{2});
    EXPECT_EQ(parsed->files()[0].path, "FIRST.W01");
    EXPECT_EQ(parsed->files()[1].path, "SECOND.W02");
    EXPECT_EQ(parsed->files()[0].first_cluster, 2U);
    EXPECT_EQ(parsed->files()[1].first_cluster, 4U);
    const auto a = parsed->read_file(parsed->files()[0]);
    const auto b = parsed->read_file(parsed->files()[1]);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(*a, first);
    EXPECT_EQ(*b, second);
}

TEST(FatFloppyWriter, SeparatesCapacityInClustersFromDirectoryEntryLimits) {
    const std::vector<std::byte> full(713 * 1024, std::byte{0x4e});
    std::vector<FloppyFile> files{{"FULL.BIN", std::span{full}}};
    ASSERT_TRUE(build_floppy(dd, files));
    files.push_back({"ONE.BIN", std::span{full}.first(1)});
    EXPECT_FALSE(build_floppy(dd, files));
    files.clear();
    for (int i = 0; i < 112; ++i)
        files.push_back({std::format("F{:03}.BIN", i), std::span<const std::byte>{}});
    ASSERT_TRUE(build_floppy(dd, files));
    files.push_back({"ONE.BIN", std::span<const std::byte>{}});
    EXPECT_FALSE(build_floppy(dd, files));
}

class CancellingReader final : public axk::RandomAccessReader {
  public:
    explicit CancellingReader(axk::CancellationSource &source) : source_(source) {}
    std::uint64_t size() const noexcept override { return 1U; }
    axk::Result<void> read_exact_at(std::uint64_t, std::span<std::byte> bytes) const override {
        std::ranges::fill(bytes, std::byte{0x7f});
        source_.cancel();
        return {};
    }

  private:
    axk::CancellationSource &source_;
};

TEST(FatFloppyWriter, CanWriteAgainImmediatelyAfterMidWriteCancellation) {
    axk::CancellationSource source;
    const CancellingReader reader(source);
    const std::array first{std::byte{0xcf}};
    const std::array files{FloppyFile{"FIRST.W01", std::span{first}}, FloppyFile{"LAST.W02", std::cref(reader)}};
    const auto cancelled = build_floppy(dd, files, source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_TRUE(build_floppy(dd, std::span{files}.first(1)));
}

TEST(FatFloppyWriter, RejectsNamesThatCannotIdentifyOneRootFile) {
    const auto empty = std::span<const std::byte>{};
    for (const std::string name : {"../A.W01", "A/B.W01", "A\\B.W01", "0:A.W01", "A.W001", ".", "A B.W01", "A.W01 Z"}) {
        const std::array files{FloppyFile{name, empty}};
        EXPECT_FALSE(build_floppy(dd, files)) << name;
    }
    const std::array alias{FloppyFile{"A.W01", empty}, FloppyFile{"a.w01", empty}};
    EXPECT_FALSE(build_floppy(dd, alias));
    const std::array nul{FloppyFile{std::string{"A.W01\0Z", 7}, empty}};
    EXPECT_FALSE(build_floppy(dd, nul));
}

TEST(FatFloppyWriter, RejectsAnOversizedReaderBeforeReadingItsPayload) {
    class OversizedReader final : public axk::RandomAccessReader {
      public:
        std::uint64_t size() const noexcept override { return std::numeric_limits<std::uint64_t>::max(); }
        axk::Result<void> read_exact_at(std::uint64_t, std::span<std::byte>) const override {
            ADD_FAILURE() << "Capacity admission must precede reading";
            return {};
        }
    };
    const OversizedReader reader;
    const std::array files{FloppyFile{"HUGE.W01", std::cref(reader)}};
    const auto result = build_floppy(dd, files);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().category, axk::ErrorCategory::allocation);
}
} // namespace
