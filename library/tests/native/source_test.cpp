#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../media_test_fixtures.hpp"
#include "axklib/filesystems/fat/view.hpp"
#include "axklib/formats/tx16w/document.hpp"
#include "axklib/native/source.hpp"

namespace {

axk::native::SourceMember member(std::string id) {
    auto bytes = std::make_shared<axk::MemoryReader>(std::vector<std::byte>{std::byte{1}, std::byte{2}});
    return {std::move(id), "disk.img", bytes, {{"fat", "FAT12", {{"file", "SAME.W01", false, 0U, bytes}}}}};
}

class CancellingReader final : public axk::RandomAccessReader {
  public:
    explicit CancellingReader(axk::CancellationSource &cancellation) : cancellation_(cancellation) {}
    std::uint64_t size() const noexcept override { return axk::native::SourceSet::maximum_read_bytes + 1U; }
    axk::Result<void> read_exact_at(std::uint64_t, std::span<std::byte> destination) const override {
        ++reads;
        std::ranges::fill(destination, std::byte{});
        cancellation_.cancel();
        return {};
    }
    mutable std::size_t reads{};

  private:
    axk::CancellationSource &cancellation_;
};

} // namespace

TEST(NativeSources, KeepsIdenticalFilesInSeparateMembers) {
    auto sources = axk::native::SourceSet::create({member("first"), member("second")});
    ASSERT_TRUE(sources);
    EXPECT_EQ((*sources)->members().size(), 2U);
    const auto first = (*sources)->locate("first", "fat", "file");
    const auto second = (*sources)->locate("second", "fat", "file");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_NE(*first, *second);
    EXPECT_EQ((*sources)->read(*first), (*sources)->read(*second));
}

TEST(NativeSources, RejectsEmptyMissingAndDuplicateIdentities) {
    EXPECT_FALSE(axk::native::SourceSet::create({}));
    EXPECT_FALSE(axk::native::SourceSet::create({member("")}));
    EXPECT_FALSE(axk::native::SourceSet::create({member("a"), member("a")}));
    auto duplicate = member("a");
    duplicate.roots.front().entries.push_back(duplicate.roots.front().entries.front());
    EXPECT_FALSE(axk::native::SourceSet::create({duplicate}));
    duplicate = member("a");
    duplicate.reader.reset();
    duplicate.roots.clear();
    EXPECT_FALSE(axk::native::SourceSet::create({duplicate}));
}

TEST(NativeSources, EnforcesAggregateMemberAndEntryLimits) {
    std::vector<axk::native::SourceMember> members;
    for (std::size_t index = 0U; index < axk::native::SourceSet::maximum_members; ++index)
        members.push_back(member(std::to_string(index)));
    EXPECT_TRUE(axk::native::SourceSet::create(members));
    members.push_back(member("extra"));
    EXPECT_FALSE(axk::native::SourceSet::create(std::move(members)));

    auto root = member("root");
    auto &entries = root.roots.front().entries;
    entries.clear();
    for (std::size_t index = 0U; index < axk::native::SourceSet::maximum_entries; ++index)
        entries.push_back({std::to_string(index), "directory", true, 0U, {}});
    EXPECT_TRUE(axk::native::SourceSet::create({root}));
    EXPECT_FALSE(axk::native::SourceSet::create({std::move(root), member("extra")}));
}

TEST(NativeSources, RejectsOversizedMaterializationAndCancelsBetweenReadChunks) {
    axk::CancellationSource cancellation;
    auto reader = std::make_shared<CancellingReader>(cancellation);
    const auto sources = axk::native::SourceSet::create({{"large", "large.img", reader, {}}});
    ASSERT_TRUE(sources);
    auto location = (*sources)->locate("large", "", "");
    ASSERT_TRUE(location);
    const auto oversized = (*sources)->read(*location);
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().code, axk::ErrorCode::io_unsupported_size);
    EXPECT_EQ(reader->reads, 0U);

    location->length = 64U * 1024U + 1U;
    const auto cancelled = (*sources)->read(*location, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_EQ(reader->reads, 1U);
}

TEST(NativeSources, SupportsDirectoryMembersWithoutAnInventedRawReader) {
    auto directory = member("directory");
    directory.reader.reset();
    const auto sources = axk::native::SourceSet::create({std::move(directory)});
    ASSERT_TRUE(sources);
    EXPECT_TRUE((*sources)->locate("directory", "fat", "file"));
    EXPECT_FALSE((*sources)->locate("directory", "", ""));
}

TEST(NativeSources, VerifiesSourceIdentityBeforeAndAfterReading) {
    auto source = member("mutable");
    auto checks = std::make_shared<std::size_t>(0U);
    source.verify_unchanged = [checks]() -> axk::Result<void> {
        if (++*checks >= 2U)
            return std::unexpected(axk::make_error(axk::ErrorCode::transaction_stale, axk::ErrorCategory::io,
                                                   "source changed during the read"));
        return {};
    };
    const auto sources = axk::native::SourceSet::create({std::move(source)});
    ASSERT_TRUE(sources);
    const auto location = (*sources)->locate("mutable", "fat", "file");
    ASSERT_TRUE(location);
    const auto read = (*sources)->read(*location);
    ASSERT_FALSE(read);
    EXPECT_EQ(read.error().code, axk::ErrorCode::transaction_stale);
    EXPECT_EQ(*checks, 2U);
}

