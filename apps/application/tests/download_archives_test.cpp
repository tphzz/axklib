#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/download_archives.hpp"

namespace {

class DownloadArchiveStoreTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ =
            std::filesystem::temp_directory_path() /
            ("axklib-download-archives-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / "exports/nested");
        std::ofstream{root_ / "exports/alpha.txt", std::ios::binary} << "alpha";
        std::ofstream{root_ / "exports/nested/beta.bin", std::ios::binary} << "beta";
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox) << sandbox.error().message;
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    std::filesystem::path root_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
};

TEST_F(DownloadArchiveStoreTest, CreatesOwnerBoundDeterministicTarAndRemovesItExplicitly) {
    axk::app::DownloadArchiveStore store{root_ / "archives", 1024U * 1024U, 1024U * 1024U, 16U,
                                         std::chrono::seconds{30}};
    const auto created = store.create("owner-a", *sandbox_, {"workspace", "exports"});
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->filename, "exports.tar");
    EXPECT_EQ(created->entry_count, 3U);
    EXPECT_EQ(created->size_bytes, 3584U);

    const auto denied = store.open(created->reference, "owner-b");
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "download_archive_not_found");

    std::vector<char> bytes;
    {
        const auto content = store.open(created->reference, "owner-a");
        ASSERT_TRUE(content) << content.error().message;
        bytes.resize(static_cast<std::size_t>(content->reader->size()));
        ASSERT_TRUE(content->reader->read_exact_at(0U, std::as_writable_bytes(std::span{bytes})));
    }
    ASSERT_EQ(bytes.size(), created->size_bytes);
#if !defined(_WIN32)
    const auto archive_path = root_ / "archives" / (created->reference.archive_id + ".tar");
    EXPECT_EQ(std::filesystem::status(root_ / "archives").permissions() & std::filesystem::perms::all,
              std::filesystem::perms::owner_all);
    EXPECT_EQ(std::filesystem::status(archive_path).permissions() & std::filesystem::perms::all,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
    EXPECT_EQ(std::string(bytes.data(), 9U), "alpha.txt");
    EXPECT_EQ(std::string(bytes.data() + 512, 5U), "alpha");
    EXPECT_EQ(std::string(bytes.data() + 1024, 6U), "nested");
    EXPECT_EQ(bytes[1024U + 156U], '5');
    EXPECT_EQ(std::string(bytes.data() + 1536, 15U), "nested/beta.bin");
    EXPECT_EQ(std::string(bytes.data() + 2048, 4U), "beta");

    ASSERT_TRUE(store.remove(created->reference, "owner-a"));
    EXPECT_FALSE(store.inspect(created->reference, "owner-a"));
}

TEST_F(DownloadArchiveStoreTest, ArchivesOnlyPrivateServerOwnedDirectoriesWithAnExplicitFilename) {
    const auto owned = root_ / "owned-export";
    std::filesystem::create_directory(owned);
    std::filesystem::permissions(owned, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
    std::ofstream{owned / "instrument.sfz"} << "<region>\n";
    axk::app::DownloadArchiveStore store{root_ / "owned-archives", 1024U * 1024U, 1024U * 1024U, 16U,
                                         std::chrono::seconds{30}};

    const auto created = store.create_owned_directory("owner", owned, "Instrument.tar");
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->filename, "Instrument.tar");
    EXPECT_EQ(created->media_type, "application/x-tar");
    EXPECT_EQ(created->entry_count, 1U);

    EXPECT_FALSE(store.create_owned_directory("owner", owned, "../escape.tar"));
#if !defined(_WIN32)
    std::filesystem::permissions(owned, std::filesystem::perms::owner_all | std::filesystem::perms::group_read,
                                 std::filesystem::perm_options::replace);
    const auto public_source = store.create_owned_directory("owner", owned, "Public.tar");
    ASSERT_FALSE(public_source);
    EXPECT_EQ(public_source.error().code, "invalid_archive_request");
#endif
}

