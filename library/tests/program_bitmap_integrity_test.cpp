#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/deletion.hpp"
#include "axklib/object.hpp"
#include "axklib/relationship.hpp"
#include "axklib/sample_storage.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"
#include "axklib/writer.hpp"

namespace {

class ProgramBitmapIntegrity : public testing::TestWithParam<axk::SampleStorageFormat> {
  protected:
    std::filesystem::path root;
    std::optional<axk::Container> container;
    axk::ObjectCatalog catalog;
    axk::RelationshipGraph graph;

    void SetUp() override {
        std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
        std::ranges::replace(name, '/', '-');
        for (std::size_t attempt = 0; attempt < 1024U; ++attempt) {
            const auto candidate = std::filesystem::temp_directory_path() /
                                   ("axklib-program-bitmap-" + name + '-' + std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                root = candidate;
                break;
            }
            ASSERT_FALSE(error) << error.message();
        }
        ASSERT_FALSE(root.empty());

        axk::Waveform waveform;
        waveform.format = {1U, 2U, 44'100U};
        waveform.frame_count = 4U;
        waveform.pcm = {std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0},
                        std::byte{2}, std::byte{0}, std::byte{0}, std::byte{0}};
        const auto audio = root / "tone.wav";
        ASSERT_TRUE(axk::write_wav_atomic(audio, waveform));

        axk::VolumeSpec volume;
        volume.name = "importtest";
        volume.waveforms.push_back({"wave", "Wave", audio, 60U, {}});
        for (const auto *sample_name : {"909 Closed HH", "909 Open HH", "DX Member"}) {
            axk::SampleSpec sample;
            sample.name = sample_name;
            sample.waveform_id = "wave";
            sample.storage_format = GetParam();
            volume.samples.push_back(std::move(sample));
        }
        volume.sample_banks.push_back({"DX PER", {"DX Member"}, {}, GetParam()});
        volume.programs.push_back({8U, "909 HH", {{"SBNK", "909 Closed HH", {}}, {"SBNK", "909 Open HH", {}}}});
        volume.programs.push_back({21U, "DX METAL", {{"SBAC", "DX PER", {}}}});
        const auto source = root / "source.hds";
        const axk::HdsBuildManifest manifest{"1.0", 4U * 1024U * 1024U, {{"Partition", {volume}}}};
        const auto written = axk::write_hds_image(manifest, source);
        ASSERT_TRUE(written) << written.error().message;
        auto opened = axk::open_image(source);
        ASSERT_TRUE(opened) << opened.error().message;
        container.emplace(std::move(*opened));
        auto loaded = axk::build_object_catalog(*container);
        ASSERT_TRUE(loaded) << loaded.error().message;
        catalog = std::move(*loaded);
        graph = axk::build_relationship_graph(catalog);
    }

    void TearDown() override {
        graph = {};
        catalog = {};
        container.reset();
        std::error_code error;
        if (!root.empty())
            std::filesystem::remove_all(root, error);
        EXPECT_FALSE(error) << error.message();
    }

    axk::ObjectSnapshot *find(axk::ObjectType type, std::string_view name) {
        const auto found = std::ranges::find_if(catalog.objects, [&](const auto &item) {
            return item.object.header.type == type && item.object.header.name == name;
        });
        return found == catalog.objects.end() ? nullptr : &*found;
    }

