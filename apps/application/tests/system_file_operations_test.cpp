#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/application/system_file_operations.hpp"
#include "axklib/bytes.hpp"
#include "axklib/filesystem_edit.hpp"
#include "axklib/system_file.hpp"
#include "axklib/writer.hpp"

namespace {
class SystemFileOperations : public testing::Test {
  protected:
    std::filesystem::path root;
    std::unique_ptr<axk::app::Sandbox> sandbox;
    axk::app::PathReservationCoordinator reservations;

    static std::vector<std::byte> record(bool native) {
        std::vector<std::byte> bytes(native ? 0x430U : 0x1030U, std::byte{0xa5});
        axk::ByteWriter writer{bytes};
        EXPECT_TRUE(writer.write_ascii_field(0, 12, "FSFSDEV3SPLX", std::byte{}));
        EXPECT_TRUE(writer.write_ascii_field(12, 4, "PRF3", std::byte{}));
        EXPECT_TRUE(writer.write_be32(0x14, 4));
        EXPECT_TRUE(writer.write_be32(0x18, 0x36));
        EXPECT_TRUE(writer.write_be32(0x1c, native ? 0x408U : 0x1008U));
        EXPECT_TRUE(writer.write_be32(0x30, native ? 0x21520531U : 0xdeadfaceU));
        bytes[0x3e] = std::byte{};
        return bytes;
    }
    void SetUp() override {
        root = std::filesystem::temp_directory_path() /
               std::format("axk-system-session-{}", std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(root / "workspace");
        const auto blank = root / "blank.hds";
        ASSERT_TRUE(axk::write_hds_image({"1.0", 8U * 1024U * 1024U, {{"Test", {}}}}, blank));
        const std::array<axk::FilesystemEdit, 3> edits{
            axk::CreateFilesystemDirectory{{"META"}},
            axk::PutFilesystemFile{{"META", "SYSTEM"}, std::make_shared<axk::MemoryReader>(record(true))},
            axk::PutFilesystemFile{{"META", "SYSTEM2"}, std::make_shared<axk::MemoryReader>(record(false))}};
        ASSERT_TRUE(axk::write_sfs_file_edits(blank, image_path(), axk::PartitionIndex{0}, edits));
        std::uint64_t name_offset{};
        {
            const auto image = axk::open_image(image_path()).value();
            const auto &partition = image.partitions().front();
            const auto id = axk::locate_partition_root_record(partition).value();
            const auto &directory = *std::ranges::find(partition.records, id, &axk::IndexRecord::sfs_id);
            const auto &entry = *std::ranges::find(directory.directory_entries, "META", &axk::DirectoryEntry::name);
            name_offset = static_cast<std::uint64_t>(partition.start_sector) * 512U +
                          static_cast<std::uint64_t>(directory.extents.front().cluster_offset) * 1024U +
                          entry.payload_relative_offset + 8U;
        }
        // Only fixture construction bypasses the normal PRF3 creation protection.
        {
            std::fstream stream{image_path(), std::ios::binary | std::ios::in | std::ios::out};
            stream.seekp(static_cast<std::streamoff>(name_offset));
            stream.write("PRF3", 4);
            ASSERT_TRUE(stream);
        }
        auto created = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", true}});
        ASSERT_TRUE(created);
        sandbox = std::make_unique<axk::app::Sandbox>(std::move(*created));
    }
    void TearDown() override {
        sandbox.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    std::filesystem::path image_path() const { return root / "workspace/image.hds"; }
    std::vector<std::byte> bytes() const {
        const auto reader = axk::FileReader::open(image_path()).value();
        std::vector<std::byte> result(static_cast<std::size_t>(reader->size()));
        EXPECT_TRUE(reader->read_exact_at(0, result));
        return result;
    }
    std::size_t input_offset(axk::ASeriesModel model) const {
        const auto native = model == axk::ASeriesModel::a3000;
        const auto image = axk::open_image(image_path()).value();
        const auto &partition = image.partitions().front();
        const auto kind = native ? axk::SystemFileKind::a3000_system : axk::SystemFileKind::a4000_a5000_system2;
        const auto id = axk::locate_system_file_record(partition, kind).value().value();
        const auto &file = *std::ranges::find(partition.records, id, &axk::IndexRecord::sfs_id);
        return static_cast<std::size_t>(partition.start_sector) * 512U +
               static_cast<std::size_t>(file.extents.front().cluster_offset) * 1024U + 0x50U +
               (native ? 0x110U : 0x320U) + 1U;
    }
    std::size_t global_offset(axk::ASeriesModel model) const {
        return input_offset(model) - (model == axk::ASeriesModel::a3000 ? 0x111U : 0x321U) + 0x10U;
    }
    std::size_t end_type_offset(axk::ASeriesModel model) const {
        return global_offset(model) + (model == axk::ASeriesModel::a3000 ? 0xcfU : 0x2dfU);
    }
    static axk::SystemFilePatch edits() {
        axk::SystemFilePatch patches;
        patches.global.master_fine_tune = 7;
        patches.recording.effects[0].input_level = 77;
        patches.recording.configuration.click_level = 90;
        patches.favorites.push_back({1, {10, 10, {}, {}}});
        patches.panel.end_type = axk::SystemEndType::beat;
        patches.panel.function_selection = axk::SystemFunctionSelection::hold;
        patches.panel.page_selection = axk::SystemPageSelection::last;
        patches.panel.note_display = axk::SystemNoteDisplay::number;
        patches.panel.layer_selection_scope = axk::SystemLayerSelectionScope::all_pages;
        return patches;
    }
};

TEST_F(SystemFileOperations, CommitsOnlyOwnedBytesAndRefreshesTheSessionForEveryModel) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    // This budget fits a few changed bytes, not a journal of the whole image.
    axk::app::AlterationJournalStore journals{root / "journals", 65536U};
    std::uint8_t value{70};
    for (const auto model : {axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        auto expected = bytes();
        const auto offset = input_offset(model);
        expected[offset] = static_cast<std::byte>(value);
        expected[offset - 1U + 120U + 0x13U] = static_cast<std::byte>(value);
        expected[global_offset(model)] = static_cast<std::byte>(value - 63U);
        expected[global_offset(model) + (model == axk::ASeriesModel::a3000 ? 0x42U : 0x1d2U)] = std::byte{0xaa};
        expected[end_type_offset(model)] = std::byte{3};
        expected[end_type_offset(model) - 7U] = std::byte{2};
        expected[end_type_offset(model) - 6U] = std::byte{1};
        expected[end_type_offset(model) - 5U] = std::byte{1};
        expected[end_type_offset(model) + 4U] = std::byte{};
        auto patches = edits();
        patches.panel.format_drive_id = model == axk::ASeriesModel::a3000 ? 7 : 9;
        patches.panel.format_type = axk::SystemFormatType::floppy_quick;
        patches.panel.effect_edit_mode = axk::SystemEffectEditMode::favorite;
        patches.panel.knob_control_types = {axk::SystemKnobControlType::off, axk::SystemKnobControlType::step_1,
                                            axk::SystemKnobControlType::step_2, axk::SystemKnobControlType::step_3};
        patches.panel.assignable_key_function = axk::SystemAssignableKeyFunction::midi_to_sample;
        patches.panel.audition_trigger_mode = axk::SystemAuditionTriggerMode::toggle;
        patches.panel.knob_1_type = axk::SystemKnob1Type::sample;
        const auto panel = end_type_offset(model) - 15U;
        for (std::size_t i = 0; i < 7U; ++i)
            expected[panel + 1U + i] = std::array{std::byte{1}, std::byte{0}, std::byte{2}, std::byte{3},
                                                  std::byte{4}, std::byte{5}, std::byte{1}}[i];
        expected[panel + 17U] = std::byte{1};
        expected[end_type_offset(model) - 15U] = model == axk::ASeriesModel::a3000 ? std::byte{7} : std::byte{9};
        expected[end_type_offset(model) - 4U] = model == axk::ASeriesModel::a3000 ? std::byte{} : std::byte{3};
        if (model == axk::ASeriesModel::a3000) {
            patches.panel.audition_name_view = true;
            patches.panel.midi_to_sample_name_view = false;
            expected[end_type_offset(model) + 3U] = std::byte{};
            expected[end_type_offset(model) + 5U] = std::byte{1};
        } else {
            patches.panel.import_view = axk::SystemImportView::sequence;
            patches.panel.cdr_scsi_id = 5;
            patches.panel.cdr_write_speed = axk::SystemCdrWriteSpeed::x4;
            expected[panel + 16U] = std::byte{3};
            expected[panel + 25U] = std::byte{5};
            expected[panel + 26U] = std::byte{2};
        }
        patches.global.master_fine_tune = static_cast<std::int8_t>(value - 63U);
        patches.recording.configuration.click_level = value;
        patches.recording.effects[0].input_level = value++;
        const auto updated = axk::app::apply_system_file(sessions, journals, opened->image_id, "owner",
                                                         opened->revision, axk::PartitionIndex{0}, patches, model);
        ASSERT_TRUE(updated) << updated.error().message;
        EXPECT_EQ(updated->image_id, opened->image_id);
        EXPECT_EQ(updated->revision, opened->revision + 1U);
        EXPECT_EQ(bytes(), expected);
        {
            const auto read = sessions.begin_read(updated->image_id, "owner", updated->revision);
            ASSERT_TRUE(read) << read.error().message;
            std::array<std::byte, 1> actual{};
            ASSERT_TRUE(read->reader->read_exact_at(offset, actual));
            EXPECT_EQ(actual[0], expected[offset]);
        }
        opened = updated;
    }
}

TEST_F(SystemFileOperations, SortingOnlyEditsRollbackAndRetryWithoutChangingOtherRecords) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    for (const auto model : {axk::ASeriesModel::a3000, axk::ASeriesModel::a4000, axk::ASeriesModel::a5000}) {
        const bool native = model == axk::ASeriesModel::a3000;
        const unsigned value = native ? 1U : model == axk::ASeriesModel::a4000 ? 0U : 2U;
        axk::SystemFilePatch patch;
        if (native) {
            patch.panel.sample_name_order_selection = static_cast<std::uint8_t>(value);
            patch.panel.program_on_placement = axk::SystemProgramOnPlacement::mixed;
            patch.panel.bank_member_visibility = axk::SystemBankMemberVisibility::show;
        } else {
            patch.panel.sample_sort = static_cast<axk::SystemSampleSort>(value);
            patch.panel.tree_sort = static_cast<axk::SystemStatusSort>(value);
            patch.panel.sample_bank_sort = static_cast<axk::SystemStatusSort>(value);
        }
        const auto original = bytes();
        auto expected = original;
        const auto offset = end_type_offset(model) - 15U + (native ? 12U : 22U);
        for (std::size_t i = 0; i < 3U; ++i)
            expected[offset + i] = static_cast<std::byte>(value);
        axk::CancellationSource cancellation;
        bool all_written{};
        axk::app::AlterationJournalStore journals{root / "journals", 65536U,
                                                  [&](std::string_view phase, std::size_t index) {
                                                      if (phase == "after-patch" && index == 0U) {
                                                          all_written = bytes() == expected;
                                                          cancellation.cancel();
                                                      }
                                                      return false;
                                                  }};
        auto invalid = patch;
        if (native)
            invalid.panel.tree_sort = axk::SystemStatusSort::off;
        else
            invalid.panel.sample_name_order_selection = 0;
        EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                                 axk::PartitionIndex{0}, invalid, model));
        EXPECT_FALSE(all_written);
        EXPECT_EQ(bytes(), original);
        const auto cancelled =
            axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                        axk::PartitionIndex{0}, patch, model, cancellation.token());
        ASSERT_FALSE(cancelled);
        EXPECT_EQ(cancelled.error().code, "operation_cancelled");
        EXPECT_TRUE(all_written);
        EXPECT_EQ(bytes(), original);
        EXPECT_TRUE(sessions.begin_read(opened->image_id, "owner", opened->revision));
        EXPECT_TRUE(journals.storage_ready());
        const auto updated = axk::app::apply_system_file(sessions, journals, opened->image_id, "owner",
                                                         opened->revision, axk::PartitionIndex{0}, patch, model);
        ASSERT_TRUE(updated) << updated.error().message;
        EXPECT_EQ(updated->revision, opened->revision + 1U);
        EXPECT_EQ(bytes(), expected);
        opened = updated;
    }
}

