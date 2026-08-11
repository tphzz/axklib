#include <barrier>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/filesystem.hpp"
#include "download_reader.hpp"

namespace {

TEST(DownloadReader, RejectsAFileChangedBetweenOpenAndRead) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-download-reader-race";
    const auto path = root / "source.bin";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    std::ofstream{path, std::ios::binary} << "original";
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    const auto opened = sandbox->open_file({"root", "source.bin"});
    ASSERT_TRUE(opened) << opened.error().message;
    std::barrier read_opened{2};
    std::barrier mutation_complete{2};
    auto hooks = std::make_shared<axk::server::detail::DownloadReadHooks>();
    hooks->after_open_before_read = [&] {
        read_opened.arrive_and_wait();
        mutation_complete.arrive_and_wait();
    };
    std::optional<axk::app::Result<std::vector<std::byte>>> result;
    std::thread reader{[&] { result = axk::server::detail::read_verified_download(*opened, 0U, opened->size, hooks); }};

    read_opened.arrive_and_wait();
    std::ofstream{path, std::ios::binary | std::ios::trunc} << "replacement-data";
    mutation_complete.arrive_and_wait();
    reader.join();

    ASSERT_TRUE(result.has_value());
    ASSERT_FALSE(*result);
    EXPECT_EQ(result->error().code, "archive_source_changed");
    std::filesystem::remove_all(root, cleanup_error);
}

TEST(DownloadReader, ReturnsAStableBoundedRange) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-download-reader-stable";
    const auto path = root / "source.bin";
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    std::filesystem::create_directories(root);
    std::ofstream{path, std::ios::binary} << "original";
    auto sandbox = axk::app::Sandbox::create({{"root", "Root", root, true}});
    ASSERT_TRUE(sandbox) << sandbox.error().message;
    const auto opened = sandbox->open_file({"root", "source.bin"});
    ASSERT_TRUE(opened) << opened.error().message;

    const auto result = axk::server::detail::read_verified_download(*opened, 2U, 4U);

    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(result->data()), result->size()), "igin");
    std::filesystem::remove_all(root, cleanup_error);
}

} // namespace
