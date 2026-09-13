#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/bytes.hpp"
#include "axklib/filesystem_edit.hpp"
#include "axklib/filesystem_transaction.hpp"
#include "axklib/system_file.hpp"
#include "axklib/system_file_write.hpp"
#include "axklib/writer.hpp"

namespace {

class SystemFileWrite : public testing::Test {
  protected:
    std::filesystem::path folder;
    void SetUp() override {
        folder = std::filesystem::temp_directory_path() /
                 std::format("axk-system-write-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directory(folder);
    }
    void TearDown() override {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    }
    static std::vector<std::byte> payload(bool native) {
        const auto size = native ? 0x400U : 0x1000U;
        std::vector<std::byte> bytes(size + 0x30U, std::byte{0xa5});
        axk::ByteWriter writer{bytes};
        EXPECT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
        EXPECT_TRUE(writer.write_ascii_field(12, 4, "PRF3", std::byte{}));
        EXPECT_TRUE(writer.write_be32(0x14, 4));
        EXPECT_TRUE(writer.write_be32(0x18, 0x36));
        EXPECT_TRUE(writer.write_be32(0x1c, size + 8U));
        EXPECT_TRUE(writer.write_be32(0x30, native ? 0x21520531U : 0xdeadfaceU));
        bytes[0x3e] = std::byte{};
        return bytes;
    }
    static std::vector<std::byte> read(const std::filesystem::path &path) {
        const auto file = axk::FileReader::open(path).value();
        std::vector<std::byte> bytes(static_cast<std::size_t>(file->size()));
        EXPECT_TRUE(file->read_exact_at(0, bytes));
        return bytes;
    }
    std::set<std::filesystem::path> retained_paths() const {
        std::set<std::filesystem::path> paths;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(folder))
            paths.insert(entry.path());
        return paths;
    }
    std::filesystem::path fixture(bool native, bool with_alias = false) {
        const auto blank = folder / "blank.hds";
        const auto source = folder / "source.hds";
        EXPECT_TRUE(axk::write_hds_image({"1.0", 8U * 1024U * 1024U, {{"Test", {}}}}, blank));
        std::vector<axk::FilesystemEdit> edits{
            axk::CreateFilesystemDirectory{{"META"}},
            axk::PutFilesystemFile{{"META", native ? "SYSTEM" : "SYSTEM2"},
                                   std::make_shared<axk::MemoryReader>(payload(native))},
        };
        if (with_alias)
            edits.emplace_back(
                axk::PutFilesystemFile{{"Alias"}, std::make_shared<axk::MemoryReader>(std::vector<std::byte>{})});
        EXPECT_TRUE(axk::write_sfs_file_edits(blank, source, axk::PartitionIndex{0}, edits));
        std::uint64_t name_offset{};
        {
            const auto image = axk::open_image(source).value();
            const auto &partition = image.partitions().front();
            const auto id = axk::locate_partition_root_record(partition).value();
            const auto &root = *std::ranges::find(partition.records, id, &axk::IndexRecord::sfs_id);
            const auto &entry = *std::ranges::find(root.directory_entries, "META", &axk::DirectoryEntry::name);
            name_offset = static_cast<std::uint64_t>(partition.start_sector) * image.superblock().sector_size_bytes +
                          static_cast<std::uint64_t>(root.extents.front().cluster_offset) *
                              partition.sectors_per_cluster * image.superblock().sector_size_bytes +
                          entry.payload_relative_offset + 8U;
        }
        // The normal Files API must not create PRF3; only this synthetic fixture does.
        std::fstream stream{source, std::ios::binary | std::ios::in | std::ios::out};
        stream.seekp(static_cast<std::streamoff>(name_offset));
        stream.write("PRF3", 4);
        EXPECT_TRUE(stream);
        return source;
    }
};

TEST_F(SystemFileWrite, PublishesOnlyChangedPayloadBytesAndLeavesAllocationAndSourceUnchanged) {
    for (const auto model : {axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const auto native = model == axk::ASeriesModel::a3000;
        const auto source = fixture(native);
        const auto original = read(source);
        const auto output = folder / "result.hds";
        axk::SystemFilePatch patches;
        patches.global.master_fine_tune = -5;
        patches.favorites.push_back({1, {10, 10, {}, {}}});
        patches.panel.end_type = native ? axk::SystemEndType::graph : axk::SystemEndType::beat;
        patches.panel.format_drive_id = native ? 7 : 9;
        patches.panel.format_type = native ? axk::SystemFormatType::floppy_2dd : axk::SystemFormatType::physical;
        patches.panel.effect_edit_mode = axk::SystemEffectEditMode::favorite;
        patches.panel.knob_control_types = {axk::SystemKnobControlType::step_3, axk::SystemKnobControlType::step_2,
                                            axk::SystemKnobControlType::step_1, axk::SystemKnobControlType::off};
        patches.panel.assignable_key_function = axk::SystemAssignableKeyFunction::controller_reset;
        patches.panel.audition_trigger_mode = axk::SystemAuditionTriggerMode::toggle;
        patches.panel.knob_1_type = axk::SystemKnob1Type::sample;
        patches.panel.function_selection = axk::SystemFunctionSelection::hold;
        patches.panel.page_selection = axk::SystemPageSelection::last;
        patches.panel.note_display = axk::SystemNoteDisplay::number;
        patches.panel.layer_selection_scope =
            native ? axk::SystemLayerSelectionScope::selection_page : axk::SystemLayerSelectionScope::tree_page;
        if (native) {
            patches.panel.sample_name_order_selection = 2;
            patches.panel.program_on_placement = axk::SystemProgramOnPlacement::mixed;
            patches.panel.bank_member_visibility = axk::SystemBankMemberVisibility::show;
            patches.panel.audition_name_view = true;
            patches.panel.midi_to_sample_name_view = false;
        } else {
            patches.panel.sample_sort = axk::SystemSampleSort::receive_channel_and_name;
            patches.panel.tree_sort = axk::SystemStatusSort::name;
            patches.panel.sample_bank_sort = axk::SystemStatusSort::status_and_name;
            patches.panel.import_view = axk::SystemImportView::sample;
            patches.panel.cdr_scsi_id = 7;
            patches.panel.cdr_write_speed = axk::SystemCdrWriteSpeed::x8;
        }
        patches.global.basic_receive_channel_selection = 15;
        patches.global.remix_type_selection = 4;
        patches.global.remix_variation_selection = native ? 3 : 7;
        patches.recording.effects[2].input_level = 77;
        patches.recording.configuration.input = 3;
        patches.recording.configuration.frequency_selection = 2;
        patches.recording.configuration.key_low = -1;
        patches.recording.configuration.key_high = 128;
        patches.recording.configuration.original_key = 64;
        patches.recording.configuration.click_tempo_hundredths = 12345;
        auto expected = original;
        {
            const auto image = axk::open_image(source).value();
            const auto &partition = image.partitions().front();
            const auto kind = native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2;
            const auto id = axk::locate_system_file_record(partition, kind).value().value();
            const auto &record = *std::ranges::find(partition.records, id, &axk::IndexRecord::sfs_id);
            const auto offset =
                static_cast<std::uint64_t>(partition.start_sector) * image.superblock().sector_size_bytes +
                static_cast<std::uint64_t>(record.extents.front().cluster_offset) * partition.sectors_per_cluster *
                    image.superblock().sector_size_bytes;
            expected[static_cast<std::size_t>(offset) + 0x50U + (native ? 0x110U : 0x320U) + 80U + 1U] = std::byte{77};
            const auto configuration = static_cast<std::size_t>(offset) + 0x50U + (native ? 0x188U : 0x398U);
            expected[static_cast<std::size_t>(offset) + 0x60U] = std::byte{251};
            expected[static_cast<std::size_t>(offset) + 0x64U] = std::byte{15};
            expected[static_cast<std::size_t>(offset) + 0x84U] = native ? std::byte{0x43} : std::byte{0x47};
            expected[static_cast<std::size_t>(offset) + (native ? 0xa2U : 0x232U)] = std::byte{0xaa};
            expected[static_cast<std::size_t>(offset) + (native ? 0x12fU : 0x33fU)] =
                native ? std::byte{4} : std::byte{3};
            const auto panel = static_cast<std::size_t>(offset) + (native ? 0x120U : 0x330U);
            for (std::size_t i = 0; i < 7U; ++i)
                expected[panel + 1U + i] = std::array{std::byte{1}, std::byte{4}, std::byte{3}, std::byte{2},
                                                      std::byte{0}, std::byte{2}, std::byte{1}}[i];
            expected[panel + 17U] = std::byte{1};
            expected[panel + 8U] = std::byte{2};
            expected[panel] = native ? std::byte{7} : std::byte{9};
            expected[panel + 11U] = native ? std::byte{2} : std::byte{1};
            expected[panel + 9U] = std::byte{1};
            expected[panel + 10U] = std::byte{1};
            expected[panel + 19U] = std::byte{1};
            expected[panel + (native ? 12U : 22U)] = std::byte{2};
            expected[panel + (native ? 13U : 23U)] = std::byte{1};
            expected[panel + (native ? 14U : 24U)] = native ? std::byte{1} : std::byte{2};
            if (native) {
                expected[panel + 18U] = std::byte{};
                expected[panel + 20U] = std::byte{1};
            } else {
                expected[panel + 16U] = std::byte{2};
                expected[panel + 25U] = std::byte{7};
                expected[panel + 26U] = std::byte{4};
            }
            expected[configuration + 1U] = std::byte{1};
            expected[configuration + 2U] = std::byte{3};
            expected[configuration + 3U] = std::byte{2};
            expected[configuration + 10U] = std::byte{255};
            expected[configuration + 11U] = std::byte{128};
            expected[configuration + 12U] = std::byte{64};
            expected[configuration + 17U] = std::byte{0};
            ASSERT_TRUE(axk::ByteWriter{expected}.write_be16(configuration + 20U, 12345));
        }
        const auto written = axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, model);
        ASSERT_TRUE(written) << written.error().message;
        EXPECT_EQ(read(output), expected);
        EXPECT_EQ(read(source), original);
        EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, model));
        EXPECT_EQ(read(output), expected);
        std::filesystem::remove(output);
        std::filesystem::remove(source);
        std::filesystem::remove(folder / "blank.hds");
    }
}

