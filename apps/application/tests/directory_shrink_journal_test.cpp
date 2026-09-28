#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/alteration_transaction.hpp"
#include "axklib/application/alteration_journal.hpp"
#include "axklib/audio.hpp"
#include "axklib/bytes.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

class DirectoryShrinkJournal : public testing::TestWithParam<bool> {
  protected:
    std::filesystem::path root;
    axk::SfsId directory_id;

    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               (GetParam() ? "axklib-shrink-journal-cancel" : "axklib-shrink-journal-failure");
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root / "workspace");
        axk::Waveform wave;
        wave.format = {1U, 2U, 44100U};
        wave.frame_count = 4U;
        wave.pcm.resize(8U);
        ASSERT_TRUE(axk::write_wav_atomic(root / "tone.wav", wave));
        axk::VolumeSpec volume;
        volume.name = "Samples";
        volume.waveforms.push_back({"wave", "Wave", root / "tone.wav", 60U, {}});
        for (std::size_t i = 0; i < 128U; ++i) {
            axk::SampleSpec sample;
            sample.name = std::format("S{:03}", i);
            sample.waveform_id = "wave";
            volume.samples.push_back(std::move(sample));
        }
        const auto path = root / "workspace/image.hds";
        ASSERT_TRUE(axk::write_hds_image({"1.0", 4U * 1024U * 1024U, {{"Test", {volume}}}}, path));
        std::uint64_t offset{};
        std::array<std::byte, 72U> raw{};
        {
            auto opened = axk::open_image(path);
            ASSERT_TRUE(opened);
            const auto &records = opened->partitions()[0].records;
            const auto found = std::ranges::find_if(records, [](const auto &record) {
                return record.payload_kind == axk::PayloadKind::directory &&
                       std::ranges::any_of(record.directory_entries,
                                           [](const auto &entry) { return entry.name == "S000"; });
            });
            ASSERT_NE(found, records.end());
            ASSERT_EQ(found->data_size, 4160U);
            ASSERT_EQ(found->extents.size(), std::size_t{1U});
            directory_id = found->sfs_id;
            offset = found->record_offset.value;
            auto reader = axk::FileReader::open(path);
            ASSERT_TRUE(reader);
            ASSERT_TRUE((*reader)->read_exact_at(offset, raw));
            axk::ByteWriter index{raw};
            ASSERT_TRUE(index.write_be16(0U, 3U));
            const auto start = found->extents[0].cluster_offset;
            const std::array extents{axk::Extent{start, 2U, 2048U}, axk::Extent{start + 2U, 2U, 2048U},
                                     axk::Extent{start + 4U, 1U, 64U}};
            for (std::size_t i = 0; i < extents.size(); ++i) {
                ASSERT_TRUE(index.write_be32(0x0aU + 12U * i, extents[i].cluster_offset));
                ASSERT_TRUE(index.write_be32(0x0eU + 12U * i, extents[i].cluster_count));
                ASSERT_TRUE(index.write_be32(0x12U + 12U * i, extents[i].byte_count));
            }
        }
        std::fstream output{path, std::ios::in | std::ios::out | std::ios::binary};
        output.seekp(static_cast<std::streamoff>(offset));
        output.write(reinterpret_cast<const char *>(raw.data()), static_cast<std::streamsize>(raw.size()));
        ASSERT_TRUE(output);
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    std::vector<char> bytes() const {
        std::ifstream input{root / "workspace/image.hds", std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, {}};
    }
};

TEST_P(DirectoryShrinkJournal, RestoresPayloadIndexBitmapAndHeaderWhenCommitDoesNotComplete) {
    const auto original = bytes();
    const auto path = root / "workspace/image.hds";
    const axk::AlterationManifest manifest{
        "1.0",
        {{"first", axk::DeleteSampleOperation{axk::PartitionIndex{0U}, "Samples", "S000"}},
         {"second", axk::DeleteSampleOperation{axk::PartitionIndex{0U}, "Samples", "S001"}}}};
    auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
    ASSERT_TRUE(sandbox);
    auto target = sandbox->open_mutation({"workspace", "image.hds"});
    ASSERT_TRUE(target);
    auto prepared = axk::detail::prepare_hds_alteration(*target, path, manifest);
    ASSERT_TRUE(prepared) << prepared.error().message;
    std::vector<axk::app::AlterationJournalPatch> patches;
    for (auto &patch : prepared->patches)
        patches.push_back({patch.offset, std::move(patch.original), std::move(patch.replacement)});
    axk::CancellationSource cancellation;
    axk::app::AlterationJournalStore store{root / "journals"};
    bool validated{};
    const auto result = store.apply(
        *target, prepared->image_size_bytes, patches, cancellation.token(), [&]() -> axk::app::Result<void> {
            validated = true;
            auto opened = axk::open_image(path);
            EXPECT_TRUE(opened);
            if (opened) {
                const auto &partition = opened->partitions()[0];
                EXPECT_TRUE(axk::allocation_is_safe_for_mutation(partition.allocation));
                const auto record = std::ranges::find(partition.records, directory_id, &axk::IndexRecord::sfs_id);
                EXPECT_NE(record, partition.records.end());
                if (record != partition.records.end()) {
                    EXPECT_EQ(record->data_size, 4096U);
                    EXPECT_EQ(record->extents.size(), std::size_t{2U});
                }
            }
            if (GetParam()) {
                cancellation.cancel();
                return {};
            }
            return std::unexpected(axk::app::Error{"test_failure", "Reject commit after directory shrink"});
        });
    EXPECT_TRUE(validated);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, std::string{GetParam() ? "operation_cancelled" : "test_failure"});
    EXPECT_EQ(bytes(), original);
    EXPECT_TRUE(store.storage_ready());
    for (const auto &entry : std::filesystem::directory_iterator{root / "journals"}) {
        EXPECT_EQ(entry.path().filename(), std::filesystem::path{".axklib-publication"});
        EXPECT_TRUE(std::filesystem::is_empty(entry.path()));
    }
}

INSTANTIATE_TEST_SUITE_P(FailureAndCancellation, DirectoryShrinkJournal, testing::Bool());

} // namespace
