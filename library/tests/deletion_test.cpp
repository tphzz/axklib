#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/alteration.hpp"
#include "axklib/audio.hpp"
#include "axklib/catalog.hpp"
#include "axklib/deletion.hpp"
#include "axklib/relationship.hpp"
#include "axklib/writer.hpp"

namespace {

axk::Waveform test_waveform() {
    axk::Waveform result;
    result.format = {1U, 2U, 44'100U};
    result.frame_count = 4U;
    result.pcm = {std::byte{0},    std::byte{0},    std::byte{0xe8}, std::byte{3},
                  std::byte{0x18}, std::byte{0xfc}, std::byte{0},    std::byte{0}};
    return result;
}

std::vector<std::byte> test_midi() {
    return {
        std::byte{'M'}, std::byte{'T'}, std::byte{'h'}, std::byte{'d'},  std::byte{0},    std::byte{0},
        std::byte{0},   std::byte{6},   std::byte{0},   std::byte{0},    std::byte{0},    std::byte{1},
        std::byte{0},   std::byte{96},  std::byte{'M'}, std::byte{'T'},  std::byte{'r'},  std::byte{'k'},
        std::byte{0},   std::byte{0},   std::byte{0},   std::byte{8},    std::byte{0},    std::byte{0x90},
        std::byte{60},  std::byte{100}, std::byte{0},   std::byte{0xff}, std::byte{0x2f}, std::byte{0},
    };
}

void write_bytes(const std::filesystem::path &path, std::span<const std::byte> content) {
    std::ofstream output{path, std::ios::binary};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char *>(content.data()), static_cast<std::streamsize>(content.size()));
    ASSERT_TRUE(output);
}

axk::HdsBuildManifest deletion_manifest(const std::filesystem::path &audio_path) {
    axk::HdsBuildManifest result{"1.0", 4U * 1024U * 1024U, {}};
    axk::VolumeSpec volume;
    volume.name = "Deletion";
    volume.waveforms.push_back({"wave", "Wave", audio_path, 60U, {}});
    axk::SampleSpec sample;
    sample.name = "Sample";
    sample.waveform_id = "wave";
    sample.root_key = 60U;
    sample.key_low = 0U;
    sample.key_high = 127U;
    volume.samples.push_back(std::move(sample));
    auto direct = volume.samples.front();
    direct.name = "Direct";
    volume.samples.push_back(std::move(direct));
    volume.sample_banks.push_back({"Bank", {"Sample"}});
    volume.programs.push_back({1U, "Pgm 001", {{"SBAC", "Bank", 1U}, {"SBNK", "Direct", 2U}}});
    result.partitions.push_back({"hd1", {std::move(volume)}});
    return result;
}

struct Fixture {
    std::filesystem::path root;
    axk::Container container;
    axk::ObjectCatalog catalog;
    axk::RelationshipGraph graph;

