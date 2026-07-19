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

TEST(FilePublication, NoOverwritePreservesAConcurrentWinnerAndTheCandidate) {
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

TEST(FilePublication, FailedReplacementLeavesTheOriginalDestinationIntact) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-failed-replace";
    const auto missing_temporary = root / ".missing.tmp";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::ofstream{output, std::ios::binary} << "original";

    const auto published = axk::detail::publish_temporary_file(missing_temporary, output, true);

    ASSERT_FALSE(published);
    EXPECT_EQ(read_text(output), "original");
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, ExclusivelyReservesUniqueRegularTemporarySiblings) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-reserve";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    const auto first = axk::detail::reserve_temporary_file(output);
    const auto second = axk::detail::reserve_temporary_file(output);

    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(*first, *second);
    EXPECT_TRUE(std::filesystem::is_regular_file(*first));
    EXPECT_TRUE(std::filesystem::is_regular_file(*second));
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, StreamsAndFlushesThroughTheExclusivelyCreatedFile) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-write";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    const auto temporary = axk::detail::write_temporary_file(output, [](const axk::detail::TemporaryFileSink &sink) {
        const std::string first{"candidate"};
        const std::string second{"-data"};
        if (auto written = sink(std::as_bytes(std::span{first.data(), first.size()})); !written)
            return written;
        return sink(std::as_bytes(std::span{second.data(), second.size()}));
    });

    ASSERT_TRUE(temporary) << temporary.error().message;
    EXPECT_EQ(read_text(*temporary), "candidate-data");
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, ProducerFailureRemovesTheExclusiveCandidateAndPreservesTheDestination) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-producer-failure";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::ofstream{output, std::ios::binary} << "original";

    const auto temporary =
        axk::detail::write_temporary_file(output, [](const axk::detail::TemporaryFileSink &) -> axk::Result<void> {
            return std::unexpected{
                axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "injected producer failure")};
        });

    ASSERT_FALSE(temporary);
    EXPECT_EQ(read_text(output), "original");
    std::size_t sibling_count{};
    for (const auto &entry : std::filesystem::directory_iterator{root}) {
        static_cast<void>(entry);
        ++sibling_count;
    }
    EXPECT_EQ(sibling_count, 1U);
    std::filesystem::remove_all(root, error);
}

} // namespace
