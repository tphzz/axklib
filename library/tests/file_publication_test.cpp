#include <array>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "axklib/file_publication.hpp"
#include "axklib/io.hpp"

namespace {

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

TEST(FilePublication, ResizePreservesWrittenBytesAndKeepsUnwrittenRegionsSparse) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-sparse";
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    } cleanup{root};
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const auto output = root / "sparse.bin";
    auto publication = axk::detail::TemporaryPublication::create(output);
    ASSERT_TRUE(publication) << publication.error().message;
    constexpr std::uint64_t size = 64ULL * 1024ULL * 1024ULL;
    const std::array prefix{std::byte{'A'}, std::byte{'B'}};
    ASSERT_TRUE(publication->append(prefix));
    const auto resized = publication->resize(size);
    ASSERT_TRUE(resized) << resized.error().message;
    const std::array tail{std::byte{'Z'}};
    ASSERT_TRUE(publication->write_at(size - 1U, tail));
    ASSERT_TRUE(publication->flush());
    EXPECT_EQ(std::filesystem::file_size(publication->path()), size);
#if defined(_WIN32)
    const auto close_file = [](void *handle) {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    };
    const std::unique_ptr<void, decltype(close_file)> handle{
        CreateFileW(publication->path().c_str(), FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT, nullptr),
        close_file};
    ASSERT_NE(handle.get(), INVALID_HANDLE_VALUE) << GetLastError();
    DWORD flags{};
    const auto queried =
        GetVolumeInformationByHandleW(handle.get(), nullptr, 0U, nullptr, nullptr, &flags, nullptr, 0U);
    if (queried == 0) {
        const auto native_error = GetLastError();
        RecordProperty("sparse_allocation_check", "unavailable: Windows error " + std::to_string(native_error));
    } else if ((flags & FILE_SUPPORTS_SPARSE_FILES) == 0U) {
        RecordProperty("sparse_allocation_check", "filesystem does not advertise sparse support");
    } else {
        const auto attributes = GetFileAttributesW(publication->path().c_str());
        ASSERT_NE(attributes, INVALID_FILE_ATTRIBUTES) << GetLastError();
        EXPECT_NE(attributes & FILE_ATTRIBUTE_SPARSE_FILE, 0U);
        DWORD high{};
        SetLastError(NO_ERROR);
        const auto low = GetCompressedFileSizeW(publication->path().c_str(), &high);
        const auto native_error = GetLastError();
        ASSERT_TRUE(low != INVALID_FILE_SIZE || native_error == NO_ERROR) << native_error;
        const auto allocated = (static_cast<std::uint64_t>(high) << 32U) | low;
        EXPECT_LT(allocated, size / 4U);
        RecordProperty("sparse_allocation_check", "checked");
    }
#endif
    {
        const auto reader = axk::FileReader::open(publication->path());
        ASSERT_TRUE(reader) << reader.error().message;
        std::array<std::byte, 2> bytes{};
        ASSERT_TRUE((*reader)->read_exact_at(0U, bytes));
        EXPECT_EQ(bytes, prefix);
        ASSERT_TRUE((*reader)->read_exact_at(size / 2U, bytes));
        EXPECT_EQ(bytes, (std::array<std::byte, 2>{}));
        ASSERT_TRUE((*reader)->read_exact_at(size - 2U, bytes));
        EXPECT_EQ(bytes, (std::array{std::byte{}, std::byte{'Z'}}));
    }
    ASSERT_TRUE(publication->resize(1U));
    ASSERT_TRUE(publication->resize(2U));
    ASSERT_TRUE(publication->flush());
    ASSERT_TRUE(publication->publish(axk::detail::PublicationMode::create_only));
    EXPECT_EQ(read_text(output), std::string("A\0", 2U));
}

