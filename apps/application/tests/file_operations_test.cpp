#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "axklib/application/file_operations.hpp"
#include "axklib/application/validation_operations.hpp"

namespace {

std::filesystem::path fixture_path() {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests" / "fixtures" / "images" / "sampler-authored" /
           "HD00_512_single_sbnk_authored.hds";
}

class FileOperationsTest : public testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() / "axklib-file-operations-test";
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        std::filesystem::create_directories(root_ / "reports");
        std::filesystem::copy_file(fixture_path(), root_ / "fixture.hds");
        auto sandbox = axk::app::Sandbox::create({{"workspace", "Workspace", root_, true}});
        ASSERT_TRUE(sandbox);
        sandbox_ = std::make_unique<axk::app::Sandbox>(std::move(*sandbox));
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
    std::unique_ptr<axk::app::Sandbox> sandbox_;
};

TEST_F(FileOperationsTest, RegistryBindsAllReadReportsToPersistentSandboxArtifacts) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox_));
    ASSERT_TRUE(axk::app::bind_validation_operations(registry, *sandbox_));
    for (const auto operation : {"report.info", "report.objects", "report.relationships", "report.inventory",
                                 "report.coverage", "report.orphans", "report.validate", "corpus.audit"}) {
        EXPECT_TRUE(registry.is_implemented(operation));
    }
    const auto result = registry.invoke(
        "report.objects",
        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports"}}}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_GT(result->at("rowCount").get<std::size_t>(), 0U);
    EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "reports" / "objects.json"));
    std::ifstream input{root_ / "reports" / "objects.json"};
    const std::string content{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    EXPECT_EQ(content.find(root_.string()), std::string::npos);
}

TEST_F(FileOperationsTest, ValidationWritesTheCompleteCliArtifactSetAndSummary) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_validation_operations(registry, *sandbox_));
    const auto result = registry.invoke(
        "report.validate",
        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/validation"}}},
         {"policy", "normal"}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("operationId"), "report.validate");
    EXPECT_EQ(result->at("sourceCount"), 1U);
    EXPECT_EQ(result->at("issueCount"), 0U);
    EXPECT_FALSE(result->at("failed").get<bool>());
    EXPECT_EQ(result->at("policy"), "normal");
    EXPECT_EQ(result->at("artifacts").size(), 24U);
    for (const auto &relative : {"validation_issues.csv",
                                 "validation_issues.json",
                                 "validation_summary.json",
                                 "allocation_summary.csv",
                                 "allocation_summary.json",
                                 "allocation_extents.csv",
                                 "allocation_extents.json",
                                 "allocation_mismatches.csv",
                                 "allocation_mismatches.json",
                                 "volume_validation.csv",
                                 "volume_validation.json",
                                 "volume_validation_issues.csv",
                                 "volume_validation_issues.json",
                                 "volume_validation_summary.csv",
                                 "volume_validation_summary.json",
                                 "_schemas/validation_issues.schema.json",
                                 "_schemas/validation_summary.schema.json",
                                 "_schemas/allocation_summary.schema.json",
                                 "_schemas/allocation_extents.schema.json",
                                 "_schemas/allocation_mismatches.schema.json",
                                 "_schemas/volume_validation.schema.json",
                                 "_schemas/volume_validation_issues.schema.json",
                                 "_schemas/volume_validation_summary.schema.json",
                                 "_schemas/schema_index.json"}) {
        EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "reports" / "validation" / relative)) << relative;
    }
    std::ifstream summary_input{root_ / "reports" / "validation" / "validation_summary.json"};
    const auto summary = nlohmann::json::parse(summary_input);
    EXPECT_EQ(summary, nlohmann::json({{"policy", "normal"},
                                       {"failed", false},
                                       {"issue_count", 0U},
                                       {"summary_counts", nlohmann::json::object()}}));
    std::ifstream volume_input{root_ / "reports" / "validation" / "volume_validation_summary.json"};
    const auto volume = nlohmann::json::parse(volume_input);
    ASSERT_EQ(volume.size(), 1U);
    EXPECT_EQ(volume.front().at("volume_count"), 1U);
    EXPECT_EQ(volume.front().at("pass_count"), 1U);
    EXPECT_EQ(volume.front().at("fail_count"), 0U);
}

