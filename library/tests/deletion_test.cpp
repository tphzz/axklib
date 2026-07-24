#include <algorithm>
#include <filesystem>
#include <ranges>

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
    volume.programs.push_back({1U, {{"SBAC", "Bank", 1U}, {"SBNK", "Direct", 2U}}});
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

TEST(ObjectDeletion, KeepsDependenciesExplicitAndUncheckedByDefault) {
    const auto fixture = make_fixture();
    const auto &bank = object(fixture, axk::ObjectType::sbac);
    const auto &sample = object(fixture, axk::ObjectType::sbnk);
    const auto &wave = object(fixture, axk::ObjectType::smpl);

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_key = bank.key, .included_dependency_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->valid);
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

TEST(ObjectDeletion, OrdersSelectedDependencyClosureFromParentsToLeaves) {
    const auto fixture = make_fixture();
    const auto &bank = object(fixture, axk::ObjectType::sbac);
    const auto &sample = object(fixture, axk::ObjectType::sbnk);
    const auto &wave = object(fixture, axk::ObjectType::smpl);

    const auto inspected =
        axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                     {.target_key = bank.key, .included_dependency_keys = {wave.key, sample.key}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->valid);
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
                                                        {.target_key = bank.key, .included_dependency_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_FALSE(inspected->valid);
    EXPECT_FALSE(inspected->blockers.empty());
    EXPECT_TRUE(inspected->manifest.operations.empty());
}

TEST(ObjectDeletion, RejectsDependenciesThatAreNotInTheTargetClosure) {
    const auto fixture = make_fixture();
    const auto &sample = object(fixture, axk::ObjectType::sbnk);
    const auto &bank = object(fixture, axk::ObjectType::sbac);

    const auto inspected =
        axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                     {.target_key = sample.key, .included_dependency_keys = {bank.key}});

    ASSERT_FALSE(inspected);
    EXPECT_EQ(inspected.error().code, axk::ErrorCode::transaction_rejected);
}

TEST(ObjectDeletion, BlocksDirectDeletionOfReferencedSamplesAndWaveData) {
    const auto fixture = make_fixture();
    const auto &sample = object(fixture, axk::ObjectType::sbnk);
    const auto &wave = object(fixture, axk::ObjectType::smpl);

    const auto sample_inspection = axk::inspect_object_deletion(
        fixture.container, fixture.catalog, fixture.graph, {.target_key = sample.key, .included_dependency_keys = {}});
    const auto wave_inspection = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                              {.target_key = wave.key, .included_dependency_keys = {}});

    ASSERT_TRUE(sample_inspection) << sample_inspection.error().message;
    EXPECT_FALSE(sample_inspection->valid);
    ASSERT_TRUE(wave_inspection) << wave_inspection.error().message;
    EXPECT_FALSE(wave_inspection->valid);
}

TEST(ObjectDeletion, AllowsConfirmedUnreferencedWaveData) {
    const auto fixture = make_fixture(false, true);
    const auto &wave = object(fixture, axk::ObjectType::smpl);

    const auto inspected = axk::inspect_object_deletion(fixture.container, fixture.catalog, fixture.graph,
                                                        {.target_key = wave.key, .included_dependency_keys = {}});

    ASSERT_TRUE(inspected) << inspected.error().message;
    EXPECT_TRUE(inspected->valid);
    ASSERT_EQ(inspected->manifest.operations.size(), 1U);
    EXPECT_TRUE(std::holds_alternative<axk::DeleteWaveformOperation>(inspected->manifest.operations.front().data));
}
