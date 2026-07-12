#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <variant>

#include <gtest/gtest.h>

#include "axklib/catalog.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"

namespace {

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored" /
         name;
}

const axk::ObjectSnapshot* object_named(
    const axk::ObjectCatalog& catalog,
    axk::ObjectType type,
    std::string_view name) {
  const auto found = std::ranges::find_if(catalog.objects, [&](const axk::ObjectSnapshot& item) {
    return item.object.header.type == type && item.object.header.name == name;
  });
  return found == catalog.objects.end() ? nullptr : &*found;
}

}  // namespace

TEST(ObjectCatalog, DerivesExactVolumeAndCategoryPlacementFromDirectories) {
  const auto container = axk::open_image(fixture("HD00_512_single_sbnk_authored.hds"));
  ASSERT_TRUE(container);
  const auto catalog = axk::build_object_catalog(*container);
  ASSERT_TRUE(catalog);
  EXPECT_TRUE(catalog->issues.empty());
  ASSERT_EQ(catalog->objects.size(), 17U);

  const auto* sample = object_named(*catalog, axk::ObjectType::smpl, "sine wave");
  ASSERT_NE(sample, nullptr);
  ASSERT_TRUE(sample->placement);
  EXPECT_EQ(sample->key, "p0:sfs9");
  EXPECT_EQ(sample->scope_key, "partition:0");
  EXPECT_EQ(sample->placement->partition_name, "New Partition");
  EXPECT_EQ(sample->placement->volume_name, "New Volume");
  EXPECT_EQ(sample->placement->category_name, "SMPL");
  EXPECT_EQ(sample->placement->entry_name, "sine wave");
}

TEST(RelationshipGraph, MatchesMaintainedFixtureCountsAndSamplerHierarchy) {
  const auto container = axk::open_image(fixture("HD00_512_single_sbnk_authored.hds"));
  ASSERT_TRUE(container);
  const auto catalog = axk::build_object_catalog(*container);
  ASSERT_TRUE(catalog);
  const auto graph = axk::build_relationship_graph(*catalog);
  ASSERT_EQ(graph.relationships.size(), 9U);
  EXPECT_EQ(
      std::ranges::count_if(graph.relationships, [](const axk::Relationship& row) {
        return row.type == "SBNK_LEFT_MEMBER_TO_SMPL" &&
               row.quality == axk::RelationshipQuality::known;
      }),
      8);
  EXPECT_EQ(
      std::ranges::count_if(graph.relationships, [](const axk::Relationship& row) {
        return row.type == "SBAC_SLOT_TO_SBNK" &&
               row.quality == axk::RelationshipQuality::known;
      }),
      1);
  ASSERT_EQ(graph.bitmap_comparisons.size(), 8U);
  EXPECT_TRUE(std::ranges::all_of(graph.bitmap_comparisons, [](const auto& row) {
    return row.status == "match";
  }));

  const auto tree = axk::build_content_tree(*container, *catalog, graph);
  ASSERT_EQ(tree.roots.size(), 1U);
  ASSERT_EQ(tree.roots[0].children.size(), 1U);
  const auto& volume = tree.roots[0].children[0];
  const auto bank_category = std::ranges::find(
      volume.children, std::string{"Sample Banks"}, &axk::ContentNode::display_name);
  ASSERT_NE(bank_category, volume.children.end());
  const auto group = std::ranges::find(
      bank_category->children, std::string{"B New SmpBank"}, &axk::ContentNode::display_name);
  ASSERT_NE(group, bank_category->children.end());
  ASSERT_EQ(group->children.size(), 1U);
  EXPECT_EQ(group->children[0].display_name, "_NewSample");
}

TEST(RelationshipGraph, PreservesDuplicateMemberCandidatesWithoutKnownEdge) {
  const auto container = axk::open_image(fixture("HD00_512_single_sbnk_authored.hds"));
  ASSERT_TRUE(container);
  auto catalog = axk::build_object_catalog(*container);
  ASSERT_TRUE(catalog);
  const auto* original = object_named(*catalog, axk::ObjectType::smpl, "sine wave");
  ASSERT_NE(original, nullptr);
  auto duplicate = *original;
  duplicate.key = "p0:sfs999";
  duplicate.sfs_id = axk::SfsId{999};
  catalog->objects.push_back(std::move(duplicate));

  const auto graph = axk::build_relationship_graph(*catalog);
  const auto bank = object_named(*catalog, axk::ObjectType::sbnk, "sine wave");
  ASSERT_NE(bank, nullptr);
  const auto edges = graph.children(bank->key);
  ASSERT_EQ(edges.size(), 1U);
  EXPECT_EQ(edges[0]->quality, axk::RelationshipQuality::tentative);
  EXPECT_FALSE(edges[0]->target_key);
  EXPECT_EQ(edges[0]->candidate_keys.size(), 2U);
}

