#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <tuple>

#include <gtest/gtest.h>

#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

class HdsPartitionNames : public testing::TestWithParam<std::tuple<std::uint8_t, bool>> {
  protected:
    void SetUp() override {
        const auto [count, custom] = GetParam();
        root = std::filesystem::temp_directory_path() /
               ("axklib-partition-names-" + std::to_string(count) + (custom ? "-custom" : "-default"));
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::filesystem::path root;
};

TEST_P(HdsPartitionNames, PreservesRequestedNamesInBothHeadersAndOnReopen) {
    const auto [count, custom] = GetParam();
    const auto planned = axk::plan_hds_creation({axk::HdsCreationProfileId::hds_128_mib, count});
    ASSERT_TRUE(planned) << planned.error().message;
    auto manifest = planned->manifest;
    // Exercise the creation plan's names with small, disposable partition slots.
    manifest.size_bytes = 8U * 1024U * 1024U;
    const std::array<std::string, 8> names{"My Sounds", "ABCDEFGHIJKLMNOP", "Piano 3", "PARTITION 4    3", "Bass",
                                           "Pads",      "Drums 7",          "Mix 8"};
    if (custom) {
        for (std::size_t index = 0; index < manifest.partitions.size(); ++index)
            manifest.partitions[index].name = names[index];
    }
    const auto path = root / "names.hds";
    const auto written = axk::write_hds_image(manifest, path);
    ASSERT_TRUE(written) << written.error().message;
    const auto reopened = axk::open_image(path);
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_EQ(reopened->partitions().size(), count);
    std::ifstream raw{path, std::ios::binary};
    ASSERT_TRUE(raw);
    for (std::size_t index = 0; index < manifest.partitions.size(); ++index) {
        SCOPED_TRACE(index);
        const auto &expected = manifest.partitions[index].name;
        const auto &partition = reopened->partitions()[index];
        EXPECT_EQ(partition.name, expected);
        EXPECT_TRUE(partition.backup_header_matches);
        EXPECT_EQ(written->partitions[index].name, expected);
        const auto offset = static_cast<std::uint64_t>(partition.start_sector) * 512U;
        for (const auto header_offset : {offset, offset + 1024U}) {
            std::array<char, 16> stored{};
            raw.seekg(static_cast<std::streamoff>(header_offset + 0x40U));
            raw.read(stored.data(), static_cast<std::streamsize>(stored.size()));
            ASSERT_EQ(raw.gcount(), static_cast<std::streamsize>(stored.size()));
            EXPECT_EQ(std::string(stored.data(), stored.size()), expected + std::string(16U - expected.size(), ' '));
        }
    }
}

INSTANTIATE_TEST_SUITE_P(PartitionCounts, HdsPartitionNames,
                         testing::Combine(testing::Values(std::uint8_t{1}, std::uint8_t{2}, std::uint8_t{3},
                                                          std::uint8_t{8}),
                                          testing::Bool()));

} // namespace
