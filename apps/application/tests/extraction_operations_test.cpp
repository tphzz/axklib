#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <ranges>
#include <set>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/extraction_operations.hpp"
#include "axklib/application/extraction_selection.hpp"
#include "axklib/application/volume_graph.hpp"
#include "axklib/audio.hpp"
#include "axklib/writer.hpp"

namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests" / "fixtures" / "images" / "sampler-authored" /
           "HD00_512_single_sbnk_authored.hds";
}

std::string read_text(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class CancelOnPhase final : public axk::ProgressSink {
  public:
    CancelOnPhase(axk::CancellationSource source, axk::ProgressPhase phase)
        : source_{std::move(source)}, phase_{phase} {}

    void report(const axk::Progress &progress) noexcept override {
        if (progress.phase == phase_)
            source_.cancel();
    }

  private:
    axk::CancellationSource source_;
    axk::ProgressPhase phase_;
};

bool has_publication_temporary(const std::filesystem::path &root) {
    return std::ranges::any_of(std::filesystem::directory_iterator{root}, [](const auto &entry) {
        return entry.path().filename().string().find(".axklib-publication.") != std::string::npos;
    });
}

void write_program_iso(const std::filesystem::path &root) {
    axk::Waveform waveform;
    waveform.format = {1, 2, 44100};
    waveform.frame_count = 4U;
    waveform.pcm = {std::byte{},     std::byte{},     std::byte{0x34}, std::byte{0x12},
                    std::byte{0xcc}, std::byte{0xed}, std::byte{},     std::byte{}};
    const auto audio_path = root / "program.wav";
    ASSERT_TRUE(axk::write_wav_atomic(audio_path, waveform));

    axk::VolumeSpec volume;
    volume.name = "Graph Volume";
    volume.waveforms.push_back({"wave", "Graph Wave", audio_path, 60U, {}});
    volume.sample_banks.push_back({"Grouped Bank", "wave", {}, {}, {}, {}, {}, 60U, 0U, 127U, 127U});
    volume.sample_banks.push_back({"Direct Bank", "wave", {}, {}, {}, {}, {}, 60U, 0U, 127U, 127U});
    volume.sample_bank_groups.push_back({"Graph Group", {"Grouped Bank"}});
    volume.programs.push_back({1U, {{"SBAC", "Graph Group", 1U}, {"SBNK", "Direct Bank", 2U}}});

    axk::MediaBuildManifest manifest;
    manifest.schema_version = "1.0";
    manifest.format = axk::MediaImageFormat::iso9660;
    manifest.authored_volume = std::move(volume);
    manifest.iso_volume_id = "AXK_GRAPH";
    manifest.raw_group = "00000010";
    manifest.group_name = "Authored Graph";
    manifest.raw_volume = "F001";
    manifest.volume_name = "Graph Volume";
    const auto written = axk::write_media_image(manifest, root / "program.iso");
    ASSERT_TRUE(written) << written.error().message;
}

class ExtractionOperationsTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-extraction-operations-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_);
        std::filesystem::copy_file(fixture_path(), root_ / "fixture.hds");
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox);
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
        registry_ = axk::app::make_operation_registry();
        ASSERT_TRUE(axk::app::bind_extraction_operations(registry_, *sandbox_));
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    axk::app::OperationContext context() const {
        return {
            .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    }

    std::filesystem::path root_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
    axk::app::OperationRegistry registry_;
};