TEST_F(SystemFileOperations, RejectsInvalidRequestsWithoutChangingBytesOrRetainingMutationAccess) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    bool journal_started{};
    axk::app::AlterationJournalStore journals{root / "journals", 65536U, [&](std::string_view, std::size_t) {
                                                  journal_started = true;
                                                  return false;
                                              }};
    const auto original = bytes();
    auto invalid = edits();
    invalid.recording.effects[2].type = 255;
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{0}, invalid, axk::ASeriesModel::a4000));
    invalid = edits();
    invalid.recording.configuration.input = 3;
    invalid.recording.configuration.frequency_selection = 4;
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{0}, invalid, axk::ASeriesModel::a4000));
    std::array<axk::SystemPanelPatch, 3> invalid_commands{};
    invalid_commands[0].import_view = static_cast<axk::SystemImportView>(4);
    invalid_commands[1].cdr_scsi_id = 8;
    invalid_commands[2].cdr_write_speed = static_cast<axk::SystemCdrWriteSpeed>(5);
    for (const auto &panel : invalid_commands) {
        invalid = edits();
        invalid.panel = panel;
        EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                                 axk::PartitionIndex{0}, invalid, axk::ASeriesModel::a4000));
        EXPECT_FALSE(journal_started);
        EXPECT_EQ(bytes(), original);
    }
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "other", opened->revision,
                                             axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000));
    const auto stale = axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision + 1U,
                                                   axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000);
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, "image_revision_stale");
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{7}, edits(), axk::ASeriesModel::a4000));
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{0}, edits(), static_cast<axk::ASeriesModel>(255)));
    axk::CancellationSource cancelled;
    cancelled.cancel();
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000,
                                             cancelled.token()));
    EXPECT_EQ(bytes(), original);
    EXPECT_FALSE(journal_started);
    const auto read = sessions.begin_read(opened->image_id, "owner", opened->revision);
    EXPECT_TRUE(read);
}