    void set_bank_links(std::span<const std::uint8_t> numbers) {
        auto *bank = find(axk::ObjectType::sbac, "DX PER");
        ASSERT_NE(bank, nullptr);
        ASSERT_GE(bank->raw_payload.size(), 0xa0U);
        std::fill(bank->raw_payload.begin() + 0x90U, bank->raw_payload.begin() + 0xa0U, std::byte{0});
        for (const auto number : numbers) {
            ASSERT_GE(number, 1U);
            ASSERT_LE(number, 128U);
            const auto index = static_cast<std::size_t>(number - 1U);
            const auto offset = 0x90U + (index / 32U) * 4U + 3U - ((index % 32U) / 8U);
            bank->raw_payload[offset] |= static_cast<std::byte>(1U << (index % 8U));
        }
        auto decoded = axk::decode_object(bank->raw_payload);
        ASSERT_TRUE(decoded) << decoded.error().message;
        bank->object = std::move(*decoded);
        graph = axk::build_relationship_graph(catalog);
    }
};

TEST_P(ProgramBitmapIntegrity, ReportsImportedBankWithSourceProgramNumberAsWarning) {
    set_bank_links(std::vector<std::uint8_t>{8U});
    const auto *bank = find(axk::ObjectType::sbac, "DX PER");
    ASSERT_NE(bank, nullptr);

    const auto report = axk::validate_semantics(*container, catalog, graph);

    const auto issue =
        std::ranges::find(report.issues, "REL_SBAC_PROGRAM_BITMAP_MISMATCH", &axk::ValidationIssue::code);
    ASSERT_NE(issue, report.issues.end());
    EXPECT_EQ(issue->object_key, bank->key);
    EXPECT_EQ(issue->severity, axk::ValidationSeverity::warning);
    EXPECT_NE(issue->sampler_path.find("importtest"), std::string::npos);
    EXPECT_NE(issue->message.find("DX PER"), std::string::npos);
    EXPECT_NE(issue->message.find("008"), std::string::npos);
    EXPECT_NE(issue->message.find("021"), std::string::npos);
    EXPECT_TRUE(report.valid());
}

TEST_P(ProgramBitmapIntegrity, BlocksUnrelatedProgramDeleteAllWhenBankClaimsItsNumber) {
    set_bank_links(std::vector<std::uint8_t>{8U});
    const auto *program = find(axk::ObjectType::prog, "008");
    const auto *closed = find(axk::ObjectType::sbnk, "909 Closed HH");
    const auto *open = find(axk::ObjectType::sbnk, "909 Open HH");
    ASSERT_NE(program, nullptr);
    ASSERT_NE(closed, nullptr);
    ASSERT_NE(open, nullptr);

    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph,
        {.target_keys = {program->key}, .referrer_keys = {}, .cleanup_keys = {closed->key, open->key}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->can_apply);
    EXPECT_TRUE(inspected->selected_keys.empty());
    EXPECT_TRUE(inspected->manifest.operations.empty());
    for (const auto *sample : {closed, open}) {
        const auto impact = std::ranges::find(inspected->impacts, sample->key, &axk::ObjectDeletionImpact::object_key);
        ASSERT_NE(impact, inspected->impacts.end());
        EXPECT_TRUE(impact->requested);
        EXPECT_FALSE(impact->selected);
        EXPECT_EQ(impact->status, axk::ObjectDeletionStatus::preserved);
    }
    EXPECT_TRUE(std::ranges::any_of(inspected->blockers,
                                    [](const auto &notice) { return notice.code == "PROGRAM_LINKS_INCONSISTENT"; }));
}

TEST_P(ProgramBitmapIntegrity, RejectsCleanupOutsideBlockedProgramsDependencyClosure) {
    set_bank_links(std::vector<std::uint8_t>{8U});
    const auto *program = find(axk::ObjectType::prog, "008");
    const auto *bank = find(axk::ObjectType::sbac, "DX PER");
    ASSERT_NE(program, nullptr);
    ASSERT_NE(bank, nullptr);

    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {program->key}, .referrer_keys = {}, .cleanup_keys = {bank->key}});

    ASSERT_FALSE(inspected);
    EXPECT_EQ(inspected.error().code, axk::ErrorCode::transaction_rejected);
}

TEST_P(ProgramBitmapIntegrity, DoesNotSelectBlockedCleanupAlongsideAnEligibleProgram) {
    set_bank_links(std::vector<std::uint8_t>{});
    const auto *blocked = find(axk::ObjectType::prog, "021");
    const auto *eligible = find(axk::ObjectType::prog, "008");
    const auto *member = find(axk::ObjectType::sbnk, "DX Member");
    ASSERT_NE(blocked, nullptr);
    ASSERT_NE(eligible, nullptr);
    ASSERT_NE(member, nullptr);

    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph,
        {.target_keys = {blocked->key, eligible->key}, .referrer_keys = {}, .cleanup_keys = {member->key}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    EXPECT_EQ(inspected->selected_keys, std::vector<std::string>{eligible->key});
    ASSERT_EQ(inspected->manifest.operations.size(), 1U);
    const auto impact = std::ranges::find(inspected->impacts, member->key, &axk::ObjectDeletionImpact::object_key);
    ASSERT_NE(impact, inspected->impacts.end());
    EXPECT_TRUE(impact->requested);
    EXPECT_FALSE(impact->selected);
    EXPECT_EQ(impact->status, axk::ObjectDeletionStatus::preserved);
}

TEST_P(ProgramBitmapIntegrity, RejectsNonoptionalCleanupWhenNoTargetIsBlocked) {
    const auto *program = find(axk::ObjectType::prog, "008");
    const auto *wave = find(axk::ObjectType::smpl, "Wave");
    ASSERT_NE(program, nullptr);
    ASSERT_NE(wave, nullptr);

    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {program->key}, .referrer_keys = {}, .cleanup_keys = {wave->key}});

    ASSERT_FALSE(inspected);
    EXPECT_EQ(inspected.error().code, axk::ErrorCode::transaction_rejected);
}

