#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "../src/alteration_internal.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/writer.hpp"
#include "axklib/writer_internal.hpp"

namespace {

class RecordResize : public testing::Test {
  protected:
    axk::alteration_internal::TransactionState state;
    axk::Partition source;
    axk::alteration_internal::MutablePartition partition;
    const axk::SfsId id{10U};

    void SetUp() override {
        source.cluster_count = 4096U;
        partition.source = &source;
        partition.bitmap.resize(512U);
    }

    void prepare(std::vector<axk::Extent> extents, std::uint32_t size, std::vector<std::uint32_t> lists = {}) {
        ASSERT_TRUE(axk::alteration_internal::normalize_extent_byte_counts(extents, size));
        axk::detail::PreparedRecord record;
        record.kind = axk::detail::RecordKind::directory;
        record.tail = 0x1234U;
        auto raw = axk::detail::encode_sfs_index_record(record, extents, size, lists);
        ASSERT_TRUE(raw) << raw.error().message;
        (*raw)[2] = std::byte{0x5a};
        for (const auto &extent : extents)
            for (std::uint32_t i = 0; i < extent.cluster_count; ++i)
                axk::alteration_internal::set_bitmap(partition.bitmap, extent.cluster_offset + i, true);
        for (const auto cluster : lists)
            axk::alteration_internal::set_bitmap(partition.bitmap, cluster, true);
        partition.inserted.emplace(id, axk::alteration_internal::MutablePartition::InsertedRecord{
                                           id, std::move(*raw), std::vector<std::byte>(size, std::byte{0x31}),
                                           std::move(extents), std::move(lists), axk::PayloadKind::directory});
    }