TEST_F(SystemFileWrite, InvalidAndCancelledEditsDoNotPublishOrBypassRawMetadataProtection) {
    const auto source = fixture(false);
    const auto original = read(source);
    const auto output = folder / "rejected.hds";
    axk::SystemFilePatch patches;
    patches.recording.effects[0].input_level = 77;
    patches.recording.configuration.pre_trigger_time = 5;
    patches.recording.effects[2].type = 255;
    EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a4000));
    EXPECT_FALSE(std::filesystem::exists(output));
    patches.recording.effects[2] = {};
    patches.recording.configuration.click_beat = 0;
    EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a4000));
    EXPECT_FALSE(std::filesystem::exists(output));
    patches.recording.configuration.click_beat.reset();
    patches.global.master_fine_tune = 64;
    EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a4000));
    EXPECT_FALSE(std::filesystem::exists(output));
    patches.global.master_fine_tune = 7;
    patches.panel.end_type = axk::SystemEndType::graph;
    EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a4000));
    EXPECT_FALSE(std::filesystem::exists(output));
    patches.panel.end_type = axk::SystemEndType::beat;
    patches.panel.knob_control_types[3] = static_cast<axk::SystemKnobControlType>(5);
    EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a4000));
    EXPECT_FALSE(std::filesystem::exists(output));
    patches.panel.knob_control_types[3] = axk::SystemKnobControlType::step_3;
    patches.panel.import_view = axk::SystemImportView::sample;
    patches.panel.cdr_scsi_id = 5;
    patches.panel.cdr_write_speed = static_cast<axk::SystemCdrWriteSpeed>(5);
    EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a4000));
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read(source), original);
    patches.panel.cdr_write_speed = axk::SystemCdrWriteSpeed::x4;
    EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a3000));
    EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{7}, patches, axk::ASeriesModel::a4000));
    axk::CancellationSource cancellation;
    cancellation.cancel();
    EXPECT_FALSE(axk::write_system_file(source, output, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a4000,
                                        cancellation.token()));
    const std::array<axk::FilesystemEdit, 1> raw{axk::PutFilesystemFile{
        {"PRF3", "SYSTEM2"}, std::make_shared<axk::MemoryReader>(payload(false)), axk::FileConflict::replace}};
    EXPECT_FALSE(axk::write_sfs_file_edits(source, output, axk::PartitionIndex{0}, raw));
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read(source), original);
}