TEST_P(ProgramBitmapIntegrity, BlocksAssignedProgramWhenBankOmitsItsNumber) {
    set_bank_links(std::vector<std::uint8_t>{});
    const auto *program = find(axk::ObjectType::prog, "021");
    ASSERT_NE(program, nullptr);

    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {program->key}, .referrer_keys = {}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->can_apply);
    EXPECT_TRUE(inspected->manifest.operations.empty());
    EXPECT_TRUE(std::ranges::any_of(inspected->blockers,
                                    [](const auto &notice) { return notice.code == "PROGRAM_LINKS_INCONSISTENT"; }));
}

TEST_P(ProgramBitmapIntegrity, BlocksBankDeletionWithItsCorrectProgramWhenAnExtraBitRemains) {
    set_bank_links(std::vector<std::uint8_t>{8U, 21U});
    const auto *bank = find(axk::ObjectType::sbac, "DX PER");
    const auto *program = find(axk::ObjectType::prog, "021");
    ASSERT_NE(bank, nullptr);
    ASSERT_NE(program, nullptr);
    const auto comparison = std::ranges::find(graph.bitmap_comparisons, bank->key, &axk::BitmapComparison::object_key);
    ASSERT_NE(comparison, graph.bitmap_comparisons.end());

    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {bank->key}, .referrer_keys = {program->key}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->can_apply);
    EXPECT_TRUE(inspected->selected_keys.empty());
    EXPECT_TRUE(inspected->manifest.operations.empty());
    const auto blocker =
        std::ranges::find(inspected->blockers, "PROGRAM_LINKS_INCONSISTENT", &axk::ObjectDeletionNotice::code);
    ASSERT_NE(blocker, inspected->blockers.end());
    EXPECT_EQ(blocker->object_keys, std::vector<std::string>{bank->key});
    EXPECT_EQ(blocker->message, axk::program_bitmap_mismatch_message(*comparison, "DX PER"));
}

TEST_P(ProgramBitmapIntegrity, BlocksBankDeletionWhenItsOnlyProgramBitHasNoProgram) {
    std::erase_if(catalog.objects, [](const auto &object) {
        return object.object.header.type == axk::ObjectType::prog && object.object.header.name == "021";
    });
    set_bank_links(std::vector<std::uint8_t>{128U});
    const auto *bank = find(axk::ObjectType::sbac, "DX PER");
    const auto *member = find(axk::ObjectType::sbnk, "DX Member");
    ASSERT_NE(bank, nullptr);
    ASSERT_NE(member, nullptr);
    const auto comparison = std::ranges::find(graph.bitmap_comparisons, bank->key, &axk::BitmapComparison::object_key);
    ASSERT_NE(comparison, graph.bitmap_comparisons.end());
    ASSERT_TRUE(comparison->direct_assignment_programs.empty());

    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {bank->key}, .referrer_keys = {}, .cleanup_keys = {member->key}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->can_apply);
    EXPECT_TRUE(inspected->selected_keys.empty());
    EXPECT_TRUE(inspected->manifest.operations.empty());
    const auto blocker =
        std::ranges::find(inspected->blockers, "PROGRAM_LINKS_INCONSISTENT", &axk::ObjectDeletionNotice::code);
    ASSERT_NE(blocker, inspected->blockers.end());
    EXPECT_EQ(blocker->object_keys, std::vector<std::string>{bank->key});
    EXPECT_EQ(blocker->message, axk::program_bitmap_mismatch_message(*comparison, "DX PER"));

    const auto *unrelated = find(axk::ObjectType::prog, "008");
    ASSERT_NE(unrelated, nullptr);
    const auto unrelated_inspection = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {unrelated->key}, .referrer_keys = {}, .cleanup_keys = {}});
    ASSERT_TRUE(unrelated_inspection) << unrelated_inspection.error().message;
    EXPECT_TRUE(unrelated_inspection->can_apply);
}

