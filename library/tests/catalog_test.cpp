#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <tuple>
#include <variant>

#include <gtest/gtest.h>

#include "axklib/catalog.hpp"
#include "axklib/media.hpp"
#include "axklib/relationship.hpp"
#include "axklib/semantic.hpp"
#include "axklib/sfs.hpp"

namespace {

std::filesystem::path fixture(std::string_view name) {
    return std::filesystem::path{AXK_SOURCE_ROOT} / "tests/fixtures/images/sampler-authored" / name;
}

const axk::ObjectSnapshot *object_named(const axk::ObjectCatalog &catalog, axk::ObjectType type,
                                        std::string_view name) {
    const auto found = std::ranges::find_if(catalog.objects, [&](const axk::ObjectSnapshot &item) {
        return item.object.header.type == type && item.object.header.name == name;
    });
    return found == catalog.objects.end() ? nullptr : &*found;
}

} // namespace

TEST(ObjectCatalog, DerivesExactVolumeAndCategoryPlacementFromDirectories) {
    const auto container = axk::open_image(fixture("HD00_512_single_sbnk_authored.hds"));
    ASSERT_TRUE(container);
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    EXPECT_TRUE(catalog->issues.empty());
    ASSERT_EQ(catalog->objects.size(), 17U);

    const auto *wave_data = object_named(*catalog, axk::ObjectType::smpl, "sine wave");
    ASSERT_NE(wave_data, nullptr);
    ASSERT_TRUE(wave_data->placement);
    EXPECT_EQ(wave_data->key, "p0:sfs9");
    EXPECT_EQ(wave_data->scope_key, "partition:0");
    EXPECT_EQ(wave_data->placement->partition_name, "New Partition");
    EXPECT_EQ(wave_data->placement->volume_name, "New Volume");
    EXPECT_EQ(wave_data->placement->category_name, "SMPL");
    EXPECT_EQ(wave_data->placement->entry_name, "sine wave");
    EXPECT_EQ(wave_data->placement_resolution, axk::PlacementResolution::exact);
    ASSERT_EQ(wave_data->placement_candidates.size(), 1U);
    EXPECT_EQ(wave_data->placement_candidates.front().volume_name, wave_data->placement->volume_name);
    const auto source_payload =
        container->read_record_data(wave_data->partition, wave_data->sfs_id, 64U * 1024U * 1024U);
    ASSERT_TRUE(source_payload) << source_payload.error().message;
    EXPECT_EQ(wave_data->raw_payload, *source_payload);
    const auto decoded_payload = axk::decode_object(wave_data->raw_payload);
    ASSERT_TRUE(decoded_payload) << decoded_payload.error().message;
    EXPECT_EQ(decoded_payload->header.type, wave_data->object.header.type);
    EXPECT_EQ(decoded_payload->header.name, wave_data->object.header.name);
}

TEST(ObjectCatalog, SfsMetadataInventoryRetainsDecodedTopologyWithoutRawPayloads) {
    const auto media = axk::open_media(fixture("HD00_512_single_sbnk_authored.hds"));
    ASSERT_TRUE(media) << media.error().message;
    const auto complete = axk::build_media_inventory(*media, axk::MediaObjectReadMode::complete);
    ASSERT_TRUE(complete) << complete.error().message;
    const auto metadata = axk::build_media_inventory(*media, axk::MediaObjectReadMode::decoded_metadata);
    ASSERT_TRUE(metadata) << metadata.error().message;

    ASSERT_TRUE(complete->raw_payloads_complete);
    ASSERT_FALSE(metadata->raw_payloads_complete);
    ASSERT_EQ(metadata->catalog.objects.size(), complete->catalog.objects.size());
    ASSERT_EQ(metadata->objects.size(), complete->objects.size());
    EXPECT_TRUE(
        std::ranges::all_of(complete->catalog.objects, [](const auto &object) { return !object.raw_payload.empty(); }));
    EXPECT_TRUE(
        std::ranges::all_of(metadata->catalog.objects, [](const auto &object) { return object.raw_payload.empty(); }));
    const auto relationship_signature = [](const axk::ObjectCatalog &catalog) {
        std::vector<
            std::tuple<std::string, std::string, std::optional<std::string>, std::string, axk::RelationshipQuality>>
            result;
        for (const auto &relationship : axk::build_relationship_graph(catalog).relationships) {
            result.emplace_back(relationship.key, relationship.source_key, relationship.target_key, relationship.type,
                                relationship.quality);
        }
        return result;
    };
    EXPECT_EQ(relationship_signature(metadata->catalog), relationship_signature(complete->catalog));
    for (std::size_t index = 0U; index < complete->catalog.objects.size(); ++index) {
        const auto &expected = complete->catalog.objects[index];
        const auto &actual = metadata->catalog.objects[index];
        EXPECT_EQ(actual.key, expected.key);
        EXPECT_EQ(actual.object.header.raw_type, expected.object.header.raw_type);
        EXPECT_EQ(actual.object.header.name, expected.object.header.name);
        EXPECT_EQ(metadata->objects[index].size, complete->objects[index].size);
        EXPECT_NE(metadata->objects[index].size, 0U);
    }
}