    auto resize(std::size_t size) {
        return axk::alteration_internal::replace_record_payload(state, partition, id,
                                                                std::vector<std::byte>(size, std::byte{0x31}), {});
    }
};

TEST_F(RecordResize, RemovesEmptyTrailingExtentAtExactDirectoryBoundary) {
    prepare({{100U, 2U, 0U}, {80U, 2U, 0U}, {200U, 1U, 0U}}, 4160U);
    ASSERT_TRUE(resize(4128U));
    EXPECT_EQ(partition.inserted.at(id).extents.size(), std::size_t{3U});
    const auto resized = resize(4096U);
    ASSERT_TRUE(resized) << resized.error().message;
    const auto &record = partition.inserted.at(id);
    ASSERT_EQ(record.extents.size(), std::size_t{2U});
    EXPECT_EQ(record.extents[0].cluster_offset, 100U);
    EXPECT_EQ(record.extents[1].cluster_offset, 80U);
    EXPECT_FALSE(axk::alteration_internal::bitmap_used(partition.bitmap, 200U));
    EXPECT_TRUE(axk::alteration_internal::bitmap_used(partition.bitmap, 81U));
    const axk::ByteReader index{record.raw_index};
    EXPECT_EQ(*index.be16(0U), std::uint16_t{2U});
    EXPECT_EQ(*index.be16(4U), std::uint16_t{4U});
    EXPECT_EQ(*index.be32(6U), 4096U);
    EXPECT_EQ(*index.be32(0x1eU), 2048U);
    EXPECT_TRUE(std::ranges::all_of(std::span{record.raw_index}.subspan(0x22U, 24U),
                                    [](std::byte byte) { return byte == std::byte{0}; }));
    EXPECT_EQ(record.raw_index[2], std::byte{0x5a});
    EXPECT_EQ(*index.be16(0x46U), std::uint16_t{0x1234U});
}

TEST_F(RecordResize, RetainsSlackInFinalExtentAndReleasesMultipleTrailingExtents) {
    prepare({{100U, 2U, 0U}, {80U, 2U, 0U}, {200U, 1U, 0U}}, 4160U);
    const auto resized = resize(64U);
    ASSERT_TRUE(resized) << resized.error().message;
    const auto &record = partition.inserted.at(id);
    ASSERT_EQ(record.extents.size(), std::size_t{1U});
    EXPECT_EQ(record.extents[0].cluster_count, 2U);
    EXPECT_EQ(record.extents[0].byte_count, 64U);
    EXPECT_TRUE(axk::alteration_internal::bitmap_used(partition.bitmap, 101U));
    EXPECT_FALSE(axk::alteration_internal::bitmap_used(partition.bitmap, 80U));
    EXPECT_FALSE(axk::alteration_internal::bitmap_used(partition.bitmap, 200U));
}

TEST_F(RecordResize, ConvertsContinuationDirectoryBackToDirectExtents) {
    prepare({{100U, 1U, 0U}, {80U, 1U, 0U}, {200U, 1U, 0U}, {90U, 1U, 0U}, {210U, 1U, 0U}}, 4128U, {300U});
    auto &index_bytes = partition.inserted.at(id).raw_index;
    std::fill(index_bytes.begin() + 0x3a, index_bytes.end(), std::byte{0x5c});
    const auto original_index = index_bytes;
    const auto resized = resize(4096U);
    ASSERT_TRUE(resized) << resized.error().message;
    const auto &record = partition.inserted.at(id);
    EXPECT_EQ(record.extents.size(), std::size_t{4U});
    EXPECT_TRUE(record.continuation_clusters.empty());
    EXPECT_FALSE(axk::alteration_internal::bitmap_used(partition.bitmap, 300U));
    EXPECT_FALSE(axk::alteration_internal::bitmap_used(partition.bitmap, 210U));
    EXPECT_EQ(*axk::ByteReader{record.raw_index}.be32(0x0aU), 100U);
    EXPECT_TRUE(std::equal(original_index.begin() + 0x3a, original_index.end(), record.raw_index.begin() + 0x3a));
}

TEST_F(RecordResize, ExpandedPublicationPreservesOpaqueIndexMetadata) {
    prepare({{100U, 2U, 0U}}, 64U);
    auto &record = partition.inserted.at(id);
    std::fill(record.raw_index.begin() + 0x3a, record.raw_index.end(), std::byte{0x67});
    record.extents.push_back({200U, 1U, 32U});
    record.extents.front().byte_count = 2048U;
    record.capacity_expanded = true;
    const auto encoded = axk::alteration_internal::encode_changed_record_index(record, record.extents, 2080U);
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ((*encoded)[2], record.raw_index[2]);
    EXPECT_TRUE(std::equal(record.raw_index.begin() + 0x3a, record.raw_index.end(), encoded->begin() + 0x3a));
}

TEST_F(RecordResize, PreservesReservedGrowthUntilPayloadActuallyShrinks) {
    prepare({{100U, 2U, 0U}}, 64U);
    auto &record = partition.inserted.at(id);
    record.extents.push_back({200U, 2U, 0U});
    record.capacity_expanded = true;
    axk::alteration_internal::set_bitmap(partition.bitmap, 200U, true);
    axk::alteration_internal::set_bitmap(partition.bitmap, 201U, true);
    ASSERT_TRUE(resize(96U));
    EXPECT_EQ(record.extents.size(), std::size_t{2U});
    ASSERT_TRUE(resize(2080U));
    ASSERT_TRUE(resize(2048U));
    EXPECT_EQ(record.extents.size(), std::size_t{1U});
    EXPECT_FALSE(axk::alteration_internal::bitmap_used(partition.bitmap, 200U));
}

TEST_F(RecordResize, ReleasesSurplusContinuationListsAndKeepsTheRemainingChain) {
    std::vector<axk::Extent> extents;
    for (std::uint32_t i = 0; i < 86U; ++i)
        extents.push_back({100U + i * 2U, 1U, 0U});
    prepare(std::move(extents), 85U * 1024U + 32U, {300U, 301U});
    const auto resized = resize(84U * 1024U);
    ASSERT_TRUE(resized) << resized.error().message;
    const auto &record = partition.inserted.at(id);
    ASSERT_EQ(record.extents.size(), std::size_t{84U});
    EXPECT_EQ(record.continuation_clusters, (std::vector<std::uint32_t>{300U}));
    EXPECT_FALSE(axk::alteration_internal::bitmap_used(partition.bitmap, 301U));
    EXPECT_EQ(state.resized_record_freed_clusters, std::uint64_t{3U});
    auto list = axk::alteration_internal::continuation_list_bytes(record, 0U);
    ASSERT_TRUE(list);
    EXPECT_EQ(*axk::ByteReader{*list}.be32(8U), 0U);
    EXPECT_EQ(*axk::ByteReader{*list}.be32(0U), 84U);
}

TEST_F(RecordResize, CancellationDoesNotReleaseAnyExtent) {
    prepare({{100U, 2U, 0U}, {80U, 2U, 0U}, {200U, 1U, 0U}}, 4160U);
    const auto bitmap = partition.bitmap;
    axk::CancellationSource cancel;
    cancel.cancel();
    EXPECT_FALSE(axk::alteration_internal::replace_record_payload(state, partition, id, std::vector<std::byte>(4096U),
                                                                  cancel.token()));
    EXPECT_EQ(partition.bitmap, bitmap);
    EXPECT_EQ(state.resized_record_freed_clusters, std::uint64_t{0U});
}

class DirectoryShrink : public testing::Test {
  protected:
    std::filesystem::path root;
    axk::SfsId directory_id;
    std::uint32_t trailing_cluster{};

