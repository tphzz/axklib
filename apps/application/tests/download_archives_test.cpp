#include <atomic>
#include <barrier>
#include <chrono>
#include <filesystem>
#include <fstream>
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
    EXPECT_EQ(created->entry_count, 2U);
    EXPECT_EQ(created->size_bytes, 3072U);

    const auto denied = store.resolve(created->reference, "owner-b");
    ASSERT_FALSE(denied);
    EXPECT_EQ(denied.error().code, "download_archive_not_found");

    const auto path = store.resolve(created->reference, "owner-a");
    ASSERT_TRUE(path) << path.error().message;
    std::ifstream input{*path, std::ios::binary};
    std::vector<char> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    ASSERT_EQ(bytes.size(), created->size_bytes);
    EXPECT_EQ(std::string(bytes.data(), 9U), "alpha.txt");
    EXPECT_EQ(std::string(bytes.data() + 512, 5U), "alpha");
    EXPECT_EQ(std::string(bytes.data() + 1024, 15U), "nested/beta.bin");
    EXPECT_EQ(std::string(bytes.data() + 1536, 4U), "beta");

    ASSERT_TRUE(store.remove(created->reference, "owner-a"));
    EXPECT_FALSE(std::filesystem::exists(*path));
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
    axk::app::DownloadArchiveStore store{root_ / "locked",   3072U, 3072U, 4U, std::chrono::seconds{5},
                                         [&] { return now; }};
    const auto created = store.create("owner", *sandbox_, {"workspace", "exports"});
    ASSERT_TRUE(created) << created.error().message;
    const auto path = store.resolve(created->reference, "owner");
    ASSERT_TRUE(path) << path.error().message;

    ASSERT_TRUE(std::filesystem::remove(*path));
    ASSERT_TRUE(std::filesystem::create_directory(*path));
    std::ofstream{*path / "transfer-open"} << "held";
    now += std::chrono::seconds{6};
    store.cleanup();

    const auto retained = store.inspect(created->reference, "owner");
    ASSERT_TRUE(retained) << retained.error().message;
    const auto exhausted = store.create("owner", *sandbox_, {"workspace", "exports"});
    ASSERT_FALSE(exhausted);
    EXPECT_EQ(exhausted.error().code, "download_archive_quota_exceeded");
}

TEST_F(DownloadArchiveStoreTest, ConcurrentReservationsCannotExceedTheArchiveQuota) {
    axk::app::DownloadArchiveStore store{root_ / "concurrent", 3072U, 3072U, 16U, std::chrono::seconds{30}};
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

} // namespace