TEST(RelationshipGraph, MatchesMaintainedFixtureCountsAndSamplerHierarchy) {
    const auto container = axk::open_image(fixture("HD00_512_single_sbnk_authored.hds"));
    ASSERT_TRUE(container);
    const auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    const auto graph = axk::build_relationship_graph(*catalog);
    ASSERT_EQ(graph.relationships.size(), 9U);
    EXPECT_EQ(std::ranges::count_if(graph.relationships,
                                    [](const axk::Relationship &row) {
                                        return row.type == "SBNK_LEFT_MEMBER_TO_SMPL" &&
                                               row.quality == axk::RelationshipQuality::known;
                                    }),
              8);
    EXPECT_EQ(std::ranges::count_if(graph.relationships,
                                    [](const axk::Relationship &row) {
                                        return row.type == "SBAC_SLOT_TO_SBNK" &&
                                               row.quality == axk::RelationshipQuality::known;
                                    }),
              1);
    ASSERT_EQ(graph.bitmap_comparisons.size(), 8U);
    EXPECT_TRUE(std::ranges::all_of(graph.bitmap_comparisons, [](const auto &row) { return row.status == "match"; }));

    const auto tree = axk::build_content_tree(*container, *catalog, graph);
    ASSERT_EQ(tree.roots.size(), 1U);
    ASSERT_EQ(tree.roots[0].children.size(), 1U);
    const auto &volume = tree.roots[0].children[0];
    const auto sample_structure_category = std::ranges::find(
        volume.children, std::string{"Sample Banks/Samples (SBAC/SBNK)"}, &axk::ContentNode::display_name);
    ASSERT_NE(sample_structure_category, volume.children.end());
    const auto sample_bank = std::ranges::find(sample_structure_category->children, std::string{"B New SmpBank"},
                                               &axk::ContentNode::display_name);
    ASSERT_NE(sample_bank, sample_structure_category->children.end());
    EXPECT_EQ(sample_bank->object_type, "SBAC");
    ASSERT_FALSE(sample_bank->children.empty());
    EXPECT_EQ(sample_bank->children.front().object_type, "SBNK");

    const auto wave_data_category =
        std::ranges::find(volume.children, std::string{"Wave Data (SMPL)"}, &axk::ContentNode::display_name);
    ASSERT_NE(wave_data_category, volume.children.end());
    ASSERT_FALSE(wave_data_category->children.empty());
    EXPECT_EQ(wave_data_category->children.front().object_type, "SMPL");
    ASSERT_EQ(sample_bank->children.size(), 1U);
    EXPECT_EQ(sample_bank->children[0].display_name, "_NewSample");
}

TEST(RelationshipGraph, PreservesDuplicateMemberCandidatesWithoutKnownEdge) {
    const auto container = axk::open_image(fixture("HD00_512_single_sbnk_authored.hds"));
    ASSERT_TRUE(container);
    auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    const auto *original = object_named(*catalog, axk::ObjectType::smpl, "sine wave");
    ASSERT_NE(original, nullptr);
    auto duplicate = *original;
    duplicate.key = "p0:sfs999";
    duplicate.sfs_id = axk::SfsId{999};
    catalog->objects.push_back(std::move(duplicate));

    const auto graph = axk::build_relationship_graph(*catalog);
    const auto sample = object_named(*catalog, axk::ObjectType::sbnk, "sine wave");
    ASSERT_NE(sample, nullptr);
    const auto edges = graph.children(sample->key);
    ASSERT_EQ(edges.size(), 1U);
    EXPECT_EQ(edges[0]->quality, axk::RelationshipQuality::tentative);
    EXPECT_FALSE(edges[0]->target_key);
    EXPECT_EQ(edges[0]->candidate_keys.size(), 2U);
    const auto validation = axk::validate_semantics(*container, *catalog, graph);
    EXPECT_TRUE(std::ranges::none_of(validation.issues,
                                     [](const auto &issue) { return issue.code == "REL_SBNK_MEMBER_TARGET_MISSING"; }));
}