TEST(ExtractionSelection, RetainsUnresolvedWaveDataOnlyForWholeImageOrConfirmedDependency) {
    axk::ExportPlan source;
    axk::UnresolvedWaveDataExport unresolved;
    unresolved.partition = axk::PartitionIndex{0};
    unresolved.relative_root = "partition_00_Test/Unresolved Wave Data";
    unresolved.waveforms.push_back({"wave", "Wave", "SMPL/Wave.wav", {}});
    source.unresolved_wave_data.push_back(std::move(unresolved));

    axk::RelationshipGraph graph;
    axk::Relationship relationship;
    relationship.source_key = "bank";
    relationship.target_key = "wave";
    relationship.type = "SBNK_LEFT_MEMBER_TO_SMPL";
    relationship.quality = axk::RelationshipQuality::known;
    graph.relationships.push_back(std::move(relationship));

    auto selected = source;
    axk::app::filter_export_plan(selected, graph, "sbnk", "Bank", "bank");
    ASSERT_EQ(selected.unresolved_wave_data.size(), 1U);
    EXPECT_EQ(selected.unresolved_wave_data.front().waveforms.size(), 1U);

    graph.relationships.front().quality = axk::RelationshipQuality::likely;
    selected = source;
    axk::app::filter_export_plan(selected, graph, "sbnk", "Bank", "bank");
    EXPECT_TRUE(selected.unresolved_wave_data.empty());

    selected = source;
    axk::app::filter_export_plan(selected, graph, "volume", "partition_00_Test/Volume", "volume");
    EXPECT_TRUE(selected.unresolved_wave_data.empty());
}

TEST(VolumeGraph, SerializesUnresolvedPlacementCandidatesAndResolutionQuality) {
    axk::UnresolvedWaveDataExport scope;
    scope.partition = axk::PartitionIndex{2};
    scope.partition_name = "Disk 3";
    scope.relative_root = "partition_02_Disk_3/Unresolved Wave Data";
    axk::PhysicalWaveformExport waveform;
    waveform.object_key = "p2:sfs9";
    waveform.display_name = "Hidden Wave";
    waveform.relative_wav_path = "SMPL/Hidden Wave.wav";
    waveform.placement_resolution = axk::PlacementResolution::ambiguous;
    waveform.placement_candidates.push_back(
        {axk::PartitionIndex{2}, "Disk 3", axk::SfsId{4}, "Vol A", "SMPL", "Hidden Wave", {}});
    waveform.placement_candidates.push_back(
        {axk::PartitionIndex{2}, "Disk 3", axk::SfsId{5}, "Vol B", "SMPL", "Hidden Wave", {}});
    scope.waveforms.push_back(std::move(waveform));

    const auto serialized = axk::app::serialize_unresolved_wave_data_graph(scope, "fixture.hds");
    ASSERT_TRUE(serialized) << serialized.error().message;
    const auto json = nlohmann::json::parse(*serialized);
    EXPECT_EQ(json.at("schema"), "axklib.unresolved_wave_data.v1");
    const auto &placement = json.at("objects").at("smpl").at(0).at("placement");
    EXPECT_EQ(placement.at("resolution"), "ambiguous");
    EXPECT_EQ(placement.at("quality"), "unresolved");
    EXPECT_EQ(placement.at("candidates").size(), 2U);
}