    void SetUp() override {
        root = std::filesystem::temp_directory_path() / (std::string{"axklib-directory-shrink-"} +
                                                         testing::UnitTest::GetInstance()->current_test_info()->name());
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
        axk::Waveform wave;
        wave.format = {1U, 2U, 44100U};
        wave.frame_count = 4U;
        wave.pcm.resize(8U);
        ASSERT_TRUE(axk::write_wav_atomic(root / "tone.wav", wave));
        axk::VolumeSpec volume;
        volume.name = "Banks";
        volume.waveforms.push_back({"wave", "Wave", root / "tone.wav", 60U, {}});
        for (std::size_t i = 0; i < 128U; ++i) {
            axk::SampleSpec sample;
            sample.name = std::format("S{:03}", i);
            sample.waveform_id = "wave";
            volume.sample_banks.push_back({std::format("B{:03}", i), {sample.name}});
            volume.samples.push_back(std::move(sample));
        }
        volume.programs.push_back({1U, "Delete", {{"SBAC", "B000", {}}, {"SBAC", "B001", {}}}});
        volume.programs.push_back({2U, "Keep", {{"SBAC", "B002", {}}}});
        ASSERT_TRUE(axk::write_hds_image({"1.0", 4U * 1024U * 1024U, {{"Test", {volume}}}}, root / "source.hds"));
        std::uint64_t offset{};
        std::vector<std::byte> raw;
        {
            auto opened = axk::open_image(root / "source.hds");
            ASSERT_TRUE(opened);
            const auto &records = opened->partitions()[0].records;
            const auto found = std::ranges::find_if(records, [](const auto &record) {
                return record.payload_kind == axk::PayloadKind::directory &&
                       std::ranges::any_of(record.directory_entries,
                                           [](const auto &entry) { return entry.name == "B000"; });
            });
            ASSERT_NE(found, records.end());
            ASSERT_EQ(found->data_size, 4160U);
            ASSERT_EQ(found->extents.size(), std::size_t{1U});
            directory_id = found->sfs_id;
            offset = found->record_offset.value;
            auto original = axk::alteration_internal::read_raw(root / "source.hds", offset, 72U);
            ASSERT_TRUE(original);
            raw = std::move(*original);
            axk::ByteWriter index{raw};
            ASSERT_TRUE(index.write_be16(0U, 3U));
            const auto start = found->extents[0].cluster_offset;
            const std::array extents{axk::Extent{start, 2U, 2048U}, axk::Extent{start + 2U, 2U, 2048U},
                                     axk::Extent{start + 4U, 1U, 64U}};
            trailing_cluster = start + 4U;
            for (std::size_t i = 0; i < extents.size(); ++i) {
                ASSERT_TRUE(index.write_be32(0x0aU + 12U * i, extents[i].cluster_offset));
                ASSERT_TRUE(index.write_be32(0x0eU + 12U * i, extents[i].cluster_count));
                ASSERT_TRUE(index.write_be32(0x12U + 12U * i, extents[i].byte_count));
            }
        }
        std::fstream output{root / "source.hds", std::ios::in | std::ios::out | std::ios::binary};
        output.seekp(static_cast<std::streamoff>(offset));
        output.write(reinterpret_cast<const char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
        ASSERT_TRUE(output);
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    axk::AlterationManifest deletion() const {
        return {"1.0",
                {{"program", axk::DeleteProgramOperation{axk::PartitionIndex{0U}, "Banks", 1U}},
                 {"bank0", axk::DeleteSampleBankOperation{axk::PartitionIndex{0U}, "Banks", "B000"}},
                 {"bank1", axk::DeleteSampleBankOperation{axk::PartitionIndex{0U}, "Banks", "B001"}}}};
    }

    std::vector<char> source_bytes() const {
        std::ifstream input{root / "source.hds", std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, {}};
    }
};

TEST_F(DirectoryShrink, DeletesAcrossBoundaryAndCanDeleteAgainAfterReopening) {
    const auto original = source_bytes();
    auto applied = axk::alter_hds(root / "source.hds", deletion(), root / "deleted.hds");
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(source_bytes(), original);
    ASSERT_EQ(applied->operations.size(), std::size_t{3U});
    EXPECT_EQ(applied->operations[2].freed_clusters, applied->operations[1].freed_clusters + 1U);
    {
        auto opened = axk::open_image(root / "deleted.hds");
        ASSERT_TRUE(opened);
        const auto &partition = opened->partitions()[0];
        EXPECT_TRUE(axk::allocation_is_safe_for_mutation(partition.allocation));
        const auto directory = std::ranges::find(partition.records, directory_id, &axk::IndexRecord::sfs_id);
        ASSERT_NE(directory, partition.records.end());
        EXPECT_EQ(directory->data_size, 4096U);
        EXPECT_EQ(directory->extents.size(), std::size_t{2U});
    }
    const axk::AlterationManifest next{
        "1.0",
        {{"program2", axk::DeleteProgramOperation{axk::PartitionIndex{0U}, "Banks", 2U}},
         {"bank2", axk::DeleteSampleBankOperation{axk::PartitionIndex{0U}, "Banks", "B002"}}}};
    auto again = axk::alter_hds(root / "deleted.hds", next, root / "again.hds");
    ASSERT_TRUE(again) << again.error().message;
}

TEST_F(DirectoryShrink, FailureAfterShrinkPublishesNothing) {
    const auto original = source_bytes();
    auto manifest = deletion();
    manifest.operations.push_back({"missing", axk::DeleteProgramOperation{axk::PartitionIndex{0U}, "Banks", 99U}});
    EXPECT_FALSE(axk::alter_hds(root / "source.hds", manifest, root / "failed.hds"));
    EXPECT_FALSE(std::filesystem::exists(root / "failed.hds"));
    EXPECT_EQ(source_bytes(), original);
}

TEST_F(DirectoryShrink, ReusesReleasedDirectoryStorageWithinTheSameTransaction) {
    auto manifest = deletion();
    manifest.operations.push_back(
        {"bank", axk::InsertSampleBankOperation{axk::PartitionIndex{0U}, "Banks", {"Added", {"S000"}}}});
    auto applied = axk::alter_hds(root / "source.hds", manifest, root / "reused.hds");
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_EQ(applied->operations.back().freed_clusters, std::uint64_t{0U});
    auto opened = axk::open_image(root / "reused.hds");
    ASSERT_TRUE(opened);
    EXPECT_TRUE(axk::allocation_is_safe_for_mutation(opened->partitions()[0].allocation));
    const auto &records = opened->partitions()[0].records;
    const auto added = std::ranges::find(records, directory_id, &axk::IndexRecord::sfs_id);
    ASSERT_NE(added, records.end());
    EXPECT_TRUE(std::ranges::any_of(added->extents, [&](const auto &extent) {
        return trailing_cluster >= extent.cluster_offset &&
               trailing_cluster < extent.cluster_offset + extent.cluster_count;
    }));
}

} // namespace