TEST(RelationshipGraph, RetainsRawLinkCandidatesFromMalformedSamplePayloads) {
    axk::ObjectCatalog catalog;
    axk::CurrentSbnk current_sample;
    current_sample.left.wave_data_name = "different name";
    current_sample.left.smpl_link_id = 42U;
    axk::DecodedObject sample;
    sample.header.type = axk::ObjectType::sbnk;
    sample.header.name = "Sample";
    sample.payload = std::move(current_sample);
    catalog.objects.push_back({"sample", axk::PartitionIndex{0}, axk::SfsId{1}, "scope", std::move(sample), {}});

    for (const auto key : {"sample-a", "sample-b"}) {
        std::vector<std::byte> raw(0x7cU);
        raw[0x7bU] = std::byte{42};
        axk::DecodedObject wave_data;
        wave_data.header.type = axk::ObjectType::smpl;
        wave_data.header.name = key;
        wave_data.payload = axk::GenericObject{std::move(raw)};
        catalog.objects.push_back({key, axk::PartitionIndex{0}, axk::SfsId{2}, "scope", std::move(wave_data), {}});
    }

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    EXPECT_EQ(graph.relationships.front().basis, "sbnk-member-link-ambiguous");
    EXPECT_EQ(graph.relationships.front().candidate_keys.size(), 2U);
}

TEST(RelationshipGraph, PrefersUniqueCrossFolderLinkBeforeDuplicateLocalNames) {
    axk::ObjectCatalog catalog;
    const axk::ObjectPlacement sample_placement{
        axk::PartitionIndex{0}, "GROUP", axk::SfsId{1}, "Volume", "SBNK", "Sample", "GROUP/F001"};
    axk::CurrentSbnk current_sample;
    current_sample.left.wave_data_name = "DUPLICATE";
    current_sample.left.smpl_link_id = 42U;
    axk::DecodedObject sample;
    sample.header.type = axk::ObjectType::sbnk;
    sample.header.name = "Sample";
    sample.payload = std::move(current_sample);
    catalog.objects.push_back(
        {"sample", axk::PartitionIndex{0}, axk::SfsId{1}, "iso:GROUP", std::move(sample), sample_placement});

    const auto add_wave_data = [&](std::string key, std::string name, std::uint32_t link, std::string folder) {
        axk::CurrentSmpl current_wave_data{};
        current_wave_data.link_id.value = link;
        axk::DecodedObject wave_data;
        wave_data.header.type = axk::ObjectType::smpl;
        wave_data.header.name = std::move(name);
        wave_data.payload = std::move(current_wave_data);
        auto placement = sample_placement;
        placement.container_directory = std::move(folder);
        catalog.objects.push_back({std::move(key), axk::PartitionIndex{0}, axk::SfsId{2}, "iso:GROUP",
                                   std::move(wave_data), std::move(placement)});
    };
    add_wave_data("local-name", "DUPLICATE", 7U, "GROUP/F001");
    add_wave_data("remote-name", "DUPLICATE", 8U, "GROUP/F002");
    add_wave_data("remote-link", "DIFFERENT", 42U, "GROUP/F002");

    const auto graph = axk::build_relationship_graph(catalog);
    const auto edges = graph.children("sample");
    ASSERT_EQ(edges.size(), 1U);
    ASSERT_TRUE(edges.front()->target_key);
    EXPECT_EQ(*edges.front()->target_key, "remote-link");
    EXPECT_EQ(edges.front()->quality, axk::RelationshipQuality::tentative);
    EXPECT_EQ(edges.front()->basis, "sbnk-member-link-id-only-iso-cross-folder-name-mismatch");
}