TEST_F(SystemFileOperations, EachParameterGroupCanBeEditedWithoutTouchingTheOthers) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    axk::app::AlterationJournalStore journals{root / "journals", 65536U};
    const auto offset = input_offset(axk::ASeriesModel::a4000);
    for (const unsigned group : {0U, 1U, 2U, 3U, 4U}) {
        auto expected = bytes();
        axk::SystemFilePatch patch;
        if (group == 0U) {
            patch.global.master_fine_tune = 7;
            expected[global_offset(axk::ASeriesModel::a4000)] = std::byte{7};
        } else if (group == 1U) {
            patch.recording.configuration.pre_trigger_time = 5;
            expected[offset - 1U + 120U + 4U] = std::byte{5};
        } else if (group == 2U) {
            patch.recording.effects[0].input_level = 77;
            expected[offset] = std::byte{77};
        } else if (group == 3U) {
            patch.favorites.push_back({1, {10, 10, {}, {}}});
            expected[global_offset(axk::ASeriesModel::a4000) + 0x1d2U] = std::byte{0xaa};
        } else {
            patch.panel = edits().panel;
            expected[end_type_offset(axk::ASeriesModel::a4000)] = std::byte{3};
            expected[end_type_offset(axk::ASeriesModel::a4000) - 7U] = std::byte{2};
            expected[end_type_offset(axk::ASeriesModel::a4000) - 6U] = std::byte{1};
            expected[end_type_offset(axk::ASeriesModel::a4000) - 5U] = std::byte{1};
            expected[end_type_offset(axk::ASeriesModel::a4000) + 4U] = std::byte{};
        }
        const auto updated =
            axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                        axk::PartitionIndex{0}, patch, axk::ASeriesModel::a4000);
        ASSERT_TRUE(updated) << updated.error().message;
        EXPECT_EQ(updated->revision, opened->revision + 1U);
        EXPECT_EQ(bytes(), expected);
        opened = updated;
    }
}