TEST(RelationshipGraph, PrefersUniqueCrossFolderLinkBeforeDuplicateLocalNames) {
  axk::ObjectCatalog catalog;
  const axk::ObjectPlacement bank_placement{axk::PartitionIndex{0},
                                             "GROUP",
                                             axk::SfsId{1},
                                             "Volume",
                                             "SBNK",
                                             "Bank",
                                             "GROUP/F001"};
  axk::CurrentSbnk current_bank;
  current_bank.left.sample_name = "DUPLICATE";
  current_bank.left.smpl_link_id = 42U;
  axk::DecodedObject bank;
  bank.header.type = axk::ObjectType::sbnk;
  bank.header.name = "Bank";
  bank.payload = std::move(current_bank);
  catalog.objects.push_back({"bank",
                             axk::PartitionIndex{0},
                             axk::SfsId{1},
                             "iso:GROUP",
                             std::move(bank),
                             bank_placement});

  const auto add_sample = [&](std::string key, std::string name, std::uint32_t link,
                              std::string folder) {
    axk::CurrentSmpl current_sample{};
    current_sample.link_id.value = link;
    axk::DecodedObject sample;
    sample.header.type = axk::ObjectType::smpl;
    sample.header.name = std::move(name);
    sample.payload = std::move(current_sample);
    auto placement = bank_placement;
    placement.container_directory = std::move(folder);
    catalog.objects.push_back({std::move(key),
                               axk::PartitionIndex{0},
                               axk::SfsId{2},
                               "iso:GROUP",
                               std::move(sample),
                               std::move(placement)});
  };
  add_sample("local-name", "DUPLICATE", 7U, "GROUP/F001");
  add_sample("remote-name", "DUPLICATE", 8U, "GROUP/F002");
  add_sample("remote-link", "DIFFERENT", 42U, "GROUP/F002");

  const auto graph = axk::build_relationship_graph(catalog);
  const auto edges = graph.children("bank");
  ASSERT_EQ(edges.size(), 1U);
  ASSERT_TRUE(edges.front()->target_key);
  EXPECT_EQ(*edges.front()->target_key, "remote-link");
  EXPECT_EQ(edges.front()->quality, axk::RelationshipQuality::tentative);
  EXPECT_EQ(edges.front()->basis,
            "sbnk-member-link-id-only-iso-cross-folder-name-mismatch");
}

TEST(ProgramRelationships, UsesIsoBasisForMissingVisibleOffSampleBankGroup) {
  axk::ObjectCatalog catalog;
  axk::CurrentProg current_program;
  axk::ProgAssignment assignment;
  assignment.name = "Missing Group";
  assignment.raw_handle = 1U;
  assignment.kind = 0x11U;
  current_program.assignments.push_back(assignment);
  axk::DecodedObject program;
  program.header.type = axk::ObjectType::prog;
  program.header.name = "001";
  program.payload = std::move(current_program);
  catalog.objects.push_back({"program",
                             axk::PartitionIndex{0},
                             axk::SfsId{1},
                             "iso:GROUP",
                             std::move(program),
                             axk::ObjectPlacement{axk::PartitionIndex{0},
                                                  "GROUP",
                                                  axk::SfsId{1},
                                                  "Volume",
                                                  "PROG",
                                                  "001",
                                                  "GROUP/F001"}});

  const auto graph = axk::build_relationship_graph(catalog);
  ASSERT_EQ(graph.relationships.size(), 1U);
  EXPECT_EQ(graph.relationships.front().assignment_state, axk::AssignmentState::visible_off);
  EXPECT_EQ(graph.relationships.front().basis,
            "assignment-visible-off-iso-missing-local-sbac");
}