TEST(FilePublication, AtomicallyReplacesAnExistingDestination) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-replace";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    auto publication = axk::detail::TemporaryPublication::create(output, [](const auto &sink) {
        constexpr std::string_view replacement{"replacement"};
        return sink(std::as_bytes(std::span{replacement}));
    });
    ASSERT_TRUE(publication);
    const auto candidate = publication->path();
    std::ofstream{output, std::ios::binary} << "original";

    const auto published = publication->publish(axk::detail::PublicationMode::replace_existing);

    ASSERT_TRUE(published) << published.error().message;
    EXPECT_EQ(read_text(output), "replacement");
    EXPECT_FALSE(std::filesystem::exists(candidate));
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, NoOverwritePreservesAConcurrentWinnerAndOwnerCanDiscardCandidate) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-no-overwrite";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    auto publication = axk::detail::TemporaryPublication::create(output, [](const auto &sink) {
        constexpr std::string_view content{"candidate"};
        return sink(std::as_bytes(std::span{content}));
    });
    ASSERT_TRUE(publication);
    const auto candidate = publication->path();
    std::ofstream{output, std::ios::binary} << "original";

    const auto published = publication->publish(axk::detail::PublicationMode::create_only);

    ASSERT_FALSE(published);
    EXPECT_EQ(read_text(output), "original");
    EXPECT_EQ(read_text(candidate), "candidate");
    ASSERT_TRUE(publication->discard());
    EXPECT_FALSE(std::filesystem::exists(candidate));
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, NoOverwriteRacePreservesTheWinnerAtTheAtomicPublicationBoundary) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-concurrent-winner";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::barrier publication_ready{2};
    std::barrier winner_created{2};
    auto hooks = std::make_shared<axk::detail::PublicationHooks>();
    hooks->before_publish = [&] {
        publication_ready.arrive_and_wait();
        winner_created.arrive_and_wait();
    };
    auto publication = axk::detail::TemporaryPublication::create(output, hooks);
    ASSERT_TRUE(publication) << publication.error().message;
    ASSERT_TRUE(publication->append(std::as_bytes(std::span{"candidate", 9U})));
    ASSERT_TRUE(publication->flush());
    std::thread winner{[&] {
        publication_ready.arrive_and_wait();
        std::ofstream{output, std::ios::binary} << "winner";
        winner_created.arrive_and_wait();
    }};

    const auto published = publication->publish(axk::detail::PublicationMode::create_only);
    winner.join();

    ASSERT_FALSE(published);
    EXPECT_EQ(read_text(output), "winner");
    ASSERT_TRUE(publication->discard());
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, RepeatedNoOverwriteFailuresReleaseTheCandidateForCleanup) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-repeated-no-overwrite";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::ofstream{output, std::ios::binary} << "winner";

    for (std::size_t attempt = 0; attempt < 128U; ++attempt) {
        auto publication = axk::detail::TemporaryPublication::create(output, [](const auto &sink) {
            constexpr std::string_view content{"candidate"};
            return sink(std::as_bytes(std::span{content}));
        });
        ASSERT_TRUE(publication) << publication.error().message;
        const auto candidate = publication->path();

        const auto published = publication->publish(axk::detail::PublicationMode::create_only);

        ASSERT_FALSE(published);
        ASSERT_TRUE(publication->discard());
        EXPECT_FALSE(std::filesystem::exists(candidate));
    }

    EXPECT_EQ(read_text(output), "winner");
#if !defined(_WIN32)
    EXPECT_TRUE(std::filesystem::is_empty(root / ".axklib-publication"));