TEST(RelationshipGraph, TreatsExactMemberIdentitiesAsKnownWithinEachIsoRawVolume) {
    axk::ObjectCatalog catalog;
    const auto add_sample = [&](std::string key, std::string folder) {
        axk::CurrentSbnk current_sample;
        current_sample.left.wave_data_name = "PAD-L";
        current_sample.left.smpl_link_id = 42U;
        current_sample.right = axk::CurrentSbnkMember{.wave_data_name = "PAD-R", .smpl_link_id = 43U};
        axk::DecodedObject sample;
        sample.header.type = axk::ObjectType::sbnk;
        sample.header.name = "PAD-S";
        sample.payload = std::move(current_sample);
        const axk::ObjectPlacement placement{axk::PartitionIndex{0}, "GROUP", axk::SfsId{1}, "Volume", "SBNK", "PAD-S",
                                             std::move(folder)};
        catalog.objects.emplace_back(std::move(key), axk::PartitionIndex{0}, axk::SfsId{1}, "iso:GROUP",
                                     std::move(sample), placement, std::vector<std::byte>{}, std::vector{placement},
                                     axk::PlacementResolution::exact);
    };
    const auto add_wave_data = [&](std::string key, std::string name, std::uint32_t link, std::string folder) {
        axk::CurrentSmpl current_wave_data{};
        current_wave_data.link_id.value = link;
        axk::DecodedObject wave_data;
        wave_data.header.type = axk::ObjectType::smpl;
        wave_data.header.name = std::move(name);
        wave_data.payload = std::move(current_wave_data);
        const auto entry_name = wave_data.header.name;
        const axk::ObjectPlacement placement{
            axk::PartitionIndex{0}, "GROUP", axk::SfsId{1}, "Volume", "SMPL", entry_name, std::move(folder)};
        catalog.objects.emplace_back(std::move(key), axk::PartitionIndex{0}, axk::SfsId{2}, "iso:GROUP",
                                     std::move(wave_data), placement, std::vector<std::byte>{}, std::vector{placement},
                                     axk::PlacementResolution::exact);
    };

    add_sample("sample-f001", "GROUP/F001");
    add_sample("sample-f004", "GROUP/F004");
    add_wave_data("left-f001", "PAD-L", 42U, "GROUP/F001");
    add_wave_data("right-f001", "PAD-R", 43U, "GROUP/F001");
    add_wave_data("left-f004", "PAD-L", 42U, "GROUP/F004");
    add_wave_data("right-f004", "PAD-R", 43U, "GROUP/F004");

    const auto graph = axk::build_relationship_graph(catalog);
    const auto assert_member = [&](std::string_view source, std::string_view type, std::string_view target) {
        const auto found = std::ranges::find_if(graph.relationships, [&](const axk::Relationship &relationship) {
            return relationship.source_key == source && relationship.type == type;
        });
        ASSERT_NE(found, graph.relationships.end());
        ASSERT_TRUE(found->target_key);
        EXPECT_EQ(*found->target_key, target);
        EXPECT_EQ(found->quality, axk::RelationshipQuality::known);
        EXPECT_EQ(found->basis, "sbnk-member-link+name+same-folder");
        EXPECT_EQ(found->candidate_keys.size(), 2U);
    };
    assert_member("sample-f001", "SBNK_LEFT_MEMBER_TO_SMPL", "left-f001");
    assert_member("sample-f001", "SBNK_RIGHT_MEMBER_TO_SMPL", "right-f001");
    assert_member("sample-f004", "SBNK_LEFT_MEMBER_TO_SMPL", "left-f004");
    assert_member("sample-f004", "SBNK_RIGHT_MEMBER_TO_SMPL", "right-f004");
}

TEST(RelationshipGraph, KeepsDuplicateExactMembersWithinOneIsoRawVolumeTentative) {
    axk::ObjectCatalog catalog;
    const axk::ObjectPlacement sample_placement{
        axk::PartitionIndex{0}, "GROUP", axk::SfsId{1}, "Volume", "SBNK", "Sample", "GROUP/F001"};
    axk::CurrentSbnk current_sample;
    current_sample.left.wave_data_name = "DUPLICATE";
    current_sample.left.smpl_link_id = 42U;
    axk::DecodedObject sample;
    sample.header.type = axk::ObjectType::sbnk;
    sample.header.name = "Sample";
    sample.payload = std::move(current_sample);
    catalog.objects.emplace_back("sample", axk::PartitionIndex{0}, axk::SfsId{1}, "iso:GROUP", std::move(sample),
                                 sample_placement, std::vector<std::byte>{}, std::vector{sample_placement},
                                 axk::PlacementResolution::exact);

    for (const auto *key : {"wave-a", "wave-b"}) {
        axk::CurrentSmpl current_wave_data{};
        current_wave_data.link_id.value = 42U;
        axk::DecodedObject wave_data;
        wave_data.header.type = axk::ObjectType::smpl;
        wave_data.header.name = "DUPLICATE";
        wave_data.payload = std::move(current_wave_data);
        const axk::ObjectPlacement placement{
            axk::PartitionIndex{0}, "GROUP", axk::SfsId{1}, "Volume", "SMPL", "DUPLICATE", "GROUP/F001"};
        catalog.objects.emplace_back(key, axk::PartitionIndex{0}, axk::SfsId{2}, "iso:GROUP", std::move(wave_data),
                                     placement, std::vector<std::byte>{}, std::vector{placement},
                                     axk::PlacementResolution::exact);
    }

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    EXPECT_FALSE(graph.relationships.front().target_key);
    EXPECT_EQ(graph.relationships.front().quality, axk::RelationshipQuality::tentative);
    EXPECT_EQ(graph.relationships.front().basis, "sbnk-member-link+name-ambiguous");
    EXPECT_EQ(graph.relationships.front().candidate_keys.size(), 2U);
}

