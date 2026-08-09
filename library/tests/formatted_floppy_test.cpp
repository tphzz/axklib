#include <algorithm>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/writer.hpp"

namespace {

std::string file_digest(const std::filesystem::path &path) {
    const auto reader = axk::FileReader::open(path);
    EXPECT_TRUE(reader) << reader.error().message;
    if (!reader)
        return {};
    const auto digest = axk::package_internal::sha256_reader(**reader);
    EXPECT_TRUE(digest) << digest.error().message;
    return digest ? axk::package_internal::hex_digest(*digest) : std::string{};
}

void expect_blank_yamaha_floppy(const std::filesystem::path &path) {
    const auto image = axk::FatImage::open(path);
    ASSERT_TRUE(image) << image.error().message;
    EXPECT_EQ(image->geometry().bytes_per_sector, 512U);
    EXPECT_EQ(image->geometry().sectors_per_cluster, 1U);
    EXPECT_EQ(image->geometry().fat_count, 2U);
    EXPECT_EQ(image->geometry().root_entry_count, 224U);
    EXPECT_EQ(image->geometry().total_sectors, 2'880U);
    EXPECT_EQ(image->geometry().data_offset, 0x4200U);
    ASSERT_EQ(image->files().size(), 2U);
    const auto catalog_file = std::ranges::find(image->files(), "YAMAHA.SYM", &axk::FatFile::path);
    ASSERT_NE(catalog_file, image->files().end());
    EXPECT_EQ(catalog_file->first_cluster, 2U);
    EXPECT_EQ(catalog_file->size, 9'766U);
    const auto marker_file = std::ranges::find(image->files(), "A3000_SY.002", &axk::FatFile::path);
    ASSERT_NE(marker_file, image->files().end());
    EXPECT_EQ(marker_file->first_cluster, 0U);
    EXPECT_EQ(marker_file->size, 0U);
    ASSERT_TRUE(image->yamaha_catalog());
    EXPECT_EQ(image->yamaha_catalog()->disk_name, std::string(16U, ' '));
    ASSERT_EQ(image->yamaha_catalog()->files.size(), 1U);
    EXPECT_EQ(image->yamaha_catalog()->files[0].slot, 2U);
    EXPECT_EQ(image->yamaha_catalog()->files[0].logical_path, "\\A3000.SYM");
    ASSERT_EQ(image->yamaha_catalog()->categories.size(), 1U);
    EXPECT_EQ(image->yamaha_catalog()->categories[0], "\\OTHERS");
    EXPECT_TRUE(image->validation_issues().empty());
    const auto objects = image->objects();
    ASSERT_TRUE(objects) << objects.error().message;
    EXPECT_TRUE(objects->empty());
}

} // namespace

TEST(FormattedFloppy, ReproducesSamplerQuickAndFullFormatProfilesExactly) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-formatted-floppy-test";
    const auto quick_path = root / "quick.ima";
    const auto full_path = root / "full.ima";
    std::error_code error;
    std::filesystem::remove_all(root, error);

    const auto quick_plan = axk::plan_floppy_creation(axk::FloppyFormatMode::quick);
    ASSERT_TRUE(quick_plan) << quick_plan.error().message;
    EXPECT_EQ(quick_plan->size_bytes, axk::formatted_floppy_size_bytes);
    EXPECT_EQ(quick_plan->object_count, 0U);
    const auto full_plan = axk::plan_floppy_creation(axk::FloppyFormatMode::full);
    ASSERT_TRUE(full_plan) << full_plan.error().message;

    const auto quick = axk::write_formatted_floppy_image(*quick_plan, quick_path);
    ASSERT_TRUE(quick) << quick.error().message;
    EXPECT_EQ(quick->format, axk::MediaImageFormat::fat12_floppy);
    EXPECT_EQ(quick->size_bytes, axk::formatted_floppy_size_bytes);
    EXPECT_EQ(quick->object_count, 0U);
    EXPECT_EQ(file_digest(quick_path), "32c623fa775c010ff648fa4bdf7ece4b886799cab90b99b1a2fe54ff1aa14030");
    expect_blank_yamaha_floppy(quick_path);

    const auto full = axk::write_formatted_floppy_image(*full_plan, full_path);
    ASSERT_TRUE(full) << full.error().message;
    EXPECT_EQ(file_digest(full_path), "59225775ad66b28498940e34d318826eddd72642214a6655c71d825a5b9c4af5");
    expect_blank_yamaha_floppy(full_path);

    EXPECT_FALSE(axk::write_formatted_floppy_image(*full_plan, full_path));
    ASSERT_TRUE(axk::write_formatted_floppy_image(*quick_plan, full_path, true));
    EXPECT_EQ(file_digest(full_path), "32c623fa775c010ff648fa4bdf7ece4b886799cab90b99b1a2fe54ff1aa14030");

    std::filesystem::remove_all(root, error);
}

TEST(FormattedFloppy, RejectsUnknownModesAndHonorsCancellation) {
    EXPECT_FALSE(axk::plan_floppy_creation(static_cast<axk::FloppyFormatMode>(0xffU)));

    axk::CancellationSource source;
    source.cancel();
    const auto cancelled = axk::plan_floppy_creation(axk::FloppyFormatMode::full, source.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
}