    ~Fixture() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

Fixture make_fixture(bool add_program = false, bool orphan_wave_data = false) {
    const auto root = std::filesystem::temp_directory_path() /
                      (add_program ? "axklib-deletion-planner-program" : "axklib-deletion-planner");
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const auto audio = root / "tone.wav";
    const auto authored = root / "authored.hds";
    const auto image = root / "source.hds";
    const auto written_audio = axk::write_wav_atomic(audio, test_waveform());
    EXPECT_TRUE(written_audio) << (written_audio ? "" : written_audio.error().message);
    const auto written_image = axk::write_hds_image(deletion_manifest(audio), authored);
    EXPECT_TRUE(written_image) << (written_image ? "" : written_image.error().message);
    if (add_program) {
        std::filesystem::copy_file(authored, image);
    } else {
        auto operations = std::vector<axk::AlterationOperation>{
            {"delete-program", axk::DeleteProgramOperation{axk::PartitionIndex{0U}, "Deletion", 1U}},
            {"delete-direct", axk::DeleteSampleOperation{axk::PartitionIndex{0U}, "Deletion", "Direct"}},
        };
        if (orphan_wave_data) {
            operations.push_back(
                {"delete-bank", axk::DeleteSampleBankOperation{axk::PartitionIndex{0U}, "Deletion", "Bank"}});
            operations.push_back(
                {"delete-sample", axk::DeleteSampleOperation{axk::PartitionIndex{0U}, "Deletion", "Sample"}});
        }
        const axk::AlterationManifest remove_program{
            "1.0",
            std::move(operations),
        };
        const auto altered = axk::alter_hds(authored, remove_program, image);
        EXPECT_TRUE(altered) << (altered ? "" : altered.error().message);
    }
    auto container = axk::open_image(image);
    EXPECT_TRUE(container);
    auto catalog = axk::build_object_catalog(*container);
    EXPECT_TRUE(catalog);
    auto graph = axk::build_relationship_graph(*catalog);
    return {root, std::move(*container), std::move(*catalog), std::move(graph)};
}

const axk::ObjectSnapshot &object(const Fixture &fixture, axk::ObjectType type) {
    return *std::ranges::find(fixture.catalog.objects, type, [](const auto &item) { return item.object.header.type; });
}

} // namespace

TEST(ObjectDeletion, PlansStandaloneSequenceDeletion) {
    const auto root = std::filesystem::temp_directory_path() / "axklib-deletion-planner-sequence";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root);
    const auto audio = root / "tone.wav";
    const auto midi = root / "sequence.mid";
    const auto source = root / "source.hds";
    const auto inserted = root / "inserted.hds";
    ASSERT_TRUE(axk::write_wav_atomic(audio, test_waveform()));
    write_bytes(midi, test_midi());
    ASSERT_TRUE(axk::write_hds_image(deletion_manifest(audio), source));
    const axk::AlterationManifest manifest{
        "1.0",
        {{"insert-sequence",
          axk::InsertSequenceOperation{
              axk::PartitionIndex{0U}, "Deletion", {"Sequence", midi, axk::SequenceSystemExclusivePolicy::reject}}}},
    };
    ASSERT_TRUE(axk::alter_hds(source, manifest, inserted));
    auto container = axk::open_image(inserted);
    ASSERT_TRUE(container) << container.error().message;
    auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog) << catalog.error().message;
    auto graph = axk::build_relationship_graph(*catalog);
    const auto sequence = std::ranges::find(catalog->objects, axk::ObjectType::sequ,
                                            [](const auto &object) { return object.object.header.type; });
    ASSERT_NE(sequence, catalog->objects.end());

    const auto inspected =
        axk::inspect_object_deletion(*container, *catalog, graph, {.target_keys = {sequence->key}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    ASSERT_EQ(inspected->impacts.size(), 1U);
    EXPECT_EQ(inspected->impacts.front().object_type, axk::ObjectType::sequ);
    ASSERT_EQ(inspected->manifest.operations.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<axk::DeleteSequenceOperation>(inspected->manifest.operations.front().data));
    std::filesystem::remove_all(root, error);
}

TEST(ObjectDeletion, KeepsDependenciesExplicitAndUncheckedByDefault) {
    const auto fixture = make_fixture();
    const auto &bank = object(fixture, axk::ObjectType::sbac);
    const auto &sample = object(fixture, axk::ObjectType::sbnk);
    const auto &wave = object(fixture, axk::ObjectType::smpl);

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_keys = {bank.key}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    EXPECT_EQ(inspected->selected_keys, std::vector<std::string>{bank.key});
    ASSERT_EQ(inspected->manifest.operations.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<axk::DeleteSampleBankOperation>(inspected->manifest.operations[0].data));
    const auto sample_impact =
        std::ranges::find(inspected->impacts, sample.key, &axk::ObjectDeletionImpact::object_key);
    const auto wave_impact = std::ranges::find(inspected->impacts, wave.key, &axk::ObjectDeletionImpact::object_key);
    ASSERT_NE(sample_impact, inspected->impacts.end());
    ASSERT_NE(wave_impact, inspected->impacts.end());
    EXPECT_EQ(sample_impact->status, axk::ObjectDeletionStatus::optional);
    EXPECT_FALSE(sample_impact->selected);
    EXPECT_EQ(wave_impact->status, axk::ObjectDeletionStatus::optional);
    EXPECT_FALSE(wave_impact->selected);
    EXPECT_EQ(wave_impact->prerequisite_keys, std::vector<std::string>{sample.key});
}

TEST(ObjectDeletion, IgnoresStoredProgramRowsWithoutAnEffectiveTarget) {
    auto fixture = make_fixture(true);
    auto program = std::ranges::find(fixture.catalog.objects, axk::ObjectType::prog,
                                     [](const auto &item) { return item.object.header.type; });
    ASSERT_NE(program, fixture.catalog.objects.end());
    auto *decoded = std::get_if<axk::CurrentProg>(&program->object.payload);
    ASSERT_NE(decoded, nullptr);

    axk::ProgAssignment stored;
    stored.name = "Missing Sample  *";
    stored.kind = 0x10U;
    stored.raw_row[0x28U] = std::byte{0xff};
    decoded->assignments.push_back(stored);
    fixture.graph.relationships.push_back({
        .key = "stored-row",
        .source_key = program->key,
        .target_key = std::nullopt,
        .candidate_keys = {},
        .type = "PROG_ASSIGNMENT_TO_SBNK",
        .quality = axk::RelationshipQuality::unknown,
        .basis = "assignment-stored-missing-local-target",
        .notes = {},
        .scope_key = program->scope_key,
        .assignment_index = decoded->assignments.size() - 1U,
        .assignment_name = stored.name,
        .assignment_state = axk::AssignmentState::stored_assignment,
        .receive_selector = std::nullopt,
        .receive_channel_display = "=Smp",
    });

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_keys = {program->key}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    const auto is_program_link_notice = [](const auto &notice) {
        return notice.code == "PROGRAM_ASSIGNMENT_UNRESOLVED" || notice.code == "PROGRAM_LINKS_INCONSISTENT";
    };
    EXPECT_TRUE(std::ranges::none_of(inspected->blockers, is_program_link_notice));
    EXPECT_TRUE(std::ranges::none_of(inspected->warnings, is_program_link_notice));
}

TEST(ObjectDeletion, OrdersSelectedDependencyClosureFromParentsToLeaves) {
    const auto fixture = make_fixture();
    const auto &bank = object(fixture, axk::ObjectType::sbac);
    const auto &sample = object(fixture, axk::ObjectType::sbnk);
    const auto &wave = object(fixture, axk::ObjectType::smpl);

    const auto inspected =
        axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                     {.target_keys = {bank.key}, .cleanup_keys = {wave.key, sample.key}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    ASSERT_EQ(inspected->manifest.operations.size(), 3U);
    EXPECT_TRUE(std::holds_alternative<axk::DeleteSampleBankOperation>(inspected->manifest.operations[0].data));
    EXPECT_TRUE(std::holds_alternative<axk::DeleteSampleOperation>(inspected->manifest.operations[1].data));
    EXPECT_TRUE(std::holds_alternative<axk::DeleteWaveformOperation>(inspected->manifest.operations[2].data));
    EXPECT_GT(inspected->estimated_freed_clusters, 0U);
    const auto partition = std::ranges::find(fixture.container.partitions(), bank.partition, &axk::Partition::index);
    ASSERT_NE(partition, fixture.container.partitions().end());
    const auto cluster_size_bytes =
        static_cast<std::uint64_t>(fixture.container.superblock().sector_size_bytes) * partition->sectors_per_cluster;
    EXPECT_EQ(inspected->estimated_freed_bytes, inspected->estimated_freed_clusters * cluster_size_bytes);
    const auto relationship =
        std::ranges::find(inspected->references, "SBNK_LEFT_MEMBER_TO_SMPL", &axk::ObjectDeletionReference::type);
    ASSERT_NE(relationship, inspected->references.end());
    EXPECT_EQ(relationship->effect, axk::ObjectDeletionReferenceEffect::removed);

    const auto output = fixture.root / "deleted.hds";
    const auto altered = axk::alter_hds(fixture.root / "source.hds", inspected->manifest, output);
    ASSERT_TRUE(altered) << altered.error().message;
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto remaining = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(remaining) << remaining.error().message;
    EXPECT_TRUE(std::ranges::none_of(remaining->objects, [](const auto &item) {
        return item.object.header.type == axk::ObjectType::sbac || item.object.header.type == axk::ObjectType::sbnk ||
               item.object.header.type == axk::ObjectType::smpl;
    }));
}

TEST(ObjectDeletion, BlocksTargetWithIncomingProgramReference) {
    const auto fixture = make_fixture(true);
    const auto &bank = object(fixture, axk::ObjectType::sbac);

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_keys = {bank.key}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->can_apply);
    EXPECT_FALSE(inspected->blockers.empty());
    EXPECT_TRUE(inspected->manifest.operations.empty());
}

TEST(ObjectDeletion, ResolvesReferencesAcrossAProgramAndSampleBankBatch) {
    const auto fixture = make_fixture(true);
    const auto &program = object(fixture, axk::ObjectType::prog);
    const auto &bank = object(fixture, axk::ObjectType::sbac);

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_keys = {bank.key, program.key}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    EXPECT_TRUE(inspected->blockers.empty());
    ASSERT_EQ(inspected->manifest.operations.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<axk::DeleteProgramOperation>(inspected->manifest.operations[0].data));
    EXPECT_TRUE(std::holds_alternative<axk::DeleteSampleBankOperation>(inspected->manifest.operations[1].data));
}

TEST(ObjectDeletion, KeepsBlockedRootsOutOfAnOtherwiseApplicableBatch) {
    const auto fixture = make_fixture(true);
    const auto &program = object(fixture, axk::ObjectType::prog);
    const auto &sample =
        *std::ranges::find(fixture.catalog.objects, "Sample", [](const auto &item) { return item.object.header.name; });

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_keys = {program.key, sample.key}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    EXPECT_EQ(inspected->selected_keys, std::vector<std::string>{program.key});
    const auto blocked = std::ranges::find(inspected->impacts, sample.key, &axk::ObjectDeletionImpact::object_key);
    ASSERT_NE(blocked, inspected->impacts.end());
    EXPECT_EQ(blocked->status, axk::ObjectDeletionStatus::blocked);
    EXPECT_FALSE(blocked->selected);
    ASSERT_EQ(inspected->manifest.operations.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<axk::DeleteProgramOperation>(inspected->manifest.operations.front().data));
}

TEST(ObjectDeletion, AppliesAHeterogeneousBatchFromParentsToWaveData) {
    const auto fixture = make_fixture(true);
    const auto &program = object(fixture, axk::ObjectType::prog);
    const auto &bank = object(fixture, axk::ObjectType::sbac);
    const auto &wave = object(fixture, axk::ObjectType::smpl);
    std::vector<std::string> targets{program.key, bank.key, wave.key};
    for (const auto &item : fixture.catalog.objects) {
        if (item.object.header.type == axk::ObjectType::sbnk)
            targets.push_back(item.key);
    }

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_keys = std::move(targets), .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    EXPECT_TRUE(inspected->blockers.empty());
    ASSERT_EQ(inspected->manifest.operations.size(), 5U);
    EXPECT_TRUE(std::holds_alternative<axk::DeleteProgramOperation>(inspected->manifest.operations[0].data));
    EXPECT_TRUE(std::holds_alternative<axk::DeleteSampleBankOperation>(inspected->manifest.operations[1].data));
    EXPECT_TRUE(std::holds_alternative<axk::DeleteSampleOperation>(inspected->manifest.operations[2].data));
    EXPECT_TRUE(std::holds_alternative<axk::DeleteSampleOperation>(inspected->manifest.operations[3].data));
    EXPECT_TRUE(std::holds_alternative<axk::DeleteWaveformOperation>(inspected->manifest.operations[4].data));

    const auto output = fixture.root / "batch-deleted.hds";
    const auto altered = axk::alter_hds(fixture.root / "source.hds", inspected->manifest, output);
    ASSERT_TRUE(altered) << altered.error().message;
    const auto reopened = axk::open_image(output);
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto remaining = axk::build_object_catalog(*reopened);
    ASSERT_TRUE(remaining) << remaining.error().message;
    EXPECT_TRUE(std::ranges::none_of(remaining->objects, [](const auto &item) {
        return item.object.header.type == axk::ObjectType::prog || item.object.header.type == axk::ObjectType::sbac ||
               item.object.header.type == axk::ObjectType::sbnk || item.object.header.type == axk::ObjectType::smpl;
    }));
}

TEST(ObjectDeletion, RejectsDependenciesThatAreNotInTheTargetClosure) {
    const auto fixture = make_fixture();
    const auto &sample = object(fixture, axk::ObjectType::sbnk);
    const auto &bank = object(fixture, axk::ObjectType::sbac);

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_keys = {sample.key}, .cleanup_keys = {bank.key}});

    ASSERT_FALSE(inspected);
    EXPECT_EQ(inspected.error().code, axk::ErrorCode::transaction_rejected);
}

TEST(ObjectDeletion, BlocksDirectDeletionOfReferencedSamplesAndWaveData) {
    const auto fixture = make_fixture();
    const auto &sample = object(fixture, axk::ObjectType::sbnk);
    const auto &wave = object(fixture, axk::ObjectType::smpl);

    const auto sample_inspection = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                                {.target_keys = {sample.key}, .cleanup_keys = {}});
    const auto wave_inspection = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                              {.target_keys = {wave.key}, .cleanup_keys = {}});

    ASSERT_TRUE(sample_inspection) << sample_inspection.error().message;
    EXPECT_FALSE(sample_inspection->can_apply);
    ASSERT_TRUE(wave_inspection) << wave_inspection.error().message;
    EXPECT_FALSE(wave_inspection->can_apply);
}

TEST(ObjectDeletion, AllowsConfirmedUnreferencedWaveData) {
    const auto fixture = make_fixture(false, true);
    const auto &wave = object(fixture, axk::ObjectType::smpl);

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_keys = {wave.key}, .cleanup_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->can_apply);
    ASSERT_EQ(inspected->manifest.operations.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<axk::DeleteWaveformOperation>(inspected->manifest.operations.front().data));
}