TEST_F(ExtractionOperationsTest, SfzPublishesPersistentCollectionAndContentAddressedResult) {
    const auto extracted = registry_.invoke("extract.sfz",
                                            {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                                             {"destination", {{"rootId", "workspace"}, {"relativePath", "export"}}},
                                             {"scope", "file"}},
                                            context());
    ASSERT_TRUE(extracted) << extracted.error().message;
    EXPECT_GT(extracted->at("artifactCount").get<std::size_t>(), 2U);
    EXPECT_EQ(extracted->at("artifactCount"), extracted->at("writtenFileCount"));
    EXPECT_EQ(extracted->at("selectionGraphCount"), 1U);
    EXPECT_EQ(extracted->at("sfzFileCount"), 8U);
    EXPECT_EQ(extracted->at("decodeErrorCount"), 0U);
    EXPECT_EQ(extracted->at("loadErrorCount"), 0U);
    EXPECT_FALSE(std::filesystem::exists(root_ / "export" / "extraction-manifest.json"));
    EXPECT_TRUE(std::ranges::any_of(std::filesystem::recursive_directory_iterator{root_ / "export"},
                                    [](const auto &entry) { return entry.path().extension() == ".wav"; }));
    EXPECT_TRUE(std::ranges::any_of(std::filesystem::recursive_directory_iterator{root_ / "export"},
                                    [](const auto &entry) { return entry.path().extension() == ".sfz"; }));
    const auto result_manifest = extracted->dump();
    EXPECT_EQ(result_manifest.find(root_.string()), std::string::npos);
    EXPECT_NE(result_manifest.find("\"sha256\""), std::string::npos);
    EXPECT_TRUE(std::ranges::all_of(extracted->at("artifacts"), [](const nlohmann::json &artifact) {
        if (artifact.at("sha256").get<std::string>().size() != 64U ||
            artifact.at("relativePath").get<std::string>().empty() || artifact.at("owners").empty()) {
            return false;
        }
        return std::ranges::all_of(artifact.at("owners"), [](const nlohmann::json &owner) {
            return owner.at("source").at("rootId") == "workspace" &&
                   owner.at("source").at("relativePath") == "fixture.hds" &&
                   !owner.at("objectType").get<std::string>().empty() &&
                   !owner.at("objectName").get<std::string>().empty();
        });
    }));
    for (const auto &artifact : extracted->at("artifacts")) {
        const auto relative = artifact.at("relativePath").get<std::string>();
        const auto path = root_ / "export" / relative;
        ASSERT_TRUE(std::filesystem::is_regular_file(path)) << path;
        EXPECT_EQ(std::filesystem::file_size(path), artifact.at("sizeBytes"));
    }

    const auto refused = registry_.invoke("extract.wav",
                                          {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                                           {"destination", {{"rootId", "workspace"}, {"relativePath", "export"}}}},
                                          context());
    EXPECT_FALSE(refused);
}

TEST_F(ExtractionOperationsTest, VolumeAndProgramSelectionsPublishOnlyTheirDependencyClosures) {
    const auto volume = registry_.invoke("extract.wav",
                                         {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                                          {"destination", {{"rootId", "workspace"}, {"relativePath", "volume"}}},
                                          {"scope", "volume"},
                                          {"selectors", {"partition_00_New_Partition/New Volume"}}},
                                         context());
    ASSERT_TRUE(volume) << volume.error().message;
    EXPECT_EQ(volume->at("selectionGraphCount"), 1U);

    write_program_iso(root_);
    const auto program = registry_.invoke("extract.sfz",
                                          {{"sources", {{{"rootId", "workspace"}, {"relativePath", "program.iso"}}}},
                                           {"destination", {{"rootId", "workspace"}, {"relativePath", "program"}}},
                                           {"scope", "program"},
                                           {"selectors", {"Authored Graph/Graph Volume/Programs/001: Pgm 001"}}},
                                          context());
    ASSERT_TRUE(program) << program.error().message;
    const auto graph_path = root_ / "program" / "program" / "Authored Graph" / "Graph Volume" / "Programs" /
                            "001_ Pgm 001" / "volume.axklib.json";
    ASSERT_TRUE(std::filesystem::is_regular_file(graph_path)) << graph_path;
    const auto graph = nlohmann::json::parse(read_text(graph_path));
    EXPECT_EQ(graph.at("objects").at("prog").size(), 1U);
    EXPECT_EQ(graph.at("objects").at("sbac").size(), 1U);
    EXPECT_EQ(graph.at("objects").at("sbnk").size(), 2U);
    EXPECT_EQ(graph.at("objects").at("smpl").size(), 1U);
}