TEST(RelationshipGraph, KeepsIsoRawVolumeNameOnlyMemberMatchesLikely) {
    axk::ObjectCatalog catalog;
    const axk::ObjectPlacement sample_placement{
        axk::PartitionIndex{0}, "GROUP", axk::SfsId{1}, "Volume", "SBNK", "Sample", "GROUP/F001"};
    axk::CurrentSbnk current_sample;
    current_sample.left.wave_data_name = "DUPLICATE";
    current_sample.left.smpl_link_id = 99U;
    axk::DecodedObject sample;
    sample.header.type = axk::ObjectType::sbnk;
    sample.header.name = "Sample";
    sample.payload = std::move(current_sample);
    catalog.objects.emplace_back("sample", axk::PartitionIndex{0}, axk::SfsId{1}, "iso:GROUP", std::move(sample),
                                 sample_placement, std::vector<std::byte>{}, std::vector{sample_placement},
                                 axk::PlacementResolution::exact);

    const auto add_wave_data = [&](std::string key, std::uint32_t link, std::string folder) {
        axk::CurrentSmpl current_wave_data{};
        current_wave_data.link_id.value = link;
        axk::DecodedObject wave_data;
        wave_data.header.type = axk::ObjectType::smpl;
        wave_data.header.name = "DUPLICATE";
        wave_data.payload = std::move(current_wave_data);
        const axk::ObjectPlacement placement{
            axk::PartitionIndex{0}, "GROUP", axk::SfsId{1}, "Volume", "SMPL", "DUPLICATE", std::move(folder)};
        catalog.objects.emplace_back(std::move(key), axk::PartitionIndex{0}, axk::SfsId{2}, "iso:GROUP",
                                     std::move(wave_data), placement, std::vector<std::byte>{}, std::vector{placement},
                                     axk::PlacementResolution::exact);
    };
    add_wave_data("local", 1U, "GROUP/F001");
    add_wave_data("remote", 2U, "GROUP/F004");

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    ASSERT_TRUE(graph.relationships.front().target_key);
    EXPECT_EQ(*graph.relationships.front().target_key, "local");
    EXPECT_EQ(graph.relationships.front().quality, axk::RelationshipQuality::likely);
    EXPECT_EQ(graph.relationships.front().basis, "sbnk-member-name+same-folder");
    EXPECT_EQ(graph.relationships.front().candidate_keys.size(), 2U);
}

TEST(ProgramRelationships, UsesIsoBasisForMissingVisibleOffSampleBank) {
    axk::ObjectCatalog catalog;
    axk::CurrentProg current_program;
    axk::ProgAssignment assignment;
    assignment.name = "Missing Bank";
    assignment.raw_handle = 1U;
    assignment.kind = 0x11U;
    current_program.assignments.push_back(assignment);
    axk::DecodedObject program;
    program.header.type = axk::ObjectType::prog;
    program.header.name = "001";
    program.payload = std::move(current_program);
    catalog.objects.push_back(
        {"program", axk::PartitionIndex{0}, axk::SfsId{1}, "iso:GROUP", std::move(program),
         axk::ObjectPlacement{axk::PartitionIndex{0}, "GROUP", axk::SfsId{1}, "Volume", "PROG", "001", "GROUP/F001"}});

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    EXPECT_EQ(graph.relationships.front().assignment_state, axk::AssignmentState::visible_off);
    EXPECT_EQ(graph.relationships.front().basis, "assignment-visible-off-iso-missing-local-sbac");
}

