#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/io.hpp"
#include "axklib/media.hpp"
#include "axklib/semantic.hpp"
#include "media_test_fixtures.hpp"

namespace {

class MediaSemanticValidation : public testing::Test {
  protected:
    std::optional<axk::MediaContainer> media;
    axk::MediaInventory inventory;
    axk::RelationshipGraph graph;

    void SetUp() override {
        auto opened = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(iso_fixture()), "source.iso");
        ASSERT_TRUE(opened) << opened.error().message;
        media.emplace(std::move(*opened));
    }

    void object(std::string key, axk::ObjectType type, std::string scope = "raw/F001") {
        axk::ObjectSnapshot item;
        item.key = std::move(key);
        item.sfs_id = axk::SfsId{static_cast<std::uint32_t>(inventory.catalog.objects.size() + 1U)};
        item.scope_key = std::move(scope);
        item.object.header.type = type;
        item.object.header.name = item.key;
        item.placement = axk::ObjectPlacement{{0U}, "Group", {1U}, "Same display name", "SBNK", item.key, {}};
        inventory.catalog.objects.push_back(std::move(item));
    }

    axk::Relationship &relationship(std::string source, std::string type,
                                    axk::RelationshipQuality quality = axk::RelationshipQuality::unknown,
                                    std::optional<std::string> target = {}) {
        axk::Relationship row;
        row.key = "r" + std::to_string(graph.relationships.size());
        row.source_key = std::move(source);
        row.type = std::move(type);
        row.quality = quality;
        row.target_key = std::move(target);
        if (row.type.starts_with("PROG_ASSIGNMENT_TO_")) {
            row.assignment_state = axk::AssignmentState::stored_assignment;
            row.assignment_index = 0U;
            row.assignment_name = "Assignment";
        }
        graph.relationships.push_back(std::move(row));
        return graph.relationships.back();
    }

    axk::ValidationReport validate() const { return axk::validate_semantics(*media, inventory, graph); }
};

TEST_F(MediaSemanticValidation, AggregatesStereoAndSamplesByNativeScopeNotDisplayedVolumeName) {
    object("sample1", axk::ObjectType::sbnk);
    object("sample2", axk::ObjectType::sbnk);
    object("sample3", axk::ObjectType::sbnk, "raw/F002");
    relationship("sample1", "SBNK_LEFT_MEMBER_TO_SMPL");
    relationship("sample1", "SBNK_RIGHT_MEMBER_TO_SMPL");
    relationship("sample2", "SBNK_LEFT_MEMBER_TO_SMPL");
    relationship("sample3", "SBNK_LEFT_MEMBER_TO_SMPL");
    const auto report = validate();
    EXPECT_TRUE(report.valid());
    ASSERT_EQ(report.issues.size(), 2U);
    EXPECT_EQ(report.issues[0].message,
              "3 Sample-to-Wave-Data link(s) across 2 Sample(s) do not resolve to Wave Data objects.");
    EXPECT_EQ(report.issues[1].message,
              "1 Sample-to-Wave-Data link(s) across 1 Sample(s) do not resolve to Wave Data objects.");
    for (const auto &issue : report.issues) {
        EXPECT_EQ(issue.code, "REL_SBNK_MEMBER_TARGET_MISSING");
        EXPECT_EQ(issue.severity, axk::ValidationSeverity::warning);
    }
    EXPECT_EQ(report.coverage.unknown_relationship_count, 4U);
}

TEST_F(MediaSemanticValidation, SharedContainerScopeDoesNotMergeDistinctVolumeIdentities) {
    for (const auto *sample : {"first", "second", "third"}) {
        object(sample, axk::ObjectType::sbnk, "partition:0");
        relationship(sample, "SBNK_LEFT_MEMBER_TO_SMPL");
    }
    inventory.catalog.objects[1].placement->volume_directory = axk::SfsId{2U};
    inventory.catalog.objects[2].placement->volume_directory = axk::SfsId{2U};
    const auto report = validate();
    ASSERT_EQ(report.issues.size(), 2U);
    EXPECT_TRUE(report.issues[0].message.starts_with("1 Sample-to-Wave-Data link(s) across 1 Sample(s)"));
    EXPECT_TRUE(report.issues[1].message.starts_with("2 Sample-to-Wave-Data link(s) across 2 Sample(s)"));
}