TEST_F(DownloadArchiveStoreTest, RetainsOwnerBoundNonArchiveDownloads) {
    axk::app::DownloadArchiveStore store{root_ / "retained", 1024U, 1024U, 4U, std::chrono::seconds{30}};
    const std::array payload{std::byte{0x41}, std::byte{0x58}, std::byte{0x4b}};
    const auto created = store.retain("owner-a", "Volume.axkvol", "application/octet-stream", payload);
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->filename, "Volume.axkvol");
    EXPECT_EQ(created->media_type, "application/octet-stream");
    EXPECT_EQ(created->size_bytes, payload.size());
    EXPECT_EQ(created->entry_count, 0U);

    const auto denied = store.open(created->reference, "owner-b");
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "download_archive_not_found");
    const auto content = store.open(created->reference, "owner-a");
    ASSERT_TRUE(content) << content.error().message;
    std::array<std::byte, 3U> read{};
    ASSERT_TRUE(content->reader->read_exact_at(0U, read));
    EXPECT_EQ(read, payload);
    EXPECT_EQ(content->path.extension(), ".download");
}

TEST_F(DownloadArchiveStoreTest, EnforcesEntryByteAndRetentionLimitsWithoutLeavingStagingFiles) {
    auto now = std::chrono::steady_clock::now();
    axk::app::DownloadArchiveStore limited{root_ / "limited",  2048U, 2048U, 1U, std::chrono::seconds{5},
                                           [&] { return now; }};
    const auto too_many = limited.create("owner", *sandbox_, {"workspace", "exports"});
    ASSERT_FALSE(too_many);
    EXPECT_EQ(too_many.error().code, "download_archive_too_large");
    EXPECT_FALSE(too_many.error().retryable);

    axk::app::DownloadArchiveStore expiring{root_ / "expiring", 4096U, 4096U, 4U, std::chrono::seconds{5},
                                            [&] { return now; }};
    const auto created = expiring.create("owner", *sandbox_, {"workspace", "exports"});
    ASSERT_TRUE(created) << created.error().message;
    now += std::chrono::seconds{6};
    expiring.cleanup();
    const auto expired = expiring.inspect(created->reference, "owner");
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, "download_archive_not_found");
}

TEST_F(DownloadArchiveStoreTest, RetainsExpiredArchiveAndQuotaWhenRemovalFails) {
    auto now = std::chrono::steady_clock::now();
    axk::app::DownloadArchiveStore store{root_ / "locked",   3584U, 3584U, 4U, std::chrono::seconds{5},
                                         [&] { return now; }};
    const auto created = store.create("owner", *sandbox_, {"workspace", "exports"});
    ASSERT_TRUE(created) << created.error().message;
    {
        const auto content = store.open(created->reference, "owner");
        ASSERT_TRUE(content) << content.error().message;
    }
    const auto path = root_ / "locked" / (created->reference.archive_id + ".tar");

    ASSERT_TRUE(std::filesystem::remove(path));
    ASSERT_TRUE(std::filesystem::create_directory(path));
    std::ofstream{path / "transfer-open"} << "held";
    now += std::chrono::seconds{6};
    store.cleanup();

    const auto retained = store.inspect(created->reference, "owner");
    ASSERT_TRUE(retained) << retained.error().message;
    const auto exhausted = store.create("owner", *sandbox_, {"workspace", "exports"});
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().code, "download_archive_quota_exceeded");
}