TEST_P(ProgramBitmapIntegrity, AllowsConsistentBankDeletionWithItsAssignedProgram) {
    const auto *bank = find(axk::ObjectType::sbac, "DX PER");
    const auto *program = find(axk::ObjectType::prog, "021");
    ASSERT_NE(bank, nullptr);
    ASSERT_NE(program, nullptr);

    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {bank->key}, .referrer_keys = {program->key}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    EXPECT_TRUE(inspected->blockers.empty());
    EXPECT_TRUE(std::ranges::contains(inspected->selected_keys, bank->key));
    EXPECT_TRUE(std::ranges::contains(inspected->selected_keys, program->key));
    EXPECT_EQ(inspected->manifest.operations.size(), 2U);
}

TEST_P(ProgramBitmapIntegrity, KeepsUnrelatedDeletionAvailableForConsistentBanks) {
    const auto *program = find(axk::ObjectType::prog, "008");
    ASSERT_NE(program, nullptr);

    const auto report = axk::validate_semantics(*container, catalog, graph);
    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {program->key}, .referrer_keys = {}, .cleanup_keys = {}});

    EXPECT_TRUE(std::ranges::none_of(
        report.issues, [](const auto &issue) { return issue.code == "REL_SBAC_PROGRAM_BITMAP_MISMATCH"; }));
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    ASSERT_EQ(inspected->manifest.operations.size(), 1U);
}

TEST_P(ProgramBitmapIntegrity, DecodesBankBitmapWordBoundariesAndReportsAbsentPrograms) {
    const std::vector<std::uint8_t> numbers{1U, 32U, 33U, 64U, 65U, 96U, 97U, 128U};
    set_bank_links(numbers);
    const auto *bank = find(axk::ObjectType::sbac, "DX PER");
    ASSERT_NE(bank, nullptr);
    const auto *decoded = std::get_if<axk::CurrentSbac>(&bank->object.payload);
    ASSERT_NE(decoded, nullptr);

    EXPECT_EQ(decoded->linked_program_numbers, numbers);
    EXPECT_EQ(decoded->linked_program_bitmap_words,
              (std::array<std::uint32_t, 4>{0x80000001U, 0x80000001U, 0x80000001U, 0x80000001U}));
    const auto comparison = std::ranges::find(graph.bitmap_comparisons, bank->key, &axk::BitmapComparison::object_key);
    ASSERT_NE(comparison, graph.bitmap_comparisons.end());
    EXPECT_EQ(comparison->object_type, axk::ObjectType::sbac);
    EXPECT_EQ(comparison->bitmap_programs, numbers);
    EXPECT_EQ(comparison->direct_assignment_programs, (std::vector<std::uint8_t>{21U}));
    EXPECT_EQ(comparison->bitmap_without_direct, numbers);
    EXPECT_EQ(comparison->direct_without_bitmap, (std::vector<std::uint8_t>{21U}));
    EXPECT_TRUE(comparison->indirect_assignment_programs.empty());
    EXPECT_EQ(comparison->status, "mismatch");
}

TEST_P(ProgramBitmapIntegrity, MatchesBankReferencedByProgramsAcrossAllBitmapWords) {
    const auto *program = find(axk::ObjectType::prog, "021");
    ASSERT_NE(program, nullptr);
    const auto template_program = *program;
    std::erase_if(catalog.objects, [](const auto &item) {
        return item.object.header.type == axk::ObjectType::prog && item.object.header.name == "021";
    });
    const std::vector<std::uint8_t> numbers{1U, 32U, 33U, 64U, 65U, 96U, 97U, 128U};
    for (const auto number : numbers) {
        auto referenced = template_program;
        referenced.key += ':' + std::to_string(number);
        referenced.object.header.name = std::format("{:03}", number);
        ASSERT_TRUE(referenced.placement);
        referenced.placement->entry_name = referenced.object.header.name;
        catalog.objects.push_back(std::move(referenced));
    }
    set_bank_links(numbers);
    const auto *bank = find(axk::ObjectType::sbac, "DX PER");
    ASSERT_NE(bank, nullptr);

    const auto comparison = std::ranges::find(graph.bitmap_comparisons, bank->key, &axk::BitmapComparison::object_key);

    ASSERT_NE(comparison, graph.bitmap_comparisons.end());
    EXPECT_EQ(comparison->status, "match");
    EXPECT_EQ(comparison->direct_assignment_programs, numbers);
    EXPECT_TRUE(comparison->bitmap_without_direct.empty());
    EXPECT_TRUE(comparison->direct_without_bitmap.empty());
}