TEST_F(MediaSemanticValidation, DirectAndBankReachableMissingMembersAreErrorsButDormantMembersStayWarnings) {
    for (const auto *sample : {"direct", "member", "dormant"}) {
        object(sample, axk::ObjectType::sbnk);
        relationship(sample, "SBNK_LEFT_MEMBER_TO_SMPL");
    }
    object("bank", axk::ObjectType::sbac);
    object("program", axk::ObjectType::prog);
    relationship("bank", "SBAC_SLOT_TO_SBNK", axk::RelationshipQuality::known, "member");
    relationship("program", "PROG_ASSIGNMENT_TO_SBNK", axk::RelationshipQuality::known, "direct");
    relationship("program", "PROG_ASSIGNMENT_TO_SBAC", axk::RelationshipQuality::known, "bank");
    const auto report = validate();
    EXPECT_FALSE(report.valid());
    ASSERT_EQ(report.issues.size(), 2U);
    EXPECT_EQ(report.issues[0].code, "REL_ACTIVE_PROGRAM_SBNK_MEMBER_TARGET_MISSING");
    EXPECT_EQ(report.issues[0].severity, axk::ValidationSeverity::error);
    EXPECT_TRUE(report.issues[0].message.starts_with("2 Sample-to-Wave-Data link(s) across 2 Sample(s)"));
    EXPECT_NE(report.issues[0].sampler_path.find("assignment 1 Assignment"), std::string::npos);
    EXPECT_EQ(report.issues[1].code, "REL_SBNK_MEMBER_TARGET_MISSING");
    EXPECT_EQ(report.issues[1].severity, axk::ValidationSeverity::warning);
}

TEST_F(MediaSemanticValidation, TentativeProgramAndBankLinksDoNotMakeMissingWaveDataActive) {
    object("sample", axk::ObjectType::sbnk);
    object("bank", axk::ObjectType::sbac);
    object("program", axk::ObjectType::prog);
    relationship("sample", "SBNK_LEFT_MEMBER_TO_SMPL");
    relationship("program", "PROG_ASSIGNMENT_TO_SBNK", axk::RelationshipQuality::tentative, "sample");
    relationship("program", "PROG_ASSIGNMENT_TO_SBAC", axk::RelationshipQuality::known, "bank");
    relationship("bank", "SBAC_SLOT_TO_SBNK", axk::RelationshipQuality::tentative, "sample");
    const auto report = validate();
    EXPECT_TRUE(report.valid());
    EXPECT_EQ(report.issues.size(), 3U);
    EXPECT_EQ(std::ranges::count(report.issues, "REL_AMBIGUOUS_TARGET", &axk::ValidationIssue::code), 2);
    EXPECT_EQ(std::ranges::count(report.issues, "REL_SBNK_MEMBER_TARGET_MISSING", &axk::ValidationIssue::code), 1);
    EXPECT_EQ(graph.relationships[1].quality, axk::RelationshipQuality::tentative);
}