TEST(ProgramRelationships, KeepsVisibleOffAmbiguousGroupsAsDiagnosticCandidates) {
  axk::ObjectCatalog catalog;
  for (const auto key : {"group-a", "group-b"}) {
    axk::DecodedObject group;
    group.header.type = axk::ObjectType::sbac;
    group.header.name = "Duplicate Group";
    group.payload = axk::CurrentSbac{};
    catalog.objects.push_back({key,
                               axk::PartitionIndex{0},
                               axk::SfsId{1},
                               "iso:GROUP",
                               std::move(group),
                               {}});
  }
  axk::CurrentProg current_program;
  axk::ProgAssignment assignment;
  assignment.name = "Duplicate Group";
  assignment.raw_handle = 1U;
  assignment.kind = 0x11U;
  current_program.assignments.push_back(assignment);
  axk::DecodedObject program;
  program.header.type = axk::ObjectType::prog;
  program.header.name = "001";
  program.payload = std::move(current_program);
  catalog.objects.push_back({"program",
                             axk::PartitionIndex{0},
                             axk::SfsId{2},
                             "iso:GROUP",
                             std::move(program),
                             {}});

  const auto graph = axk::build_relationship_graph(catalog);
  ASSERT_EQ(graph.relationships.size(), 1U);
  EXPECT_FALSE(graph.relationships.front().target_key);
  EXPECT_EQ(graph.relationships.front().candidate_keys.size(), 2U);
  EXPECT_EQ(graph.relationships.front().quality, axk::RelationshipQuality::tentative);
  EXPECT_EQ(graph.relationships.front().basis,
            "assignment-visible-off-name-ambiguous-sbac");
}

TEST(WaveformOrphans, AllowsOnlyCompleteExactUnreferencedClassification) {
  const auto container = axk::open_image(fixture("HD00_512_single_sbnk_authored.hds"));
  ASSERT_TRUE(container);
  auto catalog = axk::build_object_catalog(*container);
  ASSERT_TRUE(catalog);
  const auto bank = object_named(*catalog, axk::ObjectType::sbnk, "sine wave");
  ASSERT_NE(bank, nullptr);
  std::erase_if(catalog->objects, [&](const axk::ObjectSnapshot& item) {
    return item.key == bank->key;
  });
  auto graph = axk::build_relationship_graph(*catalog);
  auto report = axk::analyze_waveform_orphans(*container, *catalog, graph);
  EXPECT_EQ(report.known_unreferenced_count, 1U);
  EXPECT_EQ(report.referenced_count, 7U);

  catalog->issues.push_back({
      "CATALOG_OBJECT_PLACEMENT_MISSING",
      "another object has no exact placement",
      axk::PartitionIndex{0},
      axk::SfsId{999},
  });
  report = axk::analyze_waveform_orphans(*container, *catalog, graph);
  EXPECT_EQ(report.known_unreferenced_count, 0U);
  EXPECT_EQ(report.ambiguous_or_unresolved_count, 1U);
}

TEST(ProgramRelationships, KeepsVisibleOffRowsOutOfNavigableContent) {
  axk::ObjectCatalog catalog;
  axk::DecodedObject bank;
  bank.header.type = axk::ObjectType::sbnk;
  bank.header.name = "Quiet Bank";
  bank.payload = axk::CurrentSbnk{};
  catalog.objects.push_back({
      "p0:sfs10", axk::PartitionIndex{0}, axk::SfsId{10}, "partition:0", std::move(bank), {}});

  axk::CurrentProg current_program;
  axk::ProgAssignment assignment;
  assignment.name = "Quiet Bank";
  assignment.raw_handle = 1;
  assignment.kind = 0x10;
  assignment.raw_row[0x28] = std::byte{0};
  current_program.assignments.push_back(assignment);
  axk::DecodedObject program;
  program.header.type = axk::ObjectType::prog;
  program.header.name = "001";
  program.payload = std::move(current_program);
  catalog.objects.push_back({
      "p0:sfs11", axk::PartitionIndex{0}, axk::SfsId{11}, "partition:0", std::move(program), {}});

  const auto graph = axk::build_relationship_graph(catalog);
  ASSERT_EQ(graph.relationships.size(), 1U);
  EXPECT_EQ(graph.relationships[0].quality, axk::RelationshipQuality::known);
  EXPECT_EQ(graph.relationships[0].assignment_state, axk::AssignmentState::visible_off);
  EXPECT_EQ(graph.relationships[0].receive_channel_display, "off");
  ASSERT_EQ(graph.bitmap_comparisons.size(), 1U);
  EXPECT_EQ(graph.bitmap_comparisons.front().mismatch_class,
            "nondefault_flag_direct_assignment_without_bitmap");
}

TEST(Validation, ReportsStableRelationshipAndCoverageResults) {
  const auto container = axk::open_image(fixture("HD00_512_single_sbnk_authored.hds"));
  ASSERT_TRUE(container);
  const auto catalog = axk::build_object_catalog(*container);
  ASSERT_TRUE(catalog);
  const auto graph = axk::build_relationship_graph(*catalog);
  const auto report = axk::validate_semantics(*container, *catalog, graph);
  EXPECT_TRUE(report.valid());
  EXPECT_EQ(report.coverage.object_count, 17U);
  EXPECT_EQ(report.coverage.relationship_count, 9U);
  EXPECT_EQ(report.coverage.known_relationship_count, 9U);
  EXPECT_EQ(report.coverage.exact_placement_count, 17U);
}