TEST(ProgramRelationships, KeepsSourceLoadReceiveChannelUnknown) {
    axk::ObjectCatalog catalog;
    const axk::ObjectPlacement placement{
        axk::PartitionIndex{0}, "GROUP", axk::SfsId{1}, "Volume", "SBNK", "Sample", "GROUP/F001"};
    axk::DecodedObject sample;
    sample.header.type = axk::ObjectType::sbnk;
    sample.header.name = "Sample";
    sample.payload = axk::CurrentSbnk{};
    catalog.objects.push_back(
        {"sample", axk::PartitionIndex{0}, axk::SfsId{1}, "iso:GROUP", std::move(sample), placement});

    axk::CurrentProg current_program;
    axk::ProgAssignment assignment;
    assignment.name = "Sample";
    assignment.raw_handle = 1U;
    assignment.kind = 0x10U;
    assignment.raw_row[0x1d] = std::byte{0};
    assignment.raw_row[0x28] = std::byte{0};
    current_program.assignments.push_back(assignment);
    axk::DecodedObject program;
    program.header.type = axk::ObjectType::prog;
    program.header.name = "001";
    program.payload = std::move(current_program);
    auto program_placement = placement;
    program_placement.category_name = "PROG";
    program_placement.entry_name = "001";
    catalog.objects.push_back({"program", axk::PartitionIndex{0}, axk::SfsId{2}, "iso:GROUP", std::move(program),
                               std::move(program_placement)});

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    EXPECT_EQ(graph.relationships.front().assignment_state, axk::AssignmentState::source_load);
    EXPECT_EQ(graph.relationships.front().receive_channel_display, "unknown");
}

TEST(ProgramRelationships, ResolvesUnknownKindsByUniqueNameWithoutInventingACategory) {
    axk::ObjectCatalog catalog;
    axk::DecodedObject sample;
    sample.header.type = axk::ObjectType::sbnk;
    sample.header.name = "Named Object";
    sample.payload = axk::CurrentSbnk{};
    catalog.objects.push_back({"sample", axk::PartitionIndex{0}, axk::SfsId{1}, "scope", std::move(sample), {}});

    axk::CurrentProg current_program;
    axk::ProgAssignment assignment;
    assignment.name = "Named Object";
    assignment.raw_handle = 1U;
    assignment.kind = 0x22U;
    current_program.assignments.push_back(assignment);
    axk::DecodedObject program;
    program.header.type = axk::ObjectType::prog;
    program.header.name = "001";
    program.payload = std::move(current_program);
    catalog.objects.push_back({"program", axk::PartitionIndex{0}, axk::SfsId{2}, "scope", std::move(program), {}});

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    EXPECT_EQ(graph.relationships.front().type, "PROG_ASSIGNMENT_TO_OBJECT");
    EXPECT_EQ(graph.relationships.front().target_key, "sample");
    EXPECT_EQ(graph.relationships.front().basis, "assignment-name-unique");
}

TEST(ProgramRelationships, IgnoresUnclassifiedUnmatchedTailRows) {
    axk::ObjectCatalog catalog;
    axk::CurrentProg current_program;
    axk::ProgAssignment assignment;
    assignment.name = "tail bytes";
    assignment.raw_handle = 1U;
    assignment.kind = 0x22U;
    assignment.raw_row[0x28] = std::byte{0x40};
    current_program.assignments.push_back(assignment);
    axk::DecodedObject program;
    program.header.type = axk::ObjectType::prog;
    program.header.name = "001";
    program.payload = std::move(current_program);
    catalog.objects.push_back({"program", axk::PartitionIndex{0}, axk::SfsId{1}, "scope", std::move(program), {}});

    EXPECT_TRUE(axk::build_relationship_graph(catalog).relationships.empty());
}

TEST(ProgramRelationships, ClassifiesActiveMissingSampleTargets) {
    axk::ObjectCatalog catalog;
    axk::CurrentProg current_program;
    axk::ProgAssignment assignment;
    assignment.name = "Missing Sample";
    assignment.raw_handle = 1U;
    assignment.kind = 0x10U;
    assignment.raw_row[0x28] = std::byte{0xff};
    current_program.assignments.push_back(assignment);
    axk::DecodedObject program;
    program.header.type = axk::ObjectType::prog;
    program.header.name = "001";
    program.payload = std::move(current_program);
    catalog.objects.push_back({"program", axk::PartitionIndex{0}, axk::SfsId{1}, "scope", std::move(program), {}});

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    EXPECT_EQ(graph.relationships.front().basis, "assignment-active-missing-local-target");
}