TEST_F(SystemFileWrite, MapsFragmentedPayloadAndRetainsAllAllocationAndSlackBytes) {
    const auto source = fixture(false);
    auto fragmented = read(source);
    std::uint64_t physical{};
    {
        const auto image = axk::open_image(source).value();
        const auto &partition = image.partitions().front();
        const auto id =
            axk::locate_system_file_record(partition, axk::SystemFileKind::a4000_a5000_system2).value().value();
        const auto &record = *std::ranges::find(partition.records, id, &axk::IndexRecord::sfs_id);
        ASSERT_EQ(record.extents.size(), 1U);
        ASSERT_EQ(record.extents.front().cluster_count, 5U);
        const auto cluster = record.extents.front().cluster_offset;
        physical =
            static_cast<std::uint64_t>(partition.start_sector) * 512U + static_cast<std::uint64_t>(cluster) * 1024U;
        axk::ByteWriter writer{fragmented};
        const auto index = static_cast<std::size_t>(record.record_offset.value);
        ASSERT_TRUE(writer.write_be16(index, 2));
        ASSERT_TRUE(writer.write_be32(index + 0x0a, cluster + 4U));
        ASSERT_TRUE(writer.write_be32(index + 0x0e, 1));
        ASSERT_TRUE(writer.write_be32(index + 0x12, 1024));
        ASSERT_TRUE(writer.write_be32(index + 0x16, cluster));
        ASSERT_TRUE(writer.write_be32(index + 0x1a, 4));
        ASSERT_TRUE(writer.write_be32(index + 0x1e, 0x1030U - 1024U));
        const auto data = payload(false);
        std::ranges::copy(std::span{data}.first(1024),
                          fragmented.begin() + static_cast<std::ptrdiff_t>(physical + 4096U));
        std::ranges::copy(std::span{data}.subspan(1024), fragmented.begin() + static_cast<std::ptrdiff_t>(physical));
    }
    const auto reader = std::make_shared<axk::MemoryReader>(fragmented);
    axk::SystemFilePatch patches;
    patches.recording.effects[1].input_level = 31;
    patches.recording.configuration.ad_input_gain = 1;
    patches.global.master_fine_tune = 7;
    const auto prepared =
        axk::detail::prepare_sfs_system_file(reader, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a4000);
    ASSERT_TRUE(prepared) << prepared.error().message;
    ASSERT_EQ(prepared->patches.size(), 3U);
    const auto offset = physical + 4096U + 0x50U + 0x320U + 40U + 1U;
    const auto gain_offset = physical + (0x50U + 0x398U + 0x33U - 1024U);
    EXPECT_EQ(prepared->patches.front().offset, gain_offset);
    EXPECT_EQ(prepared->patches.front().size, 1U);
    const auto global = physical + 4096U + 0x60U;
    EXPECT_EQ(prepared->patches[1].offset, global);
    EXPECT_EQ(prepared->patches[1].size, 1U);
    EXPECT_EQ(prepared->patches.back().offset, offset);
    EXPECT_EQ(prepared->patches.back().size, 1U);
    auto expected = fragmented;
    expected[static_cast<std::size_t>(offset)] = std::byte{31};
    expected[static_cast<std::size_t>(gain_offset)] = std::byte{1};
    expected[static_cast<std::size_t>(global)] = std::byte{7};
    std::vector<std::byte> actual(expected.size());
    ASSERT_TRUE(prepared->preview->read_exact_at(0, actual));
    EXPECT_EQ(actual, expected);
    const auto empty =
        axk::detail::prepare_sfs_system_file(reader, axk::PartitionIndex{0}, {}, axk::ASeriesModel::a4000);
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->patches.empty());
    EXPECT_FALSE(axk::detail::prepare_sfs_system_file(nullptr, axk::PartitionIndex{0}, {}, axk::ASeriesModel::a4000));
}