TEST_F(MediaSemanticValidation, CacheOnlyAndUnresolvedAssignmentDiagnosticsRetainQualityAndBasis) {
    object("sample", axk::ObjectType::sbnk);
    object("program", axk::ObjectType::prog);
    auto &cached = relationship("sample", "SBNK_LEFT_MEMBER_TO_SMPL", axk::RelationshipQuality::tentative);
    cached.basis = "sbnk-member-cache-only-name-mismatch";
    cached.candidate_keys = {"wave"};
    relationship("program", "PROG_ASSIGNMENT_TO_SBNK").basis = "stored-row";
    auto &source_load = relationship("program", "PROG_ASSIGNMENT_TO_SBAC");
    source_load.assignment_state = axk::AssignmentState::source_load_assignment;
    source_load.basis = "source-selector";
    const auto report = validate();
    EXPECT_TRUE(report.valid());
    ASSERT_EQ(report.issues.size(), 3U);
    EXPECT_EQ(report.issues[0].code, "REL_MISSING_TARGET");
    EXPECT_EQ(report.issues[0].basis, "source-selector");
    EXPECT_EQ(report.issues[1].code, "REL_PROGRAM_STORED_ROW_TARGET_MISSING");
    EXPECT_EQ(report.issues[1].basis, "stored-row");
    EXPECT_EQ(report.issues[2].code, "REL_SBNK_MEMBER_CACHE_DIAGNOSTIC");
    EXPECT_EQ(report.issues[2].quality, "Tentative");
    EXPECT_EQ(report.issues[2].basis, "sbnk-member-cache-only-name-mismatch");
    EXPECT_FALSE(graph.relationships.front().target_key);
    EXPECT_EQ(graph.relationships.front().candidate_keys, std::vector<std::string>{"wave"});
}

TEST_F(MediaSemanticValidation, ContainerFindingsAreErrorsEvenWithAnEmptyCatalog) {
    auto bytes = iso_fixture();
    std::fill_n(bytes.begin() + 22U * 2048U + 32U, 32U, std::byte{});
    const auto opened = axk::IsoImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes)), "bad.iso");
    ASSERT_TRUE(opened);
    media.emplace(*opened);
    const auto report = validate();
    ASSERT_EQ(report.issues.size(), 1U);
    EXPECT_FALSE(report.valid());
    EXPECT_EQ(report.issues.front().code, "ISO_YAMAHA_DSKNAME_ROW_MISSING");
    EXPECT_EQ(report.issues.front().severity, axk::ValidationSeverity::error);
    EXPECT_EQ(report.issues.front().scope, "container");
}

TEST_F(MediaSemanticValidation, CatalogDecodeErrorsBindToNativeKeysWithoutAssumingSfsIdentity) {
    object("iso9660:raw/F001/SMPL/F000", axk::ObjectType::smpl);
    const auto &item = inventory.catalog.objects.front();
    inventory.catalog.issues.push_back(
        {"media_object_decode_failed", "Malformed payload", item.partition, item.sfs_id});
    const auto report = validate();
    ASSERT_EQ(report.issues.size(), 1U);
    EXPECT_FALSE(report.valid());
    EXPECT_EQ(report.issues.front().code, "media_object_decode_failed");
    EXPECT_EQ(report.issues.front().object_key, item.key);
    EXPECT_EQ(report.issues.front().scope, "object");
}

TEST_F(MediaSemanticValidation, MetadataAndCompleteReadsHaveIdenticalMalformedPayloadFindings) {
    auto bytes = fat_fixture();
    be32(bytes, 4U * 512U + 0x1cU, 0xffffffffU);
    const auto opened = axk::FatImage::open(std::make_shared<axk::MemoryReader>(std::move(bytes)), "bad.ima");
    ASSERT_TRUE(opened);
    const axk::MediaContainer container{*opened};
    for (const auto mode : {axk::MediaObjectReadMode::complete, axk::MediaObjectReadMode::decoded_metadata}) {
        const auto loaded = axk::build_media_inventory(container, mode);
        ASSERT_TRUE(loaded) << loaded.error().message;
        const auto report = axk::validate_semantics(container, *loaded, axk::build_relationship_graph(loaded->catalog));
        EXPECT_FALSE(report.valid());
        std::string descriptions;
        for (const auto &issue : report.issues)
            descriptions += issue.code + ": " + issue.message + '\n';
        ASSERT_EQ(report.issues.size(), 2U) << descriptions;
        EXPECT_EQ(report.issues[0].code, "OBJECT_PAYLOAD_TRUNCATED");
        EXPECT_EQ(report.issues[1].code, "media_object_decode_failed");
        EXPECT_EQ(report.issues[0].object_key, loaded->catalog.objects.front().key);
        EXPECT_EQ(report.issues[1].object_key, loaded->catalog.objects.front().key);
    }
}