TEST_P(ProgramBitmapIntegrity, KeepsBankProgramsIndirectForMemberSampleBitmapChecks) {
    const auto *member = find(axk::ObjectType::sbnk, "DX Member");
    ASSERT_NE(member, nullptr);

    const auto comparison =
        std::ranges::find(graph.bitmap_comparisons, member->key, &axk::BitmapComparison::object_key);

    ASSERT_NE(comparison, graph.bitmap_comparisons.end());
    EXPECT_EQ(comparison->object_type, axk::ObjectType::sbnk);
    EXPECT_EQ(comparison->status, "match");
    EXPECT_TRUE(comparison->bitmap_programs.empty());
    EXPECT_TRUE(comparison->direct_assignment_programs.empty());
    EXPECT_EQ(comparison->indirect_assignment_programs, (std::vector<std::uint8_t>{21U}));
}

TEST_P(ProgramBitmapIntegrity, DoesNotUseMatchingBankNamesOrProgramNumbersFromAnotherVolume) {
    auto foreign_objects = catalog.objects;
    for (auto &item : foreign_objects) {
        item.key = "foreign:" + item.key;
        item.scope_key = "foreign-volume";
        ASSERT_TRUE(item.placement);
        item.placement->volume_name = "Other volume";
        if (item.object.header.type == axk::ObjectType::prog && item.object.header.name == "021") {
            item.object.header.name = "022";
            item.placement->entry_name = "022";
        }
        catalog.objects.push_back(std::move(item));
    }
    graph = axk::build_relationship_graph(catalog);
    const auto *bank = find(axk::ObjectType::sbac, "DX PER");
    const auto *program = find(axk::ObjectType::prog, "021");
    ASSERT_NE(bank, nullptr);
    ASSERT_NE(program, nullptr);
    const auto comparison = std::ranges::find(graph.bitmap_comparisons, bank->key, &axk::BitmapComparison::object_key);
    const auto foreign =
        std::ranges::find(graph.bitmap_comparisons, "foreign:" + bank->key, &axk::BitmapComparison::object_key);

    ASSERT_NE(comparison, graph.bitmap_comparisons.end());
    EXPECT_EQ(comparison->status, "match");
    EXPECT_EQ(comparison->direct_assignment_programs, (std::vector<std::uint8_t>{21U}));
    ASSERT_NE(foreign, graph.bitmap_comparisons.end());
    EXPECT_EQ(foreign->bitmap_without_direct, (std::vector<std::uint8_t>{21U}));
    EXPECT_EQ(foreign->direct_without_bitmap, (std::vector<std::uint8_t>{22U}));
    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {program->key}, .referrer_keys = {}, .cleanup_keys = {}});
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
}

TEST_P(ProgramBitmapIntegrity, DoesNotTreatAmbiguousBankNamesAsConfirmedAssignments) {
    const auto *bank = find(axk::ObjectType::sbac, "DX PER");
    ASSERT_NE(bank, nullptr);
    const auto bank_key = bank->key;
    auto duplicate = *bank;
    duplicate.key += ":duplicate";
    const auto duplicate_key = duplicate.key;
    catalog.objects.push_back(std::move(duplicate));
    graph = axk::build_relationship_graph(catalog);

    for (const auto &key : {bank_key, duplicate_key}) {
        const auto comparison = std::ranges::find(graph.bitmap_comparisons, key, &axk::BitmapComparison::object_key);
        ASSERT_NE(comparison, graph.bitmap_comparisons.end());
        EXPECT_TRUE(comparison->direct_assignment_programs.empty());
        EXPECT_EQ(comparison->bitmap_without_direct, (std::vector<std::uint8_t>{21U}));
        EXPECT_EQ(comparison->status, "mismatch");
    }
    const auto *program = find(axk::ObjectType::prog, "021");
    ASSERT_NE(program, nullptr);
    const auto inspected = axk::inspect_object_deletion(
        *container, catalog, graph, {.target_keys = {program->key}, .referrer_keys = {}, .cleanup_keys = {}});
    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->can_apply);
}

INSTANTIATE_TEST_SUITE_P(BothBankFormats, ProgramBitmapIntegrity,
                         testing::Values(axk::SampleStorageFormat::a3000_188,
                                         axk::SampleStorageFormat::a4000_a5000_224));

} // namespace