TEST_F(FileOperationsTest, ValidationChecksExportTreesAndRejectsAmbiguousRequests) {
    std::filesystem::create_directories(root_ / "exports");
    std::ofstream{root_ / "exports" / "broken.json", std::ios::binary} << "{";
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_validation_operations(registry, *sandbox_));
    const auto context = axk::app::OperationContext{
        .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto result =
        registry.invoke("report.validate",
                        {{"exports", {{"rootId", "workspace"}, {"relativePath", "exports"}}},
                         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/export-validation"}}},
                         {"policy", "strict"}},
                        context);
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("issueCount"), 1U);
    EXPECT_TRUE(result->at("failed").get<bool>());
    EXPECT_EQ(result->at("policy"), "strict");
    EXPECT_EQ(result->at("artifacts").size(), 6U);
    std::ifstream issues_input{root_ / "reports" / "export-validation" / "validation_issues.json"};
    const auto issues = nlohmann::json::parse(issues_input);
    ASSERT_EQ(issues.size(), 1U);
    EXPECT_EQ(issues.front().at("code"), "EXPORT_SIDECAR_BAD_JSON");

    const auto ambiguous =
        registry.invoke("report.validate",
                        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                         {"exports", {{"rootId", "workspace"}, {"relativePath", "exports"}}},
                         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/ambiguous"}}}},
                        context);
    ASSERT_FALSE(ambiguous);
    EXPECT_EQ(ambiguous.error().code, "invalid_request");
}

TEST_F(FileOperationsTest, InfoReturnsCanonicalHierarchyWithoutRequiringAnArtifactDestination) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox_));
    const auto result = registry.invoke(
        "report.info",
        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}}, {"includeDefaultPrograms", false}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("operationId"), "report.info");
    EXPECT_EQ(result->at("sourceCount"), 1U);
    EXPECT_EQ(result->at("loadedCount"), 1U);
    EXPECT_EQ(result->at("failedCount"), 0U);
    ASSERT_EQ(result->at("trees").size(), 1U);
    const auto &tree = result->at("trees").front();
    EXPECT_EQ(tree.at("sourcePath"), "fixture.hds");
    EXPECT_EQ(tree.at("containerKind"), "sfs");
    EXPECT_EQ(tree.at("objectCount"), 17U);
    EXPECT_EQ(tree.at("objectCounts"), nlohmann::json({{"SBAC", 1U}, {"SBNK", 8U}, {"SMPL", 8U}}));
    ASSERT_EQ(tree.at("roots").size(), 1U);
    const auto &partition = tree.at("roots").front();
    EXPECT_EQ(partition.at("nodeId"), "partition:0");
    EXPECT_EQ(partition.at("selectorPath"), "partition_00_New_Partition");
    ASSERT_FALSE(partition.at("children").empty());
    const auto serialized = result->dump();
    EXPECT_EQ(serialized.find(root_.string()), std::string::npos);
}

TEST_F(FileOperationsTest, ObjectsWritesTheCompleteCliArtifactSetWithCanonicalRows) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox_));
    const auto result = registry.invoke(
        "report.objects",
        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/objects"}}},
         {"objectType", "SBNK"}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("rowCount"), 8U);
    EXPECT_EQ(result->at("loadedCount"), 1U);
    EXPECT_EQ(result->at("failedCount"), 0U);
    EXPECT_EQ(result->at("artifacts").size(), 4U);
    for (const auto &relative :
         {"objects.csv", "objects.json", "_schemas/objects.schema.json", "_schemas/schema_index.json"}) {
        EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "reports" / "objects" / relative)) << relative;
    }
    std::ifstream input{root_ / "reports" / "objects" / "objects.json"};
    const auto rows = nlohmann::json::parse(input);
    ASSERT_EQ(rows.size(), 8U);
    for (const auto &row : rows) {
        EXPECT_EQ(row.at("object_type"), "SBNK");
        EXPECT_EQ(row.at("source_path"), "fixture.hds");
        EXPECT_EQ(row.at("container_kind"), "sfs");
        EXPECT_EQ(row.at("payload_size"), 392U);
        EXPECT_TRUE(row.contains("payload_offset"));
        EXPECT_TRUE(row.contains("decoded_fields"));
    }
}