TEST_F(MediaSemanticValidation, MetadataSizeUsesPhysicalDescriptorRatherThanRetainedPrefix) {
    const auto opened = axk::IsoImage::open(
        std::make_shared<axk::MemoryReader>(iso_fixture_with_large_smpl(1024U * 1024U)), "large.iso");
    ASSERT_TRUE(opened);
    const axk::MediaContainer container{*opened};
    const auto loaded = axk::build_media_inventory(container, axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->objects.size(), 1U);
    EXPECT_LT(loaded->catalog.objects.front().raw_payload.size(), loaded->objects.front().size);
    const auto report = axk::validate_semantics(container, *loaded, axk::build_relationship_graph(loaded->catalog));
    EXPECT_TRUE(report.valid());
    EXPECT_TRUE(report.issues.empty());
}

TEST_F(MediaSemanticValidation, SfsStructuralScopeIsIdenticalThroughBothOverloads) {
    const auto source =
        axk::FileReader::open(std::filesystem::path{AXK_SOURCE_ROOT} /
                              "tests/fixtures/images/sampler-authored/HD00_512_single_sbnk_authored.hds");
    ASSERT_TRUE(source);
    ASSERT_LE((*source)->size(), 16U * 1024U * 1024U);
    std::vector<std::byte> bytes(static_cast<std::size_t>((*source)->size()));
    ASSERT_TRUE((*source)->read_exact_at(0U, bytes));
    const auto original = axk::open_image(std::make_shared<axk::MemoryReader>(bytes), "source.hds");
    ASSERT_TRUE(original);
    const auto &partition = original->partitions().front();
    const auto wave = std::ranges::find(partition.records, std::string{"SMPL"}, &axk::IndexRecord::object_type);
    ASSERT_NE(wave, partition.records.end());
    ASSERT_FALSE(wave->extents.empty());
    const auto offset =
        (static_cast<std::size_t>(partition.start_sector) +
         wave->extents.front().cluster_offset * static_cast<std::size_t>(partition.sectors_per_cluster)) *
        original->superblock().sector_size_bytes;
    ASSERT_LT(offset, bytes.size());
    bytes[offset] = std::byte{};
    const auto sfs = axk::open_image(std::make_shared<axk::MemoryReader>(std::move(bytes)), "corrupt.hds");
    ASSERT_TRUE(sfs);
    const axk::MediaContainer container{*sfs};
    const auto loaded = axk::build_media_inventory(container, axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(loaded);
    const auto relationships = axk::build_relationship_graph(loaded->catalog);
    const auto legacy = axk::validate_semantics(*sfs, loaded->catalog, relationships);
    const auto current = axk::validate_semantics(container, *loaded, relationships);
    const auto legacy_issue =
        std::ranges::find(legacy.issues, "SFS_VOLUME_UNRECOGNIZED_OBJECT_ENTRIES", &axk::ValidationIssue::code);
    const auto current_issue =
        std::ranges::find(current.issues, "SFS_VOLUME_UNRECOGNIZED_OBJECT_ENTRIES", &axk::ValidationIssue::code);
    ASSERT_NE(legacy_issue, legacy.issues.end());
    ASSERT_NE(current_issue, current.issues.end());
    EXPECT_EQ(legacy_issue->scope, "volume");
    EXPECT_EQ(current_issue->scope, legacy_issue->scope);
    EXPECT_EQ(current_issue->severity, legacy_issue->severity);
    EXPECT_EQ(current_issue->quality, legacy_issue->quality);
    EXPECT_EQ(current_issue->basis, legacy_issue->basis);
    EXPECT_EQ(current_issue->message, legacy_issue->message);
}

} // namespace