TEST_F(ExtractionOperationsTest, StrictAndNonStrictSourceFailuresPublishAtomically) {
    std::ofstream{root_ / "malformed.bin", std::ios::binary} << "not a Yamaha image";
    const auto sources = nlohmann::json::array({{{"rootId", "workspace"}, {"relativePath", "malformed.bin"}},
                                                {{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}});
    const auto strict = registry_.invoke("extract.wav",
                                         {{"sources", sources},
                                          {"destination", {{"rootId", "workspace"}, {"relativePath", "strict"}}},
                                          {"scope", "file"},
                                          {"strict", true}},
                                         context());
    EXPECT_FALSE(strict);
    EXPECT_FALSE(std::filesystem::exists(root_ / "strict"));
    EXPECT_FALSE(has_publication_temporary(root_));

    const auto tolerant = registry_.invoke("extract.wav",
                                           {{"sources", sources},
                                            {"destination", {{"rootId", "workspace"}, {"relativePath", "tolerant"}}},
                                            {"scope", "file"},
                                            {"strict", false}},
                                           context());
    ASSERT_TRUE(tolerant) << tolerant.error().message;
    EXPECT_EQ(tolerant->at("loadErrorCount"), 1U);
    ASSERT_EQ(tolerant->at("warnings").size(), 1U);
    EXPECT_EQ(tolerant->at("warnings").front().at("code"), "source_failed");
    EXPECT_TRUE(std::ranges::all_of(tolerant->at("artifacts"), [](const nlohmann::json &artifact) {
        return std::ranges::all_of(artifact.at("owners"), [](const nlohmann::json &owner) {
            return owner.at("source").at("relativePath") == "fixture.hds";
        });
    }));
}

TEST_F(ExtractionOperationsTest, DuplicateSanitizedSourceRootsFailBeforePublication) {
    std::filesystem::copy_file(root_ / "fixture.hds", root_ / "same name.hds");
    std::filesystem::copy_file(root_ / "fixture.hds", root_ / "same  name.hds");
    const auto extracted = registry_.invoke("extract.wav",
                                            {{"sources",
                                              {{{"rootId", "workspace"}, {"relativePath", "same name.hds"}},
                                               {{"rootId", "workspace"}, {"relativePath", "same  name.hds"}}}},
                                             {"destination", {{"rootId", "workspace"}, {"relativePath", "collision"}}},
                                             {"scope", "file"}},
                                            context());
    EXPECT_FALSE(extracted);
    EXPECT_EQ(extracted.error().code, "artifact_collision");
    EXPECT_FALSE(std::filesystem::exists(root_ / "collision"));
    EXPECT_FALSE(has_publication_temporary(root_));
}

TEST_F(ExtractionOperationsTest, ExplicitOverwriteReplacesExistingCollectionAtomically) {
    std::filesystem::create_directory(root_ / "replace");
    std::ofstream{root_ / "replace" / "old.txt", std::ios::binary} << "preserve";
    const auto request = nlohmann::json{{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                                        {"destination", {{"rootId", "workspace"}, {"relativePath", "replace"}}},
                                        {"scope", "file"}};
    const auto refused = registry_.invoke("extract.wav", request, context());
    EXPECT_FALSE(refused);
    EXPECT_EQ(read_text(root_ / "replace" / "old.txt"), "preserve");

    auto overwrite = request;
    overwrite["overwrite"] = true;
    const auto replaced = registry_.invoke("extract.wav", overwrite, context());
    ASSERT_TRUE(replaced) << replaced.error().message;
    EXPECT_FALSE(std::filesystem::exists(root_ / "replace" / "old.txt"));
    EXPECT_EQ(replaced->at("artifactCount"), replaced->at("writtenFileCount"));
    EXPECT_FALSE(has_publication_temporary(root_));
}

TEST_F(ExtractionOperationsTest, CancellationAtReadingAndExportingPublishesNoCollection) {
    for (const auto &[phase, destination] : {std::pair{axk::ProgressPhase::reading, "cancel-reading"},
                                             std::pair{axk::ProgressPhase::exporting, "cancel-exporting"}}) {
        axk::CancellationSource cancellation;
        CancelOnPhase progress{cancellation, phase};
        auto operation_context = context();
        operation_context.cancellation = cancellation.token();
        operation_context.progress = &progress;
        const auto extracted =
            registry_.invoke("extract.sfz",
                             {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                              {"destination", {{"rootId", "workspace"}, {"relativePath", destination}}},
                              {"scope", "file"}},
                             operation_context);
        EXPECT_FALSE(extracted);
        EXPECT_EQ(extracted.error().code, "operation_cancelled");
        EXPECT_FALSE(std::filesystem::exists(root_ / destination));
        EXPECT_FALSE(has_publication_temporary(root_));
    }
}

TEST_F(ExtractionOperationsTest, VolumeSelectionRejectsMissingVolumeWithoutPublishingDirectory) {
    const auto extracted = registry_.invoke("extract.wav",
                                            {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                                             {"destination", {{"rootId", "workspace"}, {"relativePath", "missing"}}},
                                             {"scope", "volume"},
                                             {"selectors", {{{"path", "partition_00_New_Partition/Absent"}}}}},
                                            context());
    EXPECT_FALSE(extracted);
    EXPECT_EQ(extracted.error().code, "selector_not_found");
    EXPECT_FALSE(std::filesystem::exists(root_ / "missing"));
}

TEST_F(ExtractionOperationsTest, SbnkPathSelectionRetainsOnlyTheSelectedDependencyClosure) {
    constexpr std::string_view selector = "partition_00_New_Partition/New Volume/Sample Banks and Samples/sine wave";
    const auto extracted = registry_.invoke("extract.wav",
                                            {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                                             {"destination", {{"rootId", "workspace"}, {"relativePath", "bank"}}},
                                             {"scope", "sbnk"},
                                             {"selectors", {{{"path", selector}}}}},
                                            context());
    ASSERT_TRUE(extracted) << extracted.error().message;
    const auto graph_path = root_ / "bank" / "sbnk" / "partition_00_New_Partition" / "New Volume" /
                            "Sample Banks and Samples" / "sine wave" / "volume.axklib.json";
    ASSERT_TRUE(std::filesystem::is_regular_file(graph_path));
    const auto graph = nlohmann::json::parse(read_text(graph_path));
    ASSERT_EQ(graph.at("objects").at("sbnk").size(), 1U);
    EXPECT_EQ(graph.at("objects").at("sbnk").front().at("display_name"), "sine wave");
    EXPECT_EQ(graph.at("objects").at("sbac").size(), 0U);
    EXPECT_EQ(graph.at("objects").at("prog").size(), 0U);
    EXPECT_EQ(graph.at("objects").at("smpl").size(), 1U);
}

TEST_F(ExtractionOperationsTest, SbacPathSelectionRetainsItsMemberBankAndWaveform) {
    constexpr std::string_view selector =
        "partition_00_New_Partition/New Volume/Sample Banks and Samples/B New SmpBank";
    const auto extracted = registry_.invoke("extract.sfz",
                                            {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                                             {"destination", {{"rootId", "workspace"}, {"relativePath", "group"}}},
                                             {"scope", "sbac"},
                                             {"selectors", {selector}}},
                                            context());
    ASSERT_TRUE(extracted) << extracted.error().message;
    const auto graph_path = root_ / "group" / "sbac" / "partition_00_New_Partition" / "New Volume" /
                            "Sample Banks and Samples" / "B New SmpBank" / "volume.axklib.json";
    ASSERT_TRUE(std::filesystem::is_regular_file(graph_path));
    const auto graph = nlohmann::json::parse(read_text(graph_path));
    ASSERT_EQ(graph.at("objects").at("sbac").size(), 1U);
    EXPECT_EQ(graph.at("objects").at("sbac").front().at("display_name"), "New SmpBank");
    EXPECT_EQ(graph.at("objects").at("sbnk").size(), 1U);
    EXPECT_EQ(graph.at("objects").at("smpl").size(), 1U);
}

} // namespace
