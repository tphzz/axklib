#include <array>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/floppy_import.hpp"
#include "media_test_fixtures.hpp"

namespace {
class MutableReader final : public axk::RandomAccessReader {
  public:
    std::vector<std::byte> bytes = fat_fixture();
    mutable std::size_t read_count{};
    std::uint64_t size() const noexcept override { return bytes.size(); }
    axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
        ++read_count;
        if (offset > bytes.size() || destination.size() > bytes.size() - offset)
            return std::unexpected(
                axk::make_error(axk::ErrorCode::io_short_read, axk::ErrorCategory::io, "Read exceeds test source."));
        std::ranges::copy(std::span{bytes}.subspan(static_cast<std::size_t>(offset), destination.size()),
                          destination.begin());
        return {};
    }
};

axk::Result<axk::FloppyImportSource> source(std::vector<std::byte> bytes) {
    auto fat = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes)), "source.img");
    if (!fat)
        return std::unexpected(fat.error());
    std::vector<axk::FatImage> members;
    members.push_back(std::move(*fat));
    return axk::FloppyImportSource::open(std::move(members));
}
} // namespace

TEST(FloppyImportTest, InspectsAndPreparesSelectedWaveDataWithoutAnArchive) {
    const auto opened = source(fat_fixture());
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &inspection = opened->inspection();
    EXPECT_TRUE(inspection.complete);
    ASSERT_EQ(inspection.objects.size(), 1U);
    EXPECT_EQ(inspection.objects[0].name, "TEST");
    EXPECT_TRUE(inspection.objects[0].exclusion_reason.empty());
    const auto prepared = opened->prepare(std::array{inspection.objects[0].key});
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_EQ(prepared->nodes.size(), 1U);
    EXPECT_EQ(prepared->nodes[0].raw_payload, smpl_object());
}

TEST(FloppyImportTest, RejectsEmptyUnknownDuplicateAndCancelledSelections) {
    const auto opened = source(fat_fixture());
    ASSERT_TRUE(opened);
    const auto key = opened->inspection().objects[0].key;
    EXPECT_FALSE(opened->prepare({}));
    EXPECT_FALSE(opened->prepare(std::array{std::string{"missing"}}));
    EXPECT_FALSE(opened->prepare(std::array{key, key}));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(opened->prepare(std::array{key}, cancellation.token()));
}

TEST(FloppyImportTest, LeavesConfigurationAndUnknownFilesOutsideTheObjectGraph) {
    auto bytes = fat_fixture();
    constexpr auto root = 3U * 512U;
    ascii(bytes, root + 32U, "SYSTEM2 ");
    ascii(bytes, root + 40U, "002");
    bytes[root + 43U] = std::byte{0x20};
    const auto opened = source(std::move(bytes));
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &inspection = opened->inspection();
    ASSERT_EQ(inspection.objects.size(), 1U);
    ASSERT_EQ(inspection.excluded_files.size(), 1U);
    EXPECT_EQ(inspection.excluded_files[0].path, "SYSTEM2.002");
    const auto prepared = opened->prepare(std::array{inspection.objects[0].key});
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_EQ(prepared->nodes.size(), 1U);
}

TEST(FloppyImportTest, RejectsUnrelatedDisksAndNonASeriesContents) {
    std::vector<axk::FatImage> members;
    for (const auto *name : {"one.img", "two.img"}) {
        auto fat = axk::FatImage::open(std::make_shared<axk::MemoryReader>(fat_fixture()), name);
        ASSERT_TRUE(fat);
        members.push_back(std::move(*fat));
    }
    EXPECT_FALSE(axk::FloppyImportSource::open(std::move(members)));
    auto bytes = fat_fixture();
    std::fill_n(bytes.begin() + 4U * 512U, 16U, std::byte{});
    EXPECT_FALSE(source(std::move(bytes)));
    EXPECT_FALSE(axk::FloppyImportSource::open({}));
}

TEST(FloppyImportTest, ReviewSelectionUsesTheRetainedPayloadSnapshot) {
    auto reader = std::make_shared<MutableReader>();
    auto fat = axk::FatImage::open(reader, "source.img");
    ASSERT_TRUE(fat);
    auto opened = axk::FloppyImportSource::open({std::move(*fat)});
    ASSERT_TRUE(opened) << opened.error().message;
    const auto reads = reader->read_count;
    std::ranges::fill(reader->bytes, std::byte{0xff});
    auto prepared = opened->prepare(std::array{opened->inspection().objects[0].key});
    ASSERT_TRUE(prepared) << prepared.error().message;
    EXPECT_EQ(prepared->nodes[0].raw_payload, smpl_object());
    EXPECT_EQ(reader->read_count, reads);
}

TEST(FloppyImportTest, KeepsAnUnsupportedObjectVisibleButNotSelectable) {
    auto bytes = fat_fixture_with_invalid_then_valid_waveform();
    be32(bytes, 4U * 512U + 0x10U, 0xacU);
    bytes[4U * 512U + 0x84U] = std::byte{0x31};
    const auto opened = source(std::move(bytes));
    ASSERT_TRUE(opened) << opened.error().message;
    const auto &objects = opened->inspection().objects;
    ASSERT_EQ(objects.size(), 2U);
    const auto valid = std::ranges::find(objects, std::string{"VALID"}, &axk::FloppyImportObject::name);
    ASSERT_NE(valid, objects.end());
    EXPECT_TRUE(valid->exclusion_reason.empty());
    EXPECT_TRUE(opened->prepare(std::array{valid->key}));
    const auto invalid =
        std::ranges::find_if(objects, [](const auto &object) { return !object.exclusion_reason.empty(); });
    ASSERT_NE(invalid, objects.end());
    EXPECT_FALSE(opened->prepare(std::array{invalid->key}));
    EXPECT_FALSE(opened->prepare(std::array{valid->key, invalid->key}));
}

TEST(FloppyImportTest, RejectsCrosslinkedLogicalPayload) {
    auto bytes = fat_fixture();
    for (std::uint16_t cluster = 2; cluster < 10; ++cluster)
        for (const auto fat_offset : {512U, 1024U})
            set_fat12(std::span{bytes}.subspan(fat_offset, 512), cluster,
                      cluster == 9 ? 0xfff : static_cast<std::uint16_t>(cluster + 1));
    for (std::size_t i = 0; i < 16; ++i) {
        const auto entry = 3U * 512U + i * 32U;
        ascii(bytes, entry, "SMPTEST ");
        ascii(bytes, entry + 8U, "004");
        bytes[entry + 6U] = static_cast<std::byte>('A' + i);
        bytes[entry + 0x0bU] = std::byte{0x20};
        le16(bytes, entry + 0x1aU, 2);
        le32(bytes, entry + 0x1cU, 4096);
    }
    const auto opened = source(std::move(bytes));
    ASSERT_FALSE(opened);
}