TEST_F(SystemFileOperations, RollsBackCancelledWritesAndAllowsRetryAtTheSameRevision) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto original = bytes();
    axk::CancellationSource cancellation;
    bool wrote{};
    axk::app::AlterationJournalStore journals{root / "journals", 65536U, [&](std::string_view phase, std::size_t) {
                                                  if (phase == "after-patch-chunk") {
                                                      wrote = true;
                                                      cancellation.cancel();
                                                  }
                                                  return false;
                                              }};
    const auto result =
        axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                    axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000, cancellation.token());
    ASSERT_TRUE(wrote);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "operation_cancelled");
    EXPECT_TRUE(journals.storage_ready());
    EXPECT_EQ(bytes(), original);
    {
        const auto read = sessions.begin_read(opened->image_id, "owner", opened->revision);
        ASSERT_TRUE(read) << read.error().message;
    }
    const auto retry = axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                                   axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000);
    ASSERT_TRUE(retry) << retry.error().message;
    EXPECT_EQ(retry->revision, opened->revision + 1U);
}

TEST_F(SystemFileOperations, RetainsInterruptedJournalForRecoveryBeforeReopening) {
    const auto original = bytes();
    {
        axk::app::ImageSessionManager sessions{
            *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
        const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
        ASSERT_TRUE(opened);
        axk::app::AlterationJournalStore journals{
            root / "journals", 65536U,
            [](std::string_view phase, std::size_t index) { return phase == "after-patch" && index == 6U; }};
        const auto result = axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                                        axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000);
        ASSERT_FALSE(result);
        EXPECT_FALSE(journals.storage_ready());
        auto changed = original;
        const auto offset = input_offset(axk::ASeriesModel::a4000);
        changed[offset] = std::byte{77};
        changed[offset - 1U + 120U + 0x13U] = std::byte{90};
        changed[global_offset(axk::ASeriesModel::a4000)] = std::byte{7};
        changed[global_offset(axk::ASeriesModel::a4000) + 0x1d2U] = std::byte{0xaa};
        changed[end_type_offset(axk::ASeriesModel::a4000)] = std::byte{3};
        changed[end_type_offset(axk::ASeriesModel::a4000) - 7U] = std::byte{2};
        changed[end_type_offset(axk::ASeriesModel::a4000) - 6U] = std::byte{1};
        changed[end_type_offset(axk::ASeriesModel::a4000) - 5U] = std::byte{1};
        changed[end_type_offset(axk::ASeriesModel::a4000) + 4U] = std::byte{};
        EXPECT_EQ(bytes(), changed);
        const auto stale = sessions.begin_read(opened->image_id, "owner", opened->revision);
        ASSERT_FALSE(stale);
        EXPECT_EQ(stale.error().code, "image_session_invalidated");
        EXPECT_FALSE(sessions.inspect(opened->image_id, "owner"));
        EXPECT_FALSE(sessions.begin_mutation(opened->image_id, "owner", opened->revision));
    }
    axk::app::AlterationJournalStore recovery{root / "journals", 65536U};
    ASSERT_TRUE(recovery.recover(*sandbox));
    EXPECT_EQ(bytes(), original);
    axk::app::ImageSessionManager reopened{*sandbox};
    EXPECT_TRUE(reopened.open({"workspace", "image.hds"}, "owner"));
}