TEST(NativeSources, BoundsReadsToTheSelectedEntryAndChecksCancellation) {
    auto sources = axk::native::SourceSet::create({member("a")});
    ASSERT_TRUE(sources);
    auto location = (*sources)->locate("a", "fat", "file");
    ASSERT_TRUE(location);
    location->offset = 1U;
    location->length = 1U;
    EXPECT_EQ((*sources)->read(*location), std::vector<std::byte>{std::byte{2}});
    location->length = 2U;
    const auto overflow = (*sources)->read(*location);
    ASSERT_FALSE(overflow);
    EXPECT_EQ(overflow.error().code, axk::ErrorCode::out_of_bounds);
    location->length = 1U;
    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto cancelled = (*sources)->read(*location, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_FALSE((*sources)->locate("a", "fat", "missing"));
    EXPECT_FALSE((*sources)->locate("b", "fat", "file"));
}

TEST(NativeSources, FatViewRetainsDirectoriesAttributesAndPhysicalEntryIdentity) {
    auto bytes = std::make_shared<axk::MemoryReader>(nested_fat_fixture());
    auto root = axk::filesystems::open_fat(bytes, "nested.img");
    ASSERT_TRUE(root) << root.error().message;
    ASSERT_EQ(root->entries.size(), 2U);
    const auto directory = std::ranges::find(root->entries, "OBJECTS", &axk::filesystems::Entry::path);
    ASSERT_NE(directory, root->entries.end());
    EXPECT_TRUE(directory->directory);
    EXPECT_EQ(directory->attributes, 0x10U);
    EXPECT_EQ(directory->id, "1536");
    EXPECT_FALSE(directory->reader);
    const auto file = std::ranges::find(root->entries, "OBJECTS/SMPTEST.004", &axk::filesystems::Entry::path);
    ASSERT_NE(file, root->entries.end());
    EXPECT_FALSE(file->directory);
    EXPECT_EQ(file->attributes, 0x20U);
    EXPECT_EQ(file->id, "2048");
    auto sources = axk::native::SourceSet::create({{"disk", "nested.img", bytes, {std::move(*root)}}});
    ASSERT_TRUE(sources);
    auto location = (*sources)->locate("disk", "fat", "2048");
    ASSERT_TRUE(location);
    EXPECT_EQ((*sources)->read(*location), smpl_object());
    EXPECT_FALSE((*sources)->locate("disk", "fat", "1536"));
}

TEST(NativeSources, AdmittedFatViewKeepsTheFilesystemAliveWithoutReadingAgain) {
    class CountingReader final : public axk::RandomAccessReader {
      public:
        std::uint64_t size() const noexcept override { return data.size(); }
        axk::Result<void> read_exact_at(std::uint64_t offset, std::span<std::byte> destination) const override {
            ++reads;
            return data.read_exact_at(offset, destination);
        }
        axk::MemoryReader data{nested_fat_fixture()};
        mutable std::size_t reads{};
    };
    auto reader = std::make_shared<CountingReader>();
    auto opened = axk::FatImage::open(reader, "nested.img");
    ASSERT_TRUE(opened);
    auto disk = std::make_shared<const axk::FatImage>(std::move(*opened));
    const auto reads = reader->reads;
    auto root = axk::filesystems::view_fat(disk);
    ASSERT_TRUE(root);
    EXPECT_EQ(reader->reads, reads);
    disk.reset();
    const auto file = std::ranges::find(root->entries, "2048", &axk::filesystems::Entry::id);
    ASSERT_NE(file, root->entries.end());
    std::vector<std::byte> bytes(static_cast<std::size_t>(file->reader->size()));
    ASSERT_TRUE(file->reader->read_exact_at(0U, bytes));
    EXPECT_EQ(bytes, smpl_object());
    EXPECT_FALSE(axk::filesystems::view_fat({}));
}

TEST(NativeSources, Tx16wDiscoveryLimitDoesNotPreventLargerFilesystemAdmission) {
    auto source = member("disk");
    auto &entries = source.roots.front().entries;
    entries.clear();
    for (std::size_t index = 0U; index < 16'385U; ++index)
        entries.push_back({std::to_string(index), "directory", true, 0U, {}});
    auto sources = axk::native::SourceSet::create({std::move(source)});
    ASSERT_TRUE(sources);
    const auto recognized = axk::tx16w::provider().recognize(**sources, {});
    ASSERT_TRUE(recognized);
    EXPECT_EQ(recognized->state, axk::native::RecognitionState::limit_exceeded);
    ASSERT_EQ(recognized->diagnostics.size(), 1U);
    EXPECT_EQ(recognized->diagnostics.front().code, "tx16w.discovery-limit");
    EXPECT_FALSE(axk::tx16w::Document::open(*sources));
}
