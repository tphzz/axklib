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
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const auto candidate = axk::detail::write_temporary_file(output, [](const auto &sink) {
        constexpr std::string_view replacement{"replacement"};
        return sink(std::as_bytes(std::span{replacement}));
    });
    ASSERT_TRUE(candidate);
    std::ofstream{output, std::ios::binary} << "original";

    const auto published = axk::detail::publish_temporary_file(*candidate, output, true);

    ASSERT_TRUE(published) << published.error().message;
    EXPECT_EQ(read_text(output), "replacement");
    EXPECT_FALSE(std::filesystem::exists(*candidate));
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, NoOverwritePreservesAConcurrentWinnerAndTheCandidate) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-no-overwrite";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const auto candidate = axk::detail::write_temporary_file(output, [](const auto &sink) {
        constexpr std::string_view content{"candidate"};
        return sink(std::as_bytes(std::span{content}));
    });
    ASSERT_TRUE(candidate);
    std::ofstream{output, std::ios::binary} << "original";

    const auto published = axk::detail::publish_temporary_file(*candidate, output, false);

    ASSERT_FALSE(published);
    EXPECT_EQ(read_text(output), "original");
    EXPECT_EQ(read_text(*candidate), "candidate");
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
    axk::detail::discard_temporary_file(*first);
    axk::detail::discard_temporary_file(*second);
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
    axk::detail::discard_temporary_file(*temporary);
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
#if !defined(_WIN32)
    EXPECT_TRUE(std::filesystem::is_empty(root / ".axklib-publication"));
#else
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{root}, std::filesystem::directory_iterator{}), 1);
#endif
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, RejectsCandidatePathReplacementBeforePublication) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-rebinding";
    const auto output = root / "output.bin";
    const auto victim = root / "victim.bin";
    const auto displaced = root / "displaced.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::ofstream{victim, std::ios::binary} << "victim";
    const auto candidate = axk::detail::write_temporary_file(output, [](const auto &sink) {
        constexpr std::string_view content{"validated"};
        return sink(std::as_bytes(std::span{content}));
    });
    ASSERT_TRUE(candidate);
    std::filesystem::rename(*candidate, displaced);
#if defined(_WIN32)
    std::ofstream{*candidate, std::ios::binary} << "substitute";
#else
    std::filesystem::create_symlink(victim, *candidate);
#endif

    const auto published = axk::detail::publish_temporary_file(*candidate, output, true);

    ASSERT_FALSE(published);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read_text(displaced), "validated");
    EXPECT_EQ(read_text(victim), "victim");
#if defined(_WIN32)
    EXPECT_EQ(read_text(*candidate), "substitute");
#endif
    std::filesystem::remove_all(root, error);
}

#if !defined(_WIN32)
TEST(FilePublication, PublishesThroughTheRetainedParentAfterPathRebinding) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-parent-rebinding";
    const auto original_parent = root / "destination";
    const auto retained_parent = root / "retained-destination";
    const auto output = original_parent / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(original_parent);
    const auto candidate = axk::detail::write_temporary_file(output, [](const auto &sink) {
        constexpr std::string_view content{"validated"};
        return sink(std::as_bytes(std::span{content}));
    });
    ASSERT_TRUE(candidate);
    std::filesystem::rename(original_parent, retained_parent);
    std::filesystem::create_directories(original_parent);

    const auto published = axk::detail::publish_temporary_file(*candidate, output, true);

    ASSERT_TRUE(published) << published.error().message;
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read_text(retained_parent / output.filename()), "validated");
    std::filesystem::remove_all(root, error);
}
#endif

} // namespace