TEST_F(SystemFileOperations, CancellationAfterAllGroupsWereWrittenRestoresAllAtTheOldRevision) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto original = bytes();
    auto expected = original;
    const auto offset = input_offset(axk::ASeriesModel::a4000);
    expected[offset] = std::byte{77};
    expected[offset - 1U + 120U + 0x13U] = std::byte{90};
    expected[global_offset(axk::ASeriesModel::a4000)] = std::byte{7};
    expected[global_offset(axk::ASeriesModel::a4000) + 0x1d2U] = std::byte{0xaa};
    expected[end_type_offset(axk::ASeriesModel::a4000)] = std::byte{3};
    expected[end_type_offset(axk::ASeriesModel::a4000) - 7U] = std::byte{2};
    expected[end_type_offset(axk::ASeriesModel::a4000) - 6U] = std::byte{1};
    expected[end_type_offset(axk::ASeriesModel::a4000) - 5U] = std::byte{1};
    expected[end_type_offset(axk::ASeriesModel::a4000) + 4U] = std::byte{};
    axk::CancellationSource cancellation;
    bool all_written{};
    axk::app::AlterationJournalStore journals{root / "journals", 65536U,
                                              [&](std::string_view phase, std::size_t index) {
                                                  if (phase == "after-patch" && index == 6U) {
                                                      all_written = bytes() == expected;
                                                      cancellation.cancel();
                                                  }
                                                  return false;
                                              }};
    const auto result =
        axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                    axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000, cancellation.token());
    ASSERT_TRUE(all_written);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "operation_cancelled");
    EXPECT_EQ(bytes(), original);
    EXPECT_TRUE(journals.storage_ready());
    EXPECT_TRUE(sessions.begin_read(opened->image_id, "owner", opened->revision));
}