TEST_F(FileOperationsTest, InventoryWritesTheCompleteCliArtifactSetAndSummary) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox_));
    const auto result = registry.invoke(
        "report.inventory",
        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/inventory"}}}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("rowCount"), 17U);
    EXPECT_EQ(result->at("decodeIssueCount"), 0U);
    EXPECT_EQ(result->at("loadedCount"), 1U);
    EXPECT_EQ(result->at("failedCount"), 0U);
    EXPECT_EQ(result->at("artifacts").size(), 9U);
    for (const auto &relative :
         {"inventory_objects.csv", "inventory_objects.json", "decode_issues.csv", "decode_issues.json",
          "inventory_summary.json", "_schemas/inventory_objects.schema.json", "_schemas/decode_issues.schema.json",
          "_schemas/inventory_summary.schema.json", "_schemas/schema_index.json"}) {
        EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "reports" / "inventory" / relative)) << relative;
    }
    std::ifstream summary_input{root_ / "reports" / "inventory" / "inventory_summary.json"};
    const auto summary = nlohmann::json::parse(summary_input);
    EXPECT_EQ(summary.at("input_count"), 1U);
    EXPECT_EQ(summary.at("object_count"), 17U);
    EXPECT_EQ(summary.at("decode_issue_count"), 0U);
    EXPECT_EQ(summary.at("load_error_count"), 0U);
    EXPECT_EQ(summary.at("object_type_counts"), nlohmann::json({{"SBAC", 1U}, {"SBNK", 8U}, {"SMPL", 8U}}));
    EXPECT_TRUE(summary.at("load_errors").empty());
}

TEST_F(FileOperationsTest, OrphansWritesOwnershipRowsAndPerImageSummary) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox_));
    const auto result = registry.invoke(
        "report.orphans",
        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/orphans"}}}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("rowCount"), 8U);
    EXPECT_EQ(result->at("artifacts").size(), 7U);
    ASSERT_EQ(result->at("summaries").size(), 1U);
    const auto &summary = result->at("summaries").front();
    EXPECT_EQ(summary.at("sourcePath"), "fixture.hds");
    EXPECT_EQ(summary.at("waveformCount"), 8U);
    EXPECT_EQ(summary.at("referencedCount"), 8U);
    EXPECT_EQ(summary.at("knownUnreferencedCount"), 0U);
    EXPECT_EQ(summary.at("ambiguousOrUnresolvedCount"), 0U);
    for (const auto &relative : {"waveform_orphans.csv", "waveform_orphans.json", "waveform_orphan_summary.csv",
                                 "waveform_orphan_summary.json", "_schemas/waveform_orphans.schema.json",
                                 "_schemas/waveform_orphan_summary.schema.json", "_schemas/schema_index.json"}) {
        EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "reports" / "orphans" / relative)) << relative;
    }
}

TEST_F(FileOperationsTest, CoverageWritesCanonicalRelationshipsSummaryAndLoadErrors) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox_));
    const auto result = registry.invoke(
        "report.coverage",
        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/coverage"}}}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("rowCount"), 9U);
    EXPECT_EQ(result->at("failedCount"), 0U);
    EXPECT_EQ(result->at("artifacts").size(), 10U);
    std::ifstream summary_input{root_ / "reports" / "coverage" / "coverage_summary.json"};
    const auto summary = nlohmann::json::parse(summary_input);
    EXPECT_EQ(summary.at("relationship_count"), 9U);
    EXPECT_EQ(summary.at("known_relationship_count"), 9U);
    EXPECT_EQ(summary.at("sbac_sbnk_row_count"), 1U);
    EXPECT_EQ(summary.at("sbnk_bitmap_row_count"), 8U);
    EXPECT_EQ(summary.at("load_error_count"), 0U);
}

TEST_F(FileOperationsTest, RelationshipsWritesCanonicalAndSpecializedReportFamilies) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox_));
    const auto result = registry.invoke(
        "report.relationships",
        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/relationships"}}}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("rowCount"), 9U);
    EXPECT_EQ(result->at("ambiguousCount"), 0U);
    EXPECT_EQ(result->at("failedCount"), 0U);
    EXPECT_EQ(result->at("artifacts").size(), 21U);
    std::ifstream links_input{root_ / "reports" / "relationships" / "current_sbac_sbnk_links.json"};
    const auto links = nlohmann::json::parse(links_input);
    ASSERT_EQ(links.size(), 1U);
    EXPECT_EQ(links.front().at("sbac_name"), "New SmpBank");
    EXPECT_EQ(links.front().at("matched_sbnk_name"), "_NewSample");
    std::ifstream bitmap_input{root_ / "reports" / "relationships" / "current_sbnk_program_bitmap_crosscheck.json"};
    const auto bitmaps = nlohmann::json::parse(bitmap_input);
    EXPECT_EQ(bitmaps.size(), 8U);
}

