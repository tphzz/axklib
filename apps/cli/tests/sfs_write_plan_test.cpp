#include "sfs_write_plan.hpp"

#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

namespace {
class SfsWritePlan : public testing::Test {
  protected:
    std::filesystem::path directory =
        std::filesystem::temp_directory_path() / ("axk-sfs-write-" + std::to_string(std::random_device{}()));
    std::filesystem::path image = directory / "target.img";
    void SetUp() override { std::filesystem::create_directory(directory); }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    void make_image(std::uint64_t bytes = 64U * 1024U * 1024U) {
        axk::VolumeSpec volume;
        volume.name = "Volume";
        axk::HdsBuildManifest manifest{"1.0", bytes, {{"Partition", {volume}}}};
        const auto built = axk::write_hds_image(manifest, image);
        ASSERT_TRUE(built) << built.error().message;
    }
    void byte(std::uint64_t offset, char value) {
        std::fstream out(image, std::ios::binary | std::ios::in | std::ios::out);
        out.seekp(static_cast<std::streamoff>(offset));
        out.put(value);
        ASSERT_TRUE(out);
    }
    void fill(std::uint64_t begin, std::uint64_t end, char value) {
        std::fstream out(image, std::ios::binary | std::ios::in | std::ios::out);
        out.seekp(static_cast<std::streamoff>(begin * 512U));
        const std::vector<char> bytes(64U * 1024U, value);
        auto remaining = (end - begin) * 512U;
        while (remaining != 0U) {
            const auto amount = std::min(remaining, static_cast<std::uint64_t>(bytes.size()));
            out.write(bytes.data(), static_cast<std::streamsize>(amount));
            ASSERT_TRUE(out);
            remaining -= amount;
        }
    }
};

TEST_F(SfsWritePlan, GuardsAllocationMetadataAndChoosesAUniformFreeCenter) {
    make_image();
    const auto plan = axk::test::sfs_write::prepare(image);
    ASSERT_TRUE(plan) << plan.error().message;
    EXPECT_EQ(plan->original.size(), 512U);
    EXPECT_EQ(plan->lba, plan->free_begin + (plan->free_end - plan->free_begin) / 2U);
    EXPECT_GT(plan->lba, axk::test::sfs_write::exclusion_sectors);
    EXPECT_LT(plan->lba, plan->sector_count - axk::test::sfs_write::exclusion_sectors);
    EXPECT_FALSE(plan->guards.empty());
    EXPECT_EQ(plan->guards.front().lba, 0U);
    for (const auto &guard : plan->guards)
        EXPECT_TRUE(plan->lba < guard.lba || plan->lba - guard.lba >= guard.bytes.size() / 512U);
    const auto saved = axk::test::sfs_write::save(directory / "write.plan", *plan);
    ASSERT_TRUE(saved) << saved.error().message;
    EXPECT_FALSE(axk::test::sfs_write::save(directory / "write.plan", *plan));
    // Leftover bytes at the midpoint must not authorize writing that sector.
    byte(plan->lba * 512U + 1U, 'x');
    const auto alternate = axk::test::sfs_write::prepare(image);
    ASSERT_TRUE(alternate) << alternate.error().message;
    EXPECT_EQ(alternate->lba, plan->lba - 1U);
    EXPECT_TRUE(std::ranges::all_of(alternate->original, [](auto value) { return value == std::byte{0}; }));
    EXPECT_EQ(alternate->free_begin, plan->free_begin);
    EXPECT_EQ(alternate->free_end, plan->free_end);
}

TEST_F(SfsWritePlan, FindsAnEmptySectorAboveANonemptyMidpointAndLowerNeighbor) {
    make_image();
    const auto initial = axk::test::sfs_write::prepare(image);
    ASSERT_TRUE(initial) << initial.error().message;
    byte(initial->lba * 512U + 511U, 'x');
    byte((initial->lba - 1U) * 512U, 'x');
    const auto selected = axk::test::sfs_write::prepare(image);
    ASSERT_TRUE(selected) << selected.error().message;
    EXPECT_EQ(selected->lba, initial->lba + 1U);
}

TEST_F(SfsWritePlan, FindsAUniformFfSectorButRejectsUnclearedFreeSpace) {
    make_image();
    const auto initial = axk::test::sfs_write::prepare(image);
    ASSERT_TRUE(initial) << initial.error().message;
    fill(initial->free_begin, initial->free_end, 'x');
    EXPECT_FALSE(axk::test::sfs_write::prepare(image));
    fill(initial->lba + 2U, initial->lba + 3U, '\xff');
    const auto selected = axk::test::sfs_write::prepare(image);
    ASSERT_TRUE(selected) << selected.error().message;
    EXPECT_EQ(selected->lba, initial->lba + 2U);
    EXPECT_TRUE(std::ranges::all_of(selected->original, [](auto value) { return value == std::byte{0xff}; }));
}

TEST_F(SfsWritePlan, DoesNotSearchOutsideTheBoundedFreeSpaceWindow) {
    make_image(128U * 1024U * 1024U);
    const auto initial = axk::test::sfs_write::prepare(image);
    ASSERT_TRUE(initial) << initial.error().message;
    const auto begin = initial->lba - axk::test::sfs_write::maximum_search_sectors / 2U;
    const auto end = begin + axk::test::sfs_write::maximum_search_sectors;
    ASSERT_GT(begin, initial->free_begin);
    ASSERT_LT(end, initial->free_end);
    fill(begin, end, 'x');
    const auto refused = axk::test::sfs_write::prepare(image);
    ASSERT_FALSE(refused);
    EXPECT_NE(refused.error().message.find("search is limited to 64 MiB"), std::string::npos);
}

TEST_F(SfsWritePlan, RejectsNoSafeSpaceAndBackupCopyMismatch) {
    make_image(16U * 1024U * 1024U);
    EXPECT_FALSE(axk::test::sfs_write::prepare(image));
    std::filesystem::remove(image);
    make_image();
    byte(512U, 'x');
    EXPECT_FALSE(axk::test::sfs_write::prepare(image));
}

TEST_F(SfsWritePlan, RejectsStaleOrInconsistentAllocationCopies) {
    make_image();
    const auto container = axk::open_image(image);
    ASSERT_TRUE(container);
    const auto &partition = container->partitions().front();
    const std::uint64_t second =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(partition.bitmap_copy2_cluster) * partition.sectors_per_cluster) *
        512U;
    byte(second + 100U, '\x01');
    EXPECT_FALSE(axk::test::sfs_write::prepare(image));
    const std::uint64_t first =
        (static_cast<std::uint64_t>(partition.start_sector) +
         static_cast<std::uint64_t>(partition.bitmap_copy1_cluster) * partition.sectors_per_cluster) *
        512U;
    byte(first + 100U, '\x01'); // matching copies still disagree with reconstructed ownership
    EXPECT_FALSE(axk::test::sfs_write::prepare(image));
}

TEST_F(SfsWritePlan, RejectsNonFilesAndTruncatedTargets) {
    EXPECT_FALSE(axk::test::sfs_write::prepare(directory));
    make_image();
    std::filesystem::resize_file(image, std::filesystem::file_size(image) - 512U);
    EXPECT_FALSE(axk::test::sfs_write::prepare(image));
}
} // namespace