TEST_F(SystemFileWrite, CancelsDuringCopyWithoutPublishingOrLeavingStagingFiles) {
    class CancelCopy final : public axk::ProgressSink {
      public:
        axk::CancellationSource cancellation;
        void report(const axk::Progress &) noexcept override { cancellation.cancel(); }
    } progress;
    const auto source = fixture(false);
    const auto original = read(source);
    const auto original_paths = retained_paths();
    const auto output = folder / "cancelled.hds";
    axk::SystemFilePatch patches;
    patches.recording.effects[0].enabled = true;
    const auto result = axk::write_system_file(source, output, axk::PartitionIndex{0}, patches,
                                               axk::ASeriesModel::a4000, progress.cancellation.token(), &progress);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::operation_cancelled);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(read(source), original);
    EXPECT_EQ(retained_paths(), original_paths);
}

TEST_F(SystemFileWrite, RejectsAliasedSystemPayloadEvenWhenFilesystemAllocationAndLinksAreValid) {
    const auto source = fixture(false, true);
    auto aliased = read(source);
    const auto image = axk::open_image(source).value();
    const auto &partition = image.partitions().front();
    const auto id = axk::locate_system_file_record(partition, axk::SystemFileKind::a4000_a5000_system2).value().value();
    const auto &record = *std::ranges::find(partition.records, id, &axk::IndexRecord::sfs_id);
    const auto root_id = axk::locate_partition_root_record(partition).value();
    const auto &root = *std::ranges::find(partition.records, root_id, &axk::IndexRecord::sfs_id);
    const auto &entry = *std::ranges::find(root.directory_entries, "Alias", &axk::DirectoryEntry::name);
    const auto alias =
        *std::ranges::find(partition.records, axk::SfsId{entry.target_link_id->value}, &axk::IndexRecord::sfs_id);
    ASSERT_EQ(alias.data_size, 0U);
    const auto link = static_cast<std::size_t>(partition.start_sector) * 512U +
                      static_cast<std::size_t>(root.extents.front().cluster_offset) * 1024U +
                      entry.payload_relative_offset + 4U;
    axk::ByteWriter writer{aliased};
    ASSERT_TRUE(writer.write_be32(link, id.value));
    ASSERT_TRUE(writer.write_be16(static_cast<std::size_t>(record.record_offset.value) + 0x46U, 2));
    std::ranges::fill(std::span{aliased}.subspan(static_cast<std::size_t>(alias.record_offset.value), 72), std::byte{});
    const auto reader = std::make_shared<axk::MemoryReader>(aliased);
    const auto valid = axk::detail::prepare_sfs_file_edits(reader, axk::PartitionIndex{0}, {});
    ASSERT_TRUE(valid) << valid.error().message;
    axk::SystemFilePatch patches;
    patches.recording.effects[0].input_level = 77;
    EXPECT_FALSE(
        axk::detail::prepare_sfs_system_file(reader, axk::PartitionIndex{0}, patches, axk::ASeriesModel::a4000));
}

TEST_F(SystemFileWrite, RejectsSourceChangesDuringCopyAndDiscardsTheCandidate) {
    class ChangeSource final : public axk::ProgressSink {
      public:
        std::filesystem::path path;
        bool changed{};
        void report(const axk::Progress &) noexcept override {
            if (changed)
                return;
            std::fstream stream{path, std::ios::binary | std::ios::in | std::ios::out};
            stream.seekp(-1, std::ios::end);
            stream.put('x');
            stream.flush();
            changed = static_cast<bool>(stream);
        }
    } progress;
    const auto source = fixture(false);
    progress.path = source;
    auto expected = read(source);
    expected.back() = std::byte{'x'};
    const auto original_paths = retained_paths();
    const auto output = folder / "stale.hds";
    axk::SystemFilePatch patches;
    patches.recording.effects[0].input_level = 77;
    const auto result = axk::write_system_file(source, output, axk::PartitionIndex{0}, patches,
                                               axk::ASeriesModel::a4000, {}, &progress);
    ASSERT_TRUE(progress.changed);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, axk::ErrorCode::transaction_stale);
    EXPECT_EQ(read(source), expected);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(retained_paths(), original_paths);
}

} // namespace