TEST_F(SystemFileOperations, RejectsReadOnlySourcesAndCompetingSessionLeases) {
    const auto original = bytes();
    axk::app::AlterationJournalStore journals{root / "journals", 65536U};
    {
        auto readonly = axk::app::Sandbox::create({{"workspace", "Workspace", root / "workspace", false}});
        ASSERT_TRUE(readonly);
        axk::app::ImageSessionManager sessions{
            *readonly, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
        const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
        ASSERT_TRUE(opened);
        EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                                 axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000));
    }
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    const auto other = sessions.open({"workspace", "image.hds"}, "other");
    ASSERT_TRUE(opened);
    ASSERT_TRUE(other);
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000));
    EXPECT_EQ(bytes(), original);
    ASSERT_TRUE(sessions.close(other->image_id, "other"));
    EXPECT_TRUE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                            axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000));
}

TEST_F(SystemFileOperations, MissingSystemFileIsNotCreatedAndMutationAccessIsReleased) {
    std::filesystem::copy_file(root / "blank.hds", root / "workspace/missing.hds");
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "missing.hds"}, "owner");
    ASSERT_TRUE(opened);
    axk::app::AlterationJournalStore journals{root / "journals", 65536U};
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000));
    const auto read = sessions.begin_read(opened->image_id, "owner", opened->revision);
    ASSERT_TRUE(read);
    const auto before = axk::FileReader::open(root / "blank.hds").value();
    std::vector<std::byte> expected(static_cast<std::size_t>(before->size()));
    std::vector<std::byte> actual(expected.size());
    ASSERT_TRUE(before->read_exact_at(0, expected));
    ASSERT_TRUE(read->reader->read_exact_at(0, actual));
    EXPECT_EQ(actual, expected);
}

TEST_F(SystemFileOperations, CancellationDuringMetadataRefreshRollsBackBeforePublishingRevision) {
    class CancelRefresh final : public axk::ProgressSink {
      public:
        axk::CancellationSource cancellation;
        bool refreshing{};
        void report(const axk::Progress &progress) noexcept override {
            if (progress.phase == axk::ProgressPhase::validating) {
                refreshing = true;
                cancellation.cancel();
            }
        }
    } progress;
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    const auto original = bytes();
    axk::app::AlterationJournalStore journals{root / "journals", 65536U};
    const auto result = axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                                    axk::PartitionIndex{0}, edits(), axk::ASeriesModel::a4000,
                                                    progress.cancellation.token(), &progress);
    ASSERT_TRUE(progress.refreshing);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, "operation_cancelled");
    EXPECT_EQ(bytes(), original);
    EXPECT_TRUE(journals.storage_ready());
    const auto read = sessions.begin_read(opened->image_id, "owner", opened->revision);
    EXPECT_TRUE(read);
}
TEST_F(SystemFileOperations, CombinedGlobalAndRecordingEditRefreshesTheRoutingContextInOneRevision) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    axk::app::AlterationJournalStore journals{root / "journals", 65536U};
    axk::SystemFilePatch patch;
    patch.global.program_mode = axk::ProgramMode::multi;
    patch.global.basic_receive_channel_selection = 31;
    patch.global.program_change_enabled = false;
    for (std::size_t i = 0; i < patch.global.part_program_numbers.size(); ++i)
        patch.global.part_program_numbers[i] = static_cast<std::uint8_t>(128U - i);
    patch.recording.configuration.click_level = 91;
    patch.recording.effects[0].input_level = 78;
    const auto offset = input_offset(axk::ASeriesModel::a5000);
    const auto global = offset - 0x321U + 0x10U;
    auto expected = bytes();
    expected[global + 4U] = std::byte{31};
    expected[global + 6U] &= std::byte{0xfc};
    expected[global + 0x2eU] = std::byte{1};
    for (std::size_t i = 0; i < 32U; ++i)
        expected[global + 0x30U + i] = static_cast<std::byte>(128U - i);
    expected[offset] = std::byte{78};
    expected[offset - 1U + 120U + 0x13U] = std::byte{91};
    const auto result = axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                                    axk::PartitionIndex{0}, patch, axk::ASeriesModel::a5000);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->revision, opened->revision + 1U);
    EXPECT_EQ(bytes(), expected);
    const auto contexts = sessions.system_program_contexts(result->image_id, "owner", 0);
    ASSERT_TRUE(contexts);
    const auto context = std::ranges::find(contexts->files, axk::app::SystemProgramContextFile::system2,
                                           &axk::app::ImageSystemProgramContext::file_kind);
    ASSERT_NE(context, contexts->files.end());
    EXPECT_EQ(context->availability, axk::app::SystemProgramContextAvailability::available);
    EXPECT_EQ(context->saved_program_mode, "MULTI");
    EXPECT_EQ(context->omni, false);
    EXPECT_EQ(context->program_change_enabled, false);
    ASSERT_TRUE(context->basic_receive);
    EXPECT_EQ(context->basic_receive->display, "B16");
    ASSERT_EQ(context->parts.size(), 32U);
    EXPECT_TRUE(context->parts[31].master);
    EXPECT_EQ(context->parts[31].program_number, 97);
}