#else
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{root}, std::filesystem::directory_iterator{}), 1);
#endif
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, PublishingTwiceFailsAndLeavesTheCommittedDestinationIntact) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-double-publish";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    auto publication = axk::detail::TemporaryPublication::create(output, [](const auto &sink) {
        constexpr std::string_view content{"committed"};
        return sink(std::as_bytes(std::span{content}));
    });
    ASSERT_TRUE(publication);
    ASSERT_TRUE(publication->publish(axk::detail::PublicationMode::replace_existing));

    const auto published = publication->publish(axk::detail::PublicationMode::replace_existing);

    ASSERT_FALSE(published);
    EXPECT_EQ(read_text(output), "committed");
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, ExclusivelyReservesUniqueRegularTemporarySiblings) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-reserve";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    auto first = axk::detail::TemporaryPublication::create(output);
    auto second = axk::detail::TemporaryPublication::create(output);

    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(first->path(), second->path());
    EXPECT_TRUE(std::filesystem::is_regular_file(first->path()));
    EXPECT_TRUE(std::filesystem::is_regular_file(second->path()));
    ASSERT_TRUE(first->discard());
    ASSERT_TRUE(second->discard());
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, StreamsAndFlushesThroughTheExclusivelyCreatedFile) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-write";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);

    auto publication =
        axk::detail::TemporaryPublication::create(output, [](const axk::detail::TemporaryFileSink &sink) {
            const std::string first{"candidate"};
            const std::string second{"-data"};
            if (auto written = sink(std::as_bytes(std::span{first.data(), first.size()})); !written)
                return written;
            return sink(std::as_bytes(std::span{second.data(), second.size()}));
        });

    ASSERT_TRUE(publication) << publication.error().message;
    EXPECT_EQ(read_text(publication->path()), "candidate-data");
    ASSERT_TRUE(publication->discard());
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, ProducerFailureRemovesTheExclusiveCandidateAndPreservesTheDestination) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-producer-failure";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::ofstream{output, std::ios::binary} << "original";

    const auto publication = axk::detail::TemporaryPublication::create(
        output, [](const axk::detail::TemporaryFileSink &) -> axk::Result<void> {
            return std::unexpected{
                axk::make_error(axk::ErrorCode::io_read_failed, axk::ErrorCategory::io, "injected producer failure")};
        });

    ASSERT_FALSE(publication);
    EXPECT_EQ(read_text(output), "original");
#if !defined(_WIN32)
    EXPECT_TRUE(std::filesystem::is_empty(root / ".axklib-publication"));
#else
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator{root}, std::filesystem::directory_iterator{}), 1);
#endif
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, DestructionDiscardsAnUnpublishedCandidate) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-destructor";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::filesystem::path candidate;
    {
        auto publication = axk::detail::TemporaryPublication::create(output, [](const auto &sink) {
            constexpr std::string_view content{"candidate"};
            return sink(std::as_bytes(std::span{content}));
        });
        ASSERT_TRUE(publication);
        candidate = publication->path();
        ASSERT_TRUE(std::filesystem::exists(candidate));
    }

    EXPECT_FALSE(std::filesystem::exists(candidate));
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove_all(root, error);
}

TEST(FilePublication, MoveTransfersSoleCleanupOwnership) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-file-publication-move";
    const auto output = root / "output.bin";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    std::filesystem::path candidate;
    {
        auto created = axk::detail::TemporaryPublication::create(output);
        ASSERT_TRUE(created);
        candidate = created->path();
        auto first = std::move(*created);
        auto second = std::move(first);
        ASSERT_TRUE(std::filesystem::exists(candidate));
        ASSERT_TRUE(second.append(std::as_bytes(std::span{"owned", 5U})));
    }

    EXPECT_FALSE(std::filesystem::exists(candidate));
    EXPECT_FALSE(std::filesystem::exists(output));
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
    auto publication = axk::detail::TemporaryPublication::create(output, [](const auto &sink) {
        constexpr std::string_view content{"validated"};
        return sink(std::as_bytes(std::span{content}));
    });
    ASSERT_TRUE(publication);
    const auto candidate = publication->path();
    std::filesystem::rename(candidate, displaced);
#if defined(_WIN32)
    std::ofstream{candidate, std::ios::binary} << "substitute";
#else
    std::filesystem::create_symlink(victim, candidate);
#endif

    const auto published = publication->publish(axk::detail::PublicationMode::replace_existing);

    ASSERT_FALSE(published);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read_text(displaced), "validated");
    EXPECT_EQ(read_text(victim), "victim");
#if defined(_WIN32)
    EXPECT_EQ(read_text(candidate), "substitute");
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
    auto publication = axk::detail::TemporaryPublication::create(output, [](const auto &sink) {
        constexpr std::string_view content{"validated"};
        return sink(std::as_bytes(std::span{content}));
    });
    ASSERT_TRUE(publication);
    std::filesystem::rename(original_parent, retained_parent);
    std::filesystem::create_directories(original_parent);

    const auto published = publication->publish(axk::detail::PublicationMode::replace_existing);

    ASSERT_TRUE(published) << published.error().message;
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read_text(retained_parent / output.filename()), "validated");
    std::filesystem::remove_all(root, error);
}
#endif

} // namespace
