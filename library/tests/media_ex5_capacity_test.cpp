#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/filesystem_transaction.hpp"
#include "axklib/media.hpp"
#include "media_ex5_fixture.hpp"

namespace {
constexpr std::uint32_t clusters = 4096U;
constexpr std::size_t cluster_bytes = 4U * sector_bytes;
constexpr std::size_t table_bytes = 17U * sector_bytes;
constexpr std::size_t root = sector_bytes + 2U * table_bytes;
std::vector<std::byte> capacity_fixture(bool unused_tail = false) { return ex5_capacity_fixture(unused_tail); }

void set_entry(std::vector<std::byte> &bytes, std::uint16_t cluster, std::uint16_t value) {
    for (const auto offset : {sector_bytes, sector_bytes + table_bytes})
        le16(bytes, offset + static_cast<std::size_t>(cluster) * 2U, value);
}

std::shared_ptr<axk::MemoryReader> payload(std::size_t size) {
    return std::make_shared<axk::MemoryReader>(std::vector<std::byte>(size, std::byte{0x6a}));
}

TEST(Ex5Capacity, OpensBothOneSectorMismatchLayoutsWithWarnings) {
    for (const bool unused_tail : {false, true}) {
        const auto source = std::make_shared<axk::MemoryReader>(capacity_fixture(unused_tail));
        const auto image = axk::FatImage::open(source);
        ASSERT_TRUE(image) << image.error().message;
        EXPECT_EQ(image->geometry().profile, axk::FatProfile::ex5_removable);
        EXPECT_EQ(image->geometry().data_cluster_count, clusters);
        EXPECT_EQ(image->geometry().physical_size_bytes, source->size());
        EXPECT_EQ(image->geometry().backed_data_cluster_count, unused_tail ? clusters : clusters - 1U);
        ASSERT_EQ(image->validation_issues().size(), 1U);
        EXPECT_EQ(image->validation_issues()[0].code, "EX5_CAPACITY_EXCEEDS_IMAGE");
    }
}

TEST(Ex5Capacity, DoesNotRelaxOtherGeometryOrTruncationChecks) {
    for (unsigned variant = 0; variant < 7U; ++variant) {
        auto bytes = capacity_fixture();
        switch (variant) {
        case 0:
            ascii(bytes, 3U, "MSDOS5.0");
            break;
        case 1:
            bytes.resize(bytes.size() - sector_bytes);
            break;
        case 2:
            bytes.pop_back();
            break;
        case 3:
            le16(bytes, 14U, 2U);
            break;
        case 4:
            le16(bytes, 17U, 16U);
            break;
        case 5:
            bytes[510U] = std::byte{};
            break;
        case 6:
            bytes[sector_bytes + table_bytes + 4U] = std::byte{1};
            break;
        }
        EXPECT_FALSE(axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes)))) << variant;
    }
}

TEST(Ex5Capacity, ReadsOnlyExistingLogicalPayloadAndReportsMissingFileBytes) {
    for (const bool missing_payload : {false, true}) {
        auto bytes = capacity_fixture();
        set_entry(bytes, clusters + 1U, 0xffffU);
        ascii(bytes, root, "TAIL    BIN");
        bytes[root + 11U] = std::byte{0x20};
        le16(bytes, root + 26U, clusters + 1U);
        const auto size = static_cast<std::uint32_t>(cluster_bytes - sector_bytes + (missing_payload ? 1U : 0U));
        le32(bytes, root + 28U, size);
        const auto source = std::make_shared<axk::MemoryReader>(std::move(bytes));
        const auto image = axk::FatImage::open(source);
        ASSERT_TRUE(image) << image.error().message;
        ASSERT_EQ(image->files().size(), 1U);
        EXPECT_EQ(image->validation_issues().size(), missing_payload ? 2U : 1U);
        EXPECT_TRUE(image->read_file_prefix(image->files()[0], size - 1U));
        const auto complete = image->read_file(image->files()[0]);
        EXPECT_EQ(complete.has_value(), !missing_payload);
        if (missing_payload) {
            EXPECT_EQ(image->validation_issues()[1].code, "EX5_FILE_DATA_UNAVAILABLE");
            EXPECT_NE(complete.error().message.find("TAIL.BIN"), std::string::npos);
        }
        const std::vector<axk::FilesystemEdit> edits{
            axk::PutFilesystemFile{{"TAIL.BIN"}, payload(23U), axk::FileConflict::replace}};
        const auto replacement = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
        ASSERT_TRUE(replacement) << replacement.error().message;
        const auto replaced = axk::FatImage::open(replacement->preview);
        ASSERT_TRUE(replaced);
        EXPECT_EQ(replaced->validation_issues().size(), 1U);
        EXPECT_EQ(replaced->read_file(replaced->files()[0]).value(), std::vector<std::byte>(23U, std::byte{0x6a}));
    }
}

