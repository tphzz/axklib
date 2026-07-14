#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/io.hpp"
#include "axklib/package_archive.hpp"

namespace {

std::vector<std::byte> bytes(std::string_view value) {
    const auto source = std::as_bytes(std::span{value});
    return {source.begin(), source.end()};
}

std::vector<axk::package_internal::ArchiveEntry> entries() {
    return {{"payloads/sha256/abc.bin", bytes("payload")}, {"manifest.json", bytes("{\"schema_version\":\"1.0\"}\n")}};
}

class CountingReader final : public axk::RandomAccessReader {
  public:
    explicit CountingReader(std::vector<std::byte> source) : source_{std::move(source)} {}

    [[nodiscard]] std::uint64_t size() const noexcept override { return source_.size(); }

    [[nodiscard]] axk::Result<void> read_exact_at(std::uint64_t offset,
                                                  std::span<std::byte> destination) const override {
        bytes_read_ += destination.size();
        return source_.read_exact_at(offset, destination);
    }

    [[nodiscard]] std::uint64_t bytes_read() const noexcept { return bytes_read_; }

  private:
    axk::MemoryReader source_;
    mutable std::uint64_t bytes_read_{};
};

} // namespace

TEST(PackageDigest, MatchesPublishedSha256AndCrc32Vectors) {
    EXPECT_EQ(axk::package_internal::hex_digest(axk::package_internal::sha256(bytes(""))),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(axk::package_internal::hex_digest(axk::package_internal::sha256(bytes("abc"))),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(axk::package_internal::crc32(bytes("123456789")), 0xcbf43926U);
}

TEST(PackageDigest, StreamsReadersWithoutChangingTheDigestAndHonorsCancellation) {
    std::vector<std::byte> payload(2U * 1024U * 1024U + 37U);
    for (std::size_t index = 0; index < payload.size(); ++index)
        payload[index] = static_cast<std::byte>((index * 131U + 17U) & 0xffU);
    const axk::MemoryReader reader{payload};
    const auto streamed = axk::package_internal::sha256_reader(reader);
    ASSERT_TRUE(streamed) << streamed.error().message;
    EXPECT_EQ(*streamed, axk::package_internal::sha256(payload));

    axk::CancellationSource cancellation;
    cancellation.cancel();
    const auto cancelled = axk::package_internal::sha256_reader(reader, cancellation.token());
    ASSERT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error().code, axk::ErrorCode::operation_cancelled);
}

TEST(PackageArchive, IsDeterministicCanonicalAndRoundTrips) {
    const auto first = axk::package_internal::write_archive(entries());
    ASSERT_TRUE(first) << first.error().message;
    auto reversed = entries();
    std::ranges::reverse(reversed);
    const auto second = axk::package_internal::write_archive(std::move(reversed));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(*first, *second);

    const auto reopened = axk::package_internal::read_archive(*first);
    ASSERT_TRUE(reopened) << reopened.error().message;
    ASSERT_EQ(reopened->size(), 2U);
    EXPECT_EQ((*reopened)[0].path, "manifest.json");
    EXPECT_EQ((*reopened)[0].bytes, bytes("{\"schema_version\":\"1.0\"}\n"));
    EXPECT_EQ((*reopened)[1].path, "payloads/sha256/abc.bin");
    EXPECT_EQ((*reopened)[1].bytes, bytes("payload"));
}

TEST(PackageArchive, RejectsUnsafeDuplicateMissingManifestAndOversizedInputs) {
    auto unsafe = entries();
    unsafe.push_back({"payloads/../escape.bin", {}});
    EXPECT_FALSE(axk::package_internal::write_archive(std::move(unsafe)));

    auto duplicate = entries();
    duplicate.push_back(duplicate.back());
    EXPECT_FALSE(axk::package_internal::write_archive(std::move(duplicate)));

    EXPECT_FALSE(axk::package_internal::write_archive({{"payload.bin", {}}}));

    auto limits = axk::package_internal::ArchiveLimits{};
    limits.maximum_entry_bytes = 3U;
    EXPECT_FALSE(axk::package_internal::write_archive(entries(), limits));
    limits = {};
    limits.maximum_entries = 1U;
    EXPECT_FALSE(axk::package_internal::write_archive(entries(), limits));
}

TEST(PackageArchive, RejectsCorruptionTrailingDataAndNonProfileMetadata) {
    const auto valid = axk::package_internal::write_archive(entries());
    ASSERT_TRUE(valid) << valid.error().message;

    auto corrupt = *valid;
    constexpr std::size_t manifest_data_offset = 30U + std::string_view{"manifest.json"}.size();
    corrupt[manifest_data_offset] ^= std::byte{0x01};
    const auto bad_crc = axk::package_internal::read_archive(corrupt);
    ASSERT_FALSE(bad_crc);
    EXPECT_EQ(bad_crc.error().code, axk::ErrorCode::manifest_invalid);

    auto trailing = *valid;
    trailing.push_back(std::byte{});
    EXPECT_FALSE(axk::package_internal::read_archive(trailing));

    auto descriptor = *valid;
    descriptor[6U] |= std::byte{0x08};
    EXPECT_FALSE(axk::package_internal::read_archive(descriptor));

    auto truncated = *valid;
    truncated.resize(truncated.size() - 1U);
    EXPECT_FALSE(axk::package_internal::read_archive(truncated));
}

TEST(PackageArchive, AppliesReadLimitsBeforeMaterializingEntries) {
    const auto valid = axk::package_internal::write_archive(entries());
    ASSERT_TRUE(valid) << valid.error().message;
    auto limits = axk::package_internal::ArchiveLimits{};
    limits.maximum_archive_bytes = valid->size() - 1U;
    EXPECT_FALSE(axk::package_internal::read_archive(*valid, limits));
    limits = {};
    limits.maximum_entry_bytes = 3U;
    EXPECT_FALSE(axk::package_internal::read_archive(*valid, limits));
    limits = {};
    limits.maximum_total_bytes = 4U;
    EXPECT_FALSE(axk::package_internal::read_archive(*valid, limits));
}

TEST(PackageArchive, InspectionReadsOnlyBoundedMetadataAndManifest) {
    auto source_entries = entries();
    source_entries[0].bytes.resize(4U * 1024U * 1024U, std::byte{0x5a});
    const auto valid = axk::package_internal::write_archive(std::move(source_entries));
    ASSERT_TRUE(valid) << valid.error().message;

    CountingReader reader{*valid};
    const auto inspected = axk::package_internal::inspect_archive(reader);
    ASSERT_TRUE(inspected) << inspected.error().message;
    ASSERT_EQ(inspected->entries.size(), 2U);
    EXPECT_EQ(inspected->manifest.path, "manifest.json");
    EXPECT_EQ(inspected->manifest.bytes, bytes("{\"schema_version\":\"1.0\"}\n"));
    EXPECT_EQ(inspected->archive_size, valid->size());
    EXPECT_LT(reader.bytes_read(), 4096U);
    EXPECT_LT(reader.bytes_read(), inspected->entries[1].size);

    auto corrupt = *valid;
    const auto &payload = inspected->entries[1];
    corrupt[static_cast<std::size_t>(payload.data_offset)] ^= std::byte{0x01};
    CountingReader corrupt_reader{corrupt};
    EXPECT_TRUE(axk::package_internal::inspect_archive(corrupt_reader));
    EXPECT_FALSE(axk::package_internal::read_archive(corrupt));
}

TEST(PackageArchive, InspectionAppliesDirectoryAndManifestLimitsBeforeAllocation) {
    const auto valid = axk::package_internal::write_archive(entries());
    ASSERT_TRUE(valid) << valid.error().message;

    auto limits = axk::package_internal::ArchiveLimits{};
    limits.maximum_directory_bytes = 1U;
    CountingReader directory_limited{*valid};
    EXPECT_FALSE(axk::package_internal::inspect_archive(directory_limited, {}, limits));

    limits = {};
    limits.maximum_manifest_bytes = 1U;
    CountingReader manifest_limited{*valid};
    EXPECT_FALSE(axk::package_internal::inspect_archive(manifest_limited, {}, limits));
}