TEST(ProgramRelationships, KeepsVisibleOffAmbiguousSampleBanksAsDiagnosticCandidates) {
    axk::ObjectCatalog catalog;
    for (const auto key : {"sample_bank-a", "sample_bank-b"}) {
        axk::DecodedObject sample_bank;
        sample_bank.header.type = axk::ObjectType::sbac;
        sample_bank.header.name = "Duplicate Bank";
        sample_bank.payload = axk::CurrentSbac{};
        catalog.objects.push_back(
            {key, axk::PartitionIndex{0}, axk::SfsId{1}, "iso:GROUP", std::move(sample_bank), {}});
    }
    axk::CurrentProg current_program;
    axk::ProgAssignment assignment;
    assignment.name = "Duplicate Bank";
    assignment.raw_handle = 1U;
    assignment.kind = 0x11U;
    current_program.assignments.push_back(assignment);
    axk::DecodedObject program;
    program.header.type = axk::ObjectType::prog;
    program.header.name = "001";
    program.payload = std::move(current_program);
    catalog.objects.push_back({"program", axk::PartitionIndex{0}, axk::SfsId{2}, "iso:GROUP", std::move(program), {}});

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    EXPECT_FALSE(graph.relationships.front().target_key);
    EXPECT_EQ(graph.relationships.front().candidate_keys.size(), 2U);
    EXPECT_EQ(graph.relationships.front().quality, axk::RelationshipQuality::tentative);
    EXPECT_EQ(graph.relationships.front().basis, "assignment-visible-off-name-ambiguous-sbac");
}

TEST(RelationshipGraph, ResolvesActiveSfsSampleBankMemberWithinItsOwningVolume) {
    axk::ObjectCatalog catalog;
    for (const auto &[key, volume] : {std::pair{"sample-a", 1U}, std::pair{"sample-b", 2U}}) {
        axk::DecodedObject sample;
        sample.header.type = axk::ObjectType::sbnk;
        sample.header.name = "Duplicate Sample";
        sample.payload = axk::CurrentSbnk{};
        catalog.objects.push_back(
            {key, axk::PartitionIndex{0}, axk::SfsId{volume}, "partition:0", std::move(sample),
             axk::ObjectPlacement{
                 axk::PartitionIndex{0}, "Disk", axk::SfsId{volume}, "Volume", "SBNK", "Duplicate Sample", {}}});
    }

    axk::CurrentSbac current_sample_bank;
    current_sample_bank.slots.push_back({"Duplicate Sample", 1U, 0x14cU});
    axk::DecodedObject sample_bank;
    sample_bank.header.type = axk::ObjectType::sbac;
    sample_bank.header.name = "Bank";
    sample_bank.payload = std::move(current_sample_bank);
    catalog.objects.push_back(
        {"sample_bank", axk::PartitionIndex{0}, axk::SfsId{3}, "partition:0", std::move(sample_bank),
         axk::ObjectPlacement{axk::PartitionIndex{0}, "Disk", axk::SfsId{2}, "Volume", "SBAC", "Bank", {}}});

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    ASSERT_TRUE(graph.relationships.front().target_key);
    EXPECT_EQ(*graph.relationships.front().target_key, "sample-b");
    EXPECT_EQ(graph.relationships.front().quality, axk::RelationshipQuality::known);
    EXPECT_EQ(graph.relationships.front().basis, "active-sbac-slot-name+same-volume");
    EXPECT_EQ(graph.relationships.front().candidate_keys.size(), 2U);
}

TEST(ProgramRelationships, IdentifiesOneSameVolumeVisibleOffDiagnosticCandidate) {
    axk::ObjectCatalog catalog;
    for (const auto &[key, volume] : {std::pair{"sample_bank-a", 1U}, std::pair{"sample_bank-b", 2U}}) {
        axk::DecodedObject sample_bank;
        sample_bank.header.type = axk::ObjectType::sbac;
        sample_bank.header.name = "Duplicate Bank";
        sample_bank.payload = axk::CurrentSbac{};
        catalog.objects.push_back({key, axk::PartitionIndex{0}, axk::SfsId{volume}, "partition:0",
                                   std::move(sample_bank),
                                   axk::ObjectPlacement{axk::PartitionIndex{0}, "Disk", axk::SfsId{volume}, "Volume",
                                                        "SBAC", "Duplicate Bank", "Disk/Volume/SBAC"}});
    }

    axk::CurrentProg current_program;
    axk::ProgAssignment assignment;
    assignment.name = "Duplicate Bank";
    assignment.raw_handle = 1U;
    assignment.kind = 0x11U;
    current_program.assignments.push_back(assignment);
    axk::DecodedObject program;
    program.header.type = axk::ObjectType::prog;
    program.header.name = "001";
    program.payload = std::move(current_program);
    catalog.objects.push_back({"program", axk::PartitionIndex{0}, axk::SfsId{3}, "partition:0", std::move(program),
                               axk::ObjectPlacement{axk::PartitionIndex{0}, "Disk", axk::SfsId{1}, "Volume", "PROG",
                                                    "001", "Disk/Volume/PROG"}});

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    EXPECT_FALSE(graph.relationships.front().target_key);
    EXPECT_EQ(graph.relationships.front().candidate_keys.size(), 2U);
    EXPECT_EQ(graph.relationships.front().quality, axk::RelationshipQuality::tentative);
    EXPECT_EQ(graph.relationships.front().basis, "assignment-visible-off-same-volume-sbac-diagnostic");
}

