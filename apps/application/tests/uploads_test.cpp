#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/uploads.hpp"

namespace {

std::span<const std::byte> bytes(std::string_view value) {
    return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
}

class UploadStoreTest : public testing::Test {
  protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path() / "axklib-upload-store-test";
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
        std::filesystem::create_directories(directory_);
#if !defined(_WIN32)
        std::filesystem::permissions(directory_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace);
#endif
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    [[nodiscard]] axk::app::UploadStore store() const {
        return {directory_, 32U, 16U, 2U, 4U, std::chrono::seconds{60}};
    }

    std::filesystem::path directory_;
};

TEST_F(UploadStoreTest, ReceivesBoundedChunksAndFinalizesOnlyAfterHashVerification) {
    auto value = store();
    const auto created = value.create({.owner_id = "owner",
                                       .filename = "sample.wav",
                                       .kind = axk::app::UploadKind::audio,
                                       .media_type = "audio/wav",
                                       .declared_size = 3U,
                                       .sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_EQ(created->state, axk::app::UploadState::receiving);
    EXPECT_FALSE(value.resolve(created->reference, "owner"));

    const auto appended = value.append(created->reference, "owner", 0U, bytes("abc"));
    ASSERT_TRUE(appended) << appended.error().message;
    EXPECT_EQ(appended->received_size, 3U);
    const auto completed = value.complete(created->reference, "owner");
    ASSERT_TRUE(completed) << completed.error().message;
    EXPECT_EQ(completed->state, axk::app::UploadState::ready);
    const auto path = value.resolve(created->reference, "owner");
    ASSERT_TRUE(path) << path.error().message;
    EXPECT_EQ(std::filesystem::file_size(*path), 3U);
#if !defined(_WIN32)
    EXPECT_EQ(std::filesystem::status(directory_).permissions() & std::filesystem::perms::all,
              std::filesystem::perms::owner_all);
    EXPECT_EQ(std::filesystem::status(*path).permissions() & std::filesystem::perms::all,
              std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
}

#if !defined(_WIN32)
TEST(UploadStorePermissions, RejectsAnUnsafePreexistingStorageDirectory) {
    const auto directory = std::filesystem::temp_directory_path() / "axklib-upload-store-unsafe";
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    std::filesystem::create_directories(directory);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all | std::filesystem::perms::group_read,
                                 std::filesystem::perm_options::replace);
    axk::app::UploadStore store{directory, 32U, 16U, 2U, 4U, std::chrono::seconds{60}};
    EXPECT_FALSE(store.storage_ready());
    EXPECT_FALSE(store.create({.owner_id = "owner",
                               .filename = "sample.wav",
                               .kind = axk::app::UploadKind::audio,
                               .media_type = "audio/wav",
                               .declared_size = 3U,
                               .sha256 = std::nullopt}));
    std::filesystem::remove_all(directory, error);
}
#endif

TEST_F(UploadStoreTest, RejectsDiskImagesWrongOwnersOffsetsAndOversizedChunks) {
    auto value = store();
    const auto disk = value.create({.owner_id = "owner",
                                    .filename = "disk.iso",
                                    .kind = axk::app::UploadKind::package,
                                    .media_type = "application/octet-stream",
                                    .declared_size = 3U,
                                    .sha256 = std::nullopt});
    ASSERT_FALSE(disk);
    EXPECT_EQ(disk.error().code, "upload_type_not_allowed");

    const auto created = value.create({.owner_id = "owner",
                                       .filename = "object.axkvol",
                                       .kind = axk::app::UploadKind::package,
                                       .media_type = "application/vnd.axklib.package",
                                       .declared_size = 5U,
                                       .sha256 = std::nullopt});
    ASSERT_TRUE(created) << created.error().message;
    EXPECT_FALSE(value.inspect(created->reference, "other"));
    EXPECT_FALSE(value.append(created->reference, "owner", 1U, bytes("a")));
    EXPECT_FALSE(value.append(created->reference, "owner", 0U, bytes("abcde")));
}

TEST_F(UploadStoreTest, EnforcesReservedByteQuotaAndExpiresUploads) {
    auto now = std::chrono::steady_clock::now();
    axk::app::UploadStore value{directory_, 6U, 6U, 2U, 4U, std::chrono::seconds{5}, [&now] { return now; }};
    const auto first = value.create({.owner_id = "owner",
                                     .filename = "first.json",
                                     .kind = axk::app::UploadKind::manifest,
                                     .media_type = "application/json",
                                     .declared_size = 4U,
                                     .sha256 = std::nullopt});
    ASSERT_TRUE(first) << first.error().message;
    const auto second = value.create({.owner_id = "owner",
                                      .filename = "second.json",
                                      .kind = axk::app::UploadKind::manifest,
                                      .media_type = "application/json",
                                      .declared_size = 4U,
                                      .sha256 = std::nullopt});
    ASSERT_FALSE(second);
    EXPECT_EQ(second.error().code, "upload_quota_exceeded");
    EXPECT_TRUE(second.error().retryable);

    now += std::chrono::seconds{6};
    value.cleanup();
    EXPECT_FALSE(value.inspect(first->reference, "owner"));
    const auto replacement = value.create({.owner_id = "owner",
                                           .filename = "second.json",
                                           .kind = axk::app::UploadKind::manifest,
                                           .media_type = "application/json",
                                           .declared_size = 4U,
                                           .sha256 = std::nullopt});
    ASSERT_TRUE(replacement) << replacement.error().message;
}

TEST_F(UploadStoreTest, ConcurrentReservationsCannotExceedTheWorkspaceQuota) {
    axk::app::UploadStore value{directory_, 4U, 4U, 32U, 4U, std::chrono::seconds{60}};
    constexpr std::size_t contenders = 16U;
    std::barrier start{static_cast<std::ptrdiff_t>(contenders)};
    std::atomic<std::size_t> accepted{};
    std::atomic<std::size_t> rejected{};
    std::mutex reference_mutex;
    std::optional<axk::app::UploadRef> accepted_reference;
    std::vector<std::thread> threads;
    threads.reserve(contenders);
    for (std::size_t index = 0; index < contenders; ++index) {
        threads.emplace_back([&, index] {
            start.arrive_and_wait();
            const auto created = value.create({.owner_id = "owner-" + std::to_string(index),
                                               .filename = "manifest.json",
                                               .kind = axk::app::UploadKind::manifest,
                                               .media_type = "application/json",
                                               .declared_size = 3U,
                                               .sha256 = std::nullopt});
            if (created) {
                ++accepted;
                const std::scoped_lock lock{reference_mutex};
                accepted_reference = created->reference;
            } else if (created.error().code == "upload_quota_exceeded") {
                ++rejected;
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    EXPECT_EQ(accepted, 1U);
    EXPECT_EQ(rejected, contenders - 1U);
    ASSERT_TRUE(accepted_reference);
}

TEST_F(UploadStoreTest, RejectsHashMismatchAndRemovesOwnedUploads) {
    auto value = store();
    const auto created = value.create({.owner_id = "owner",
                                       .filename = "manifest.json",
                                       .kind = axk::app::UploadKind::manifest,
                                       .media_type = "application/json",
                                       .declared_size = 3U,
                                       .sha256 = std::string(64U, '0')});
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_TRUE(value.append(created->reference, "owner", 0U, bytes("abc")));
    const auto completed = value.complete(created->reference, "owner");
    ASSERT_FALSE(completed);
    EXPECT_EQ(completed.error().code, "upload_hash_mismatch");
    ASSERT_TRUE(value.remove(created->reference, "owner"));
    EXPECT_FALSE(value.inspect(created->reference, "owner"));
}

TEST_F(UploadStoreTest, LeasePreventsExpiryAndDeletionUntilReleased) {
    auto now = std::chrono::steady_clock::now();
    axk::app::UploadStore value{directory_, 16U, 16U, 2U, 4U, std::chrono::seconds{5}, [&now] { return now; }};
    const auto created = value.create({.owner_id = "owner",
                                       .filename = "manifest.json",
                                       .kind = axk::app::UploadKind::manifest,
                                       .media_type = "application/json",
                                       .declared_size = 2U,
                                       .sha256 = std::nullopt});
    ASSERT_TRUE(created);
    ASSERT_TRUE(value.append(created->reference, "owner", 0U, bytes("{}")));
    ASSERT_TRUE(value.complete(created->reference, "owner"));
    {
        const auto lease = value.lease(created->reference, "owner");
        ASSERT_TRUE(lease) << lease.error().message;
        EXPECT_TRUE(std::filesystem::exists(lease->path()));
        now += std::chrono::seconds{6};
        value.cleanup();
        EXPECT_TRUE(value.inspect(created->reference, "owner"));
        const auto removed = value.remove(created->reference, "owner");
        ASSERT_FALSE(removed);
        EXPECT_EQ(removed.error().code, "upload_in_use");
    }
    now += std::chrono::seconds{6};
    value.cleanup();
    EXPECT_FALSE(value.inspect(created->reference, "owner"));
}

TEST_F(UploadStoreTest, RemovesAbandonedStagingFilesAtStartup) {
    const auto abandoned = directory_ / "abandoned.upload";
    std::ofstream(abandoned) << "partial";
    ASSERT_TRUE(std::filesystem::exists(abandoned));
    auto value = store();
    EXPECT_FALSE(std::filesystem::exists(abandoned));
}

TEST_F(UploadStoreTest, RetainsExpiredUploadAndQuotaWhenRemovalFails) {
    auto now = std::chrono::steady_clock::now();
    bool allow_removal = false;
    axk::app::UploadStore value{directory_,
                                4U,
                                4U,
                                2U,
                                4U,
                                std::chrono::seconds{5},
                                [&now] { return now; },
                                [&allow_removal](const std::filesystem::path &path, std::error_code &error) {
                                    if (!allow_removal) {
                                        error = std::make_error_code(std::errc::permission_denied);
                                        return false;
                                    }
                                    return std::filesystem::remove(path, error);
                                }};
    const auto created = value.create({.owner_id = "owner",
                                       .filename = "manifest.json",
                                       .kind = axk::app::UploadKind::manifest,
                                       .media_type = "application/json",
                                       .declared_size = 4U,
                                       .sha256 = std::nullopt});
    ASSERT_TRUE(created);
    now += std::chrono::seconds{6};

    const auto failed = value.cleanup_snapshot();
    EXPECT_FALSE(failed.healthy);
    EXPECT_EQ(failed.failed_deletions, 1U);
    EXPECT_EQ(failed.reserved_bytes, 4U);
    EXPECT_TRUE(std::filesystem::exists(directory_ / (created->reference.upload_id + ".upload")));
    EXPECT_FALSE(value.create({.owner_id = "other",
                               .filename = "replacement.json",
                               .kind = axk::app::UploadKind::manifest,
                               .media_type = "application/json",
                               .declared_size = 4U,
                               .sha256 = std::nullopt}));

    allow_removal = true;
    const auto recovered = value.cleanup_snapshot();
    EXPECT_TRUE(recovered.healthy);
    EXPECT_EQ(recovered.reserved_bytes, 0U);
}

TEST_F(UploadStoreTest, RetainsUndeletableStartupOrphanAndReportsItUnhealthy) {
    const auto abandoned = directory_ / "abandoned.upload";
    std::ofstream(abandoned) << "partial";
    axk::app::UploadStore value{directory_,
                                32U,
                                16U,
                                2U,
                                4U,
                                std::chrono::seconds{60},
                                std::chrono::steady_clock::now,
                                [](const std::filesystem::path &, std::error_code &error) {
                                    error = std::make_error_code(std::errc::permission_denied);
                                    return false;
                                }};

    const auto cleanup = value.cleanup_snapshot();
    EXPECT_FALSE(cleanup.healthy);
    EXPECT_EQ(cleanup.orphan_count, 1U);
    EXPECT_EQ(cleanup.orphan_bytes, 7U);
    EXPECT_EQ(cleanup.reserved_bytes, 7U);
    EXPECT_TRUE(std::filesystem::exists(abandoned));
}

TEST_F(UploadStoreTest, MaterializesReadyUploadAtomicallyInsideWritableSandbox) {
    auto value = store();
    const auto created = value.create({.owner_id = "owner",
                                       .filename = "manifest.json",
                                       .kind = axk::app::UploadKind::manifest,
                                       .media_type = "application/json",
                                       .declared_size = 2U,
                                       .sha256 = std::nullopt});
    ASSERT_TRUE(created);
    ASSERT_TRUE(value.append(created->reference, "owner", 0U, bytes("{}")));
    ASSERT_TRUE(value.complete(created->reference, "owner"));
    const auto output_root = directory_ / "workspace";
    std::filesystem::create_directories(output_root);
    const auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", output_root, true}});
    ASSERT_TRUE(sandbox);
    const axk::app::FileRef destination{"workspace", "saved/manifest.json"};
    std::filesystem::create_directories(output_root / "saved");
    const auto materialized = value.materialize(created->reference, "owner", *sandbox, destination, false);
    ASSERT_TRUE(materialized) << materialized.error().message;
    std::ifstream input{output_root / "saved" / "manifest.json"};
    std::string content;
    input >> content;
    EXPECT_EQ(content, "{}");
    EXPECT_TRUE(value.inspect(created->reference, "owner"));
    EXPECT_FALSE(value.materialize(created->reference, "owner", *sandbox, destination, false));
}

TEST_F(UploadStoreTest, PartialUploadCannotReplacePersistentDestination) {
    auto value = store();
    const auto created = value.create({.owner_id = "owner",
                                       .filename = "manifest.json",
                                       .kind = axk::app::UploadKind::manifest,
                                       .media_type = "application/json",
                                       .declared_size = 4U,
                                       .sha256 = std::nullopt});
    ASSERT_TRUE(created) << created.error().message;
    ASSERT_TRUE(value.append(created->reference, "owner", 0U, bytes("{")));

    const auto output_root = directory_ / "workspace";
    std::filesystem::create_directories(output_root / "saved");
    const auto destination_path = output_root / "saved" / "manifest.json";
    std::ofstream(destination_path) << "sentinel";
    const auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", output_root, true}});
    ASSERT_TRUE(sandbox);

    const auto materialized =
        value.materialize(created->reference, "owner", *sandbox, {"workspace", "saved/manifest.json"}, true);
    ASSERT_FALSE(materialized);
    EXPECT_EQ(materialized.error().code, "upload_not_ready");
    std::ifstream input{destination_path};
    std::string content;
    input >> content;
    EXPECT_EQ(content, "sentinel");
}

} // namespace