TEST_F(FileOperationsTest, CorpusAuditWritesCanonicalArtifactsAndRetainsPerSourceFailures) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox_));
    const auto result = registry.invoke(
        "corpus.audit",
        {{"sources",
          {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}},
           {{"rootId", "workspace"}, {"relativePath", "missing.hds"}}}},
         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports"}}}},
        {.owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}});
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->at("loadedCount"), 1U);
    EXPECT_EQ(result->at("failedCount"), 1U);
    EXPECT_EQ(result->at("objectCount"), 17U);
    EXPECT_EQ(result->at("relationshipCount"), 9U);
    EXPECT_EQ(result->at("validationIssueCount"), 0U);
    EXPECT_EQ(result->at("waveSmokeDecoded"), 8U);
    EXPECT_EQ(result->at("waveSmokeErrorCount"), 0U);
    EXPECT_EQ(result->at("artifacts").size(), 14U);
    for (const auto &relative : {"corpus_audit_summary.json", "input_manifest.csv", "input_manifest.json",
                                 "inventory_objects.csv", "validation_issues.csv", "relationships.csv",
                                 "wave_smoke_issues.csv", "_schemas/corpus_audit_summary.schema.json",
                                 "_schemas/input_manifest.schema.json", "_schemas/inventory_objects.schema.json",
                                 "_schemas/validation_issues.schema.json", "_schemas/relationships.schema.json",
                                 "_schemas/wave_smoke_issues.schema.json", "_schemas/schema_index.json"}) {
        EXPECT_TRUE(std::filesystem::is_regular_file(root_ / "reports" / relative)) << relative;
    }
    std::ifstream summary_input{root_ / "reports" / "corpus_audit_summary.json"};
    const auto summary = nlohmann::json::parse(summary_input);
    EXPECT_EQ(summary.at("input_count"), 2U);
    EXPECT_EQ(summary.at("loaded_container_count"), 1U);
    EXPECT_EQ(summary.at("load_error_count"), 1U);
    std::ifstream manifest_input{root_ / "reports" / "input_manifest.json"};
    const auto manifest = nlohmann::json::parse(manifest_input);
    ASSERT_EQ(manifest.size(), 2U);
    EXPECT_TRUE(manifest.front().at("exists").get<bool>());
    EXPECT_FALSE(manifest.back().at("exists").get<bool>());
    EXPECT_EQ(manifest.back().at("path"), "missing.hds");
}

TEST_F(FileOperationsTest, CorpusAuditHonorsWaveSmokeLimitAndSkipPolicy) {
    auto registry = axk::app::make_operation_registry();
    ASSERT_TRUE(axk::app::bind_file_operations(registry, *sandbox_));
    const auto context = axk::app::OperationContext{
        .owner_id = "owner", .request_id = "request", .cancellation = {}, .progress = nullptr, .display_path = {}};
    const auto limited =
        registry.invoke("corpus.audit",
                        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/limited"}}},
                         {"waveSmokeLimit", 3U}},
                        context);
    ASSERT_TRUE(limited) << limited.error().message;
    EXPECT_EQ(limited->at("waveSmokeDecoded"), 3U);

    const auto skipped =
        registry.invoke("corpus.audit",
                        {{"sources", {{{"rootId", "workspace"}, {"relativePath", "fixture.hds"}}}},
                         {"destination", {{"rootId", "workspace"}, {"relativePath", "reports/skipped"}}},
                         {"skipWaveSmoke", true},
                         {"policy", "salvage-aware"}},
                        context);
    ASSERT_TRUE(skipped) << skipped.error().message;
    EXPECT_EQ(skipped->at("waveSmokeDecoded"), 0U);
    EXPECT_EQ(skipped->at("waveSmokeErrorCount"), 0U);
}

} // namespace
