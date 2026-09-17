#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "../../../library/tests/media_ex5_fixture.hpp"
#include "axklib/application/filesystem_export.hpp"
#include "content_digest.hpp"

namespace {
class FilesystemExportProfile : public testing::TestWithParam<std::string> {};

TEST_P(FilesystemExportProfile, ExportsFragmentedRawBytesThroughTheProfileLocator) {
    auto bytes = ex5_fixture();
    const auto profile = GetParam();
    if (profile != "ex5-hd") {
        bytes.erase(bytes.begin(), bytes.begin() + boot_offset);
        ascii(bytes, 3U, profile == "ex5-mo" ? "YAMAHA??" : "MSDOS5.0");
        le16(bytes, 19U, 0U);
        le32(bytes, 32U, static_cast<std::uint32_t>(bytes.size() / sector_bytes));
    }
    if (profile == "mbr") {
        const auto volume = std::move(bytes);
        constexpr std::size_t start = 63U * sector_bytes;
        bytes.resize(start + 2U * volume.size());
        bytes[510U] = std::byte{0x55};
        bytes[511U] = std::byte{0xaa};
        for (std::size_t part = 0U; part < 2U; ++part) {
            const auto offset = start + part * volume.size();
            const auto slot = part * 2U;
            bytes[446U + slot * 16U + 4U] = std::byte{6};
            le32(bytes, 446U + slot * 16U + 8U, static_cast<std::uint32_t>(offset / sector_bytes));
            le32(bytes, 446U + slot * 16U + 12U, static_cast<std::uint32_t>(volume.size() / sector_bytes));
            std::ranges::copy(volume, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
            bytes[offset + data_offset - boot_offset + sector_bytes] = static_cast<std::byte>(part + 1U);
        }
    }
    const auto root =
        std::filesystem::temp_directory_path() /
        std::format("axk-files-export-profile-{}", std::chrono::steady_clock::now().time_since_epoch().count());
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } cleanup{root};
    std::filesystem::create_directories(root);
    {
        std::ofstream output{root / "source.hda", std::ios::binary};
        output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output);
    }
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root, true}});
    ASSERT_TRUE(sandbox);
    axk::app::PathReservationCoordinator reservations;
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "source.hda"}, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    {
        const auto read = sessions.begin_read(opened->image_id, "owner", 1U);
        ASSERT_TRUE(read);
        if (profile != "mbr") {
            const auto *fat = std::get_if<axk::FatImage>(&read->media->storage());
            ASSERT_NE(fat, nullptr);
            const auto expected = profile == "ex5-hd"   ? axk::FatProfile::ex5_disk
                                  : profile == "ex5-mo" ? axk::FatProfile::ex5_removable
                                                        : axk::FatProfile::fat16;
            EXPECT_EQ(fat->geometry().profile, expected);
        }
    }
    const auto roots = sessions.filesystem(opened->image_id, "owner", 1U);
    ASSERT_TRUE(roots);
    ASSERT_EQ(roots->items.size(), profile == "mbr" ? 2U : 1U);
    std::vector<std::string> selected;
    for (const auto &entry : roots->items)
        selected.push_back(entry.id);
    const auto result = axk::app::export_filesystem_entries(sessions, *sandbox, reservations, opened->image_id, "owner",
                                                            1U, selected, {"workspace", "out"});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->total_bytes, profile == "mbr" ? 1400U : 700U);
    std::size_t files{};
    for (const auto &entry : result->entries) {
        if (entry.directory)
            continue;
        auto path = root / "out";
        for (const auto &component : entry.relative_path)
            path /= component;
        const auto reader = axk::FileReader::open(path);
        ASSERT_TRUE(reader);
        std::vector<std::byte> content(700U);
        ASSERT_TRUE((*reader)->read_exact_at(0U, content));
        EXPECT_TRUE(std::ranges::all_of(std::span{content}.subspan(1U, 511U),
                                        [](auto value) { return value == std::byte{0x31}; }));
        EXPECT_TRUE(
            std::ranges::all_of(std::span{content}.subspan(512U), [](auto value) { return value == std::byte{0x72}; }));
        EXPECT_EQ(content.front(), profile == "mbr" ? static_cast<std::byte>(++files) : std::byte{0x31});
    }
    const auto before = axk::app::detail::reader_sha256(axk::MemoryReader{std::move(bytes)});
    const auto after = axk::app::detail::file_sha256(root / "source.hda");
    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    EXPECT_EQ(*before, *after);
}

INSTANTIATE_TEST_SUITE_P(Filesystems, FilesystemExportProfile, testing::Values("ex5-hd", "ex5-mo", "fat16", "mbr"));
} // namespace