TEST(Ex5Capacity, MissingDirectoryMetadataStillBlocksOpening) {
    auto bytes = capacity_fixture();
    set_entry(bytes, clusters + 1U, 0xffffU);
    ascii(bytes, root, "TAIL       ");
    bytes[root + 11U] = std::byte{0x10};
    le16(bytes, root + 26U, clusters + 1U);
    EXPECT_FALSE(axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes))));
}

TEST(Ex5Capacity, FullAndPartialWritesCanUseTheLastCompleteCluster) {
    for (const bool unused_tail : {false, true}) {
        for (const auto size : {std::size_t{1}, cluster_bytes}) {
            auto bytes = capacity_fixture(unused_tail);
            const auto last = static_cast<std::uint16_t>(unused_tail ? clusters + 1U : clusters);
            for (std::uint16_t cluster = 2U; cluster < last; ++cluster)
                set_entry(bytes, cluster, 0xffffU);
            const auto source = std::make_shared<axk::MemoryReader>(std::move(bytes));
            const std::vector<axk::FilesystemEdit> edits{axk::PutFilesystemFile{{"LAST.BIN"}, payload(size)}};
            const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
            ASSERT_TRUE(plan) << plan.error().message;
            const auto image = axk::FatImage::open(plan->preview);
            ASSERT_TRUE(image);
            ASSERT_EQ(image->files().size(), 1U);
            EXPECT_EQ(image->files()[0].first_cluster, last);
            EXPECT_EQ(image->read_file(image->files()[0]).value(), std::vector<std::byte>(size, std::byte{0x6a}));
        }
    }
}

TEST(Ex5Capacity, DirectoryGrowthCannotAllocateAnIncompleteCluster) {
    auto bytes = capacity_fixture();
    for (std::uint16_t cluster = 3U; cluster < clusters + 1U; ++cluster)
        set_entry(bytes, cluster, 0xffffU);
    const auto source = std::make_shared<axk::MemoryReader>(std::move(bytes));
    std::vector<axk::FilesystemEdit> edits{axk::CreateFilesystemDirectory{{"NEW"}}};
    for (unsigned i = 0; i < 63U; ++i)
        edits.emplace_back(axk::PutFilesystemFile{{"NEW", "F" + std::to_string(i)}, payload(0U)});
    const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
    ASSERT_FALSE(plan);
    EXPECT_NE(plan.error().message.find("incomplete"), std::string::npos);
    const auto original = axk::FatImage::open(source);
    ASSERT_TRUE(original);
    EXPECT_TRUE(original->directories().empty());
}

TEST(Ex5Capacity, SafeEditsKeepTheDeclaredGeometryAndSourceLength) {
    const auto bytes = capacity_fixture();
    const auto source = std::make_shared<axk::MemoryReader>(bytes);
    const std::vector<axk::FilesystemEdit> edits{
        axk::CreateFilesystemDirectory{{"NEW"}},
        axk::PutFilesystemFile{{"NEW", "DATA.BIN"}, payload(2100U)},
    };
    const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(plan->preview->size(), source->size());
    std::vector<std::byte> boot(sector_bytes);
    ASSERT_TRUE(plan->preview->read_exact_at(0U, boot));
    EXPECT_TRUE(std::equal(boot.begin(), boot.end(), bytes.begin()));
    const auto image = axk::FatImage::open(plan->preview);
    ASSERT_TRUE(image) << image.error().message;
    ASSERT_EQ(image->files().size(), 1U);
    EXPECT_EQ(image->read_file(image->files()[0]).value(), std::vector<std::byte>(2100U, std::byte{0x6a}));
    EXPECT_EQ(image->validation_issues().size(), 1U);
    for (const auto &patch : plan->patches)
        EXPECT_LE(patch.offset + patch.size, source->size());
}

TEST(Ex5Capacity, RejectsAllocationWhenOnlyTheIncompleteClusterIsFree) {
    auto bytes = capacity_fixture();
    for (std::uint16_t cluster = 2U; cluster < clusters + 1U; ++cluster)
        set_entry(bytes, cluster, 0xffffU);
    const auto source = std::make_shared<axk::MemoryReader>(bytes);
    const std::vector<axk::FilesystemEdit> edits{axk::PutFilesystemFile{{"NEW.BIN"}, payload(1U)}};
    const auto plan = axk::detail::prepare_fat_file_edits(source, axk::PartitionIndex{0}, edits);
    ASSERT_FALSE(plan);
    EXPECT_NE(plan.error().message.find("incomplete"), std::string::npos) << plan.error().message;
    std::vector<std::byte> unchanged(bytes.size());
    ASSERT_TRUE(source->read_exact_at(0U, unchanged));
    EXPECT_EQ(unchanged, bytes);
}
} // namespace