TEST(ProgramRelationships, MarksVisibleOffRepeatOfActiveTargetAsDuplicate) {
    axk::ObjectCatalog catalog;
    axk::DecodedObject sample;
    sample.header.type = axk::ObjectType::sbnk;
    sample.header.name = "Repeated Sample";
    sample.payload = axk::CurrentSbnk{};
    catalog.objects.push_back({"sample", axk::PartitionIndex{0}, axk::SfsId{1}, "partition:0", std::move(sample), {}});

    axk::ProgAssignment active;
    active.name = "Repeated Sample";
    active.raw_handle = 1U;
    active.kind = 0x10U;
    active.raw_row[0x28] = std::byte{0xff};
    auto visible_off = active;
    visible_off.raw_row[0x28] = std::byte{0};
    axk::CurrentProg current_program;
    current_program.assignments = {active, visible_off};
    axk::DecodedObject program;
    program.header.type = axk::ObjectType::prog;
    program.header.name = "001";
    program.payload = std::move(current_program);
    catalog.objects.push_back(
        {"program", axk::PartitionIndex{0}, axk::SfsId{2}, "partition:0", std::move(program), {}});

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 2U);
    EXPECT_EQ(graph.relationships[0].assignment_state, axk::AssignmentState::active);
    EXPECT_EQ(graph.relationships[1].assignment_state, axk::AssignmentState::duplicate_not_active);
}

TEST(WaveformOrphans, AllowsOnlyCompleteExactUnreferencedClassification) {
    const auto container = axk::open_image(fixture("HD00_512_single_sbnk_authored.hds"));
    ASSERT_TRUE(container);
    auto catalog = axk::build_object_catalog(*container);
    ASSERT_TRUE(catalog);
    const auto sample = object_named(*catalog, axk::ObjectType::sbnk, "sine wave");
    ASSERT_NE(sample, nullptr);
    std::erase_if(catalog->objects, [&](const axk::ObjectSnapshot &item) { return item.key == sample->key; });
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
    axk::DecodedObject sample;
    sample.header.type = axk::ObjectType::sbnk;
    sample.header.name = "Quiet Sample";
    sample.payload = axk::CurrentSbnk{};
    catalog.objects.push_back(
        {"p0:sfs10", axk::PartitionIndex{0}, axk::SfsId{10}, "partition:0", std::move(sample), {}});

    axk::CurrentProg current_program;
    axk::ProgAssignment assignment;
    assignment.name = "Quiet Sample";
    assignment.raw_handle = 1;
    assignment.kind = 0x10;
    assignment.raw_row[0x28] = std::byte{0};
    current_program.assignments.push_back(assignment);
    axk::DecodedObject program;
    program.header.type = axk::ObjectType::prog;
    program.header.name = "001";
    program.payload = std::move(current_program);
    catalog.objects.push_back(
        {"p0:sfs11", axk::PartitionIndex{0}, axk::SfsId{11}, "partition:0", std::move(program), {}});

    const auto graph = axk::build_relationship_graph(catalog);
    ASSERT_EQ(graph.relationships.size(), 1U);
    EXPECT_EQ(graph.relationships[0].quality, axk::RelationshipQuality::known);
    EXPECT_EQ(graph.relationships[0].assignment_state, axk::AssignmentState::visible_off);
    EXPECT_EQ(graph.relationships[0].receive_channel_display, "off");
    ASSERT_EQ(graph.bitmap_comparisons.size(), 1U);
    EXPECT_EQ(graph.bitmap_comparisons.front().mismatch_class, "nondefault_flag_direct_assignment_without_bitmap");
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