TEST_F(SystemFileOperations, InvalidGlobalWithValidRecordingNeverStartsTheJournal) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    bool journal_started{};
    axk::app::AlterationJournalStore journals{root / "journals", 65536U, [&](std::string_view, std::size_t) {
                                                  journal_started = true;
                                                  return false;
                                              }};
    const auto original = bytes();
    auto patch = edits();
    patch.global.master_fine_tune = 64;
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{0}, patch, axk::ASeriesModel::a4000));
    EXPECT_FALSE(journal_started);
    EXPECT_EQ(bytes(), original);
    EXPECT_TRUE(sessions.begin_read(opened->image_id, "owner", opened->revision));
}

TEST_F(SystemFileOperations, InvalidFavoritesWithValidOtherGroupsNeverStartsTheJournal) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    bool journal_started{};
    axk::app::AlterationJournalStore journals{root / "journals", 65536U, [&](std::string_view, std::size_t) {
                                                  journal_started = true;
                                                  return false;
                                              }};
    const auto original = bytes();
    auto patch = edits();
    patch.favorites.push_back({15, {2, {}, {}, {}}});
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{0}, patch, axk::ASeriesModel::a4000));
    EXPECT_FALSE(journal_started);
    EXPECT_EQ(bytes(), original);
    EXPECT_TRUE(sessions.begin_read(opened->image_id, "owner", opened->revision));
}

TEST_F(SystemFileOperations, InvalidPanelWithValidOtherGroupsNeverStartsTheJournal) {
    axk::app::ImageSessionManager sessions{
        *sandbox, 32U, 500U, std::chrono::minutes{15}, std::chrono::steady_clock::now, &reservations};
    const auto opened = sessions.open({"workspace", "image.hds"}, "owner");
    ASSERT_TRUE(opened);
    bool journal_started{};
    axk::app::AlterationJournalStore journals{root / "journals", 65536U, [&](std::string_view, std::size_t) {
                                                  journal_started = true;
                                                  return false;
                                              }};
    const auto original = bytes();
    auto patch = edits();
    patch.panel.end_type = axk::SystemEndType::graph;
    EXPECT_FALSE(axk::app::apply_system_file(sessions, journals, opened->image_id, "owner", opened->revision,
                                             axk::PartitionIndex{0}, patch, axk::ASeriesModel::a4000));
    EXPECT_FALSE(journal_started);
    EXPECT_EQ(bytes(), original);
    EXPECT_TRUE(sessions.begin_read(opened->image_id, "owner", opened->revision));
}
} // namespace
