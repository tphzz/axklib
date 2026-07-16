#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <gtest/gtest.h>

#include "axklib/file_publication.hpp"

namespace {

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(FilePublication, AtomicallyReplacesAnExistingDestination) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-replace";
    const auto temporary = root / ".output.tmp";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::ofstream{temporary, std::ios::binary} << "replacement";
    std::ofstream{output, std::ios::binary} << "original";

    const auto published = axk::detail::publish_temporary_file(temporary, output, true);

    ASSERT_TRUE(published) << published.error().message;
    EXPECT_EQ(read_text(output), "replacement");
    EXPECT_FALSE(std::filesystem::exists(temporary));
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, NoOverwritePreservesBothFilesWhenDestinationExists) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-no-overwrite";
    const auto temporary = root / ".output.tmp";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::ofstream{temporary, std::ios::binary} << "candidate";
    std::ofstream{output, std::ios::binary} << "original";

    const auto published = axk::detail::publish_temporary_file(temporary, output, false);

    ASSERT_FALSE(published);
    EXPECT_EQ(read_text(output), "original");
    EXPECT_EQ(read_text(temporary), "candidate");
    std::filesystem::remove_all(root, error);
}

} // namespace