TEST_F(DownloadArchiveStoreTest, ActiveDownloadLeaseDefersExpiryAndDeletion) {
    auto now = std::chrono::steady_clock::now();
    axk::app::DownloadArchiveStore store{root_ / "leased",   4096U, 4096U, 4U, std::chrono::seconds{5},
                                         [&] { return now; }};
    const auto created = store.create("owner", *sandbox_, {"workspace", "exports"});
    ASSERT_TRUE(created) << created.error().message;
    auto opened = store.open(created->reference, "owner");
    ASSERT_TRUE(opened) << opened.error().message;
    std::optional<axk::app::DownloadArchiveContent> content{std::move(*opened)};
    EXPECT_EQ(content->path, root_ / "leased" / (created->reference.archive_id + ".tar"));

    now += std::chrono::seconds{6};
    store.cleanup();
    EXPECT_TRUE(std::filesystem::is_regular_file(content->path));
    const auto in_use = store.remove(created->reference, "owner");
    ASSERT_FALSE(in_use);
    EXPECT_EQ(in_use.error().code, "archive_in_use");
    EXPECT_TRUE(in_use.error().retryable);

    content.reset();
    store.cleanup();
    EXPECT_FALSE(std::filesystem::exists(root_ / "leased" / (created->reference.archive_id + ".tar")));
    const auto expired = store.inspect(created->reference, "owner");
    ASSERT_FALSE(expired);
    EXPECT_EQ(expired.error().code, "download_archive_not_found");
}

TEST_F(DownloadArchiveStoreTest, ConcurrentReservationsCannotExceedTheArchiveQuota) {
    axk::app::DownloadArchiveStore store{root_ / "concurrent", 3584U, 3584U, 16U, std::chrono::seconds{30}};
    constexpr std::size_t contenders = 8U;
    std::barrier start{static_cast<std::ptrdiff_t>(contenders)};
    std::atomic<std::size_t> accepted{};
    std::atomic<std::size_t> rejected{};
    std::vector<std::thread> threads;
    threads.reserve(contenders);
    for (std::size_t index = 0; index < contenders; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            const auto created = store.create("owner-" + std::to_string(index), *sandbox_, {"workspace", "exports"});
            if (created) {
                ++accepted;
            } else if (created.error().code == "download_archive_quota_exceeded") {
                ++rejected;
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    EXPECT_EQ(accepted, 1U);
    EXPECT_EQ(rejected, contenders - 1U);
}

TEST_F(DownloadArchiveStoreTest, CountsAndArchivesWideEmptyDirectoryTrees) {
    constexpr std::size_t file_count = 1500U;
    std::filesystem::create_directories(root_ / "wide/empty/leaf");
    for (std::size_t index = 0U; index < file_count; ++index)
        std::ofstream{root_ / "wide" / ("file-" + std::to_string(index) + ".bin"), std::ios::binary}.put('x');

    axk::app::DownloadArchiveStore store{root_ / "wide-archives", 4U * 1024U * 1024U, 4U * 1024U * 1024U,
                                         file_count + 2U, std::chrono::seconds{30}};
    const auto created = store.create("owner", *sandbox_, {"workspace", "wide"});
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->entry_count, file_count + 2U);

    const auto content = store.open(created->reference, "owner");
    ASSERT_TRUE(content) << content.error().message;
    std::vector<std::byte> bytes(static_cast<std::size_t>(content->reader->size()));
    ASSERT_TRUE(content->reader->read_exact_at(0U, bytes));
    std::size_t directory_headers{};
    for (std::size_t offset = 0U; offset + 512U <= bytes.size();) {
        const auto header = std::span{bytes}.subspan(offset, 512U);
        if (std::ranges::all_of(header, [](std::byte value) { return value == std::byte{}; }))
            break;
        if (header[156U] == std::byte{'5'})
            ++directory_headers;
        std::uint64_t size{};
        for (std::size_t digit = 124U;
             digit < 135U && header[digit] >= std::byte{'0'} && header[digit] <= std::byte{'7'}; ++digit) {
            size = size * 8U + static_cast<std::uint64_t>(std::to_integer<unsigned int>(header[digit]) - '0');
        }
        offset += 512U + static_cast<std::size_t>((size + 511U) / 512U) * 512U;
    }
    EXPECT_EQ(directory_headers, 2U);
}

} // namespace
