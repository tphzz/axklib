#include <algorithm>
#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "schema/export_v1.hpp"
#include "schema/info_v1.hpp"
#include "schema/objects_v1.hpp"
#include "schema/operations_v1.hpp"
#include "schema/package_v1.hpp"

namespace schema = axk::cli::schema::operations_v1;
namespace info_schema = axk::cli::schema::info_v1;
namespace object_schema = axk::cli::schema::objects_v1;
namespace export_schema = axk::cli::schema::export_v1;
namespace package_schema = axk::cli::schema::package_v1;

TEST(CliSchema, VolumeGraphSchemaStaysParseable) {
    EXPECT_EQ(info_schema::schema_version, "compat-v1");
    EXPECT_EQ(schema::alteration_schema_version, "compat-v1");
    EXPECT_EQ(export_schema::volume_graph_schema_version, "axklib.volume_graph.v1");
    EXPECT_EQ(object_schema::schema_version, "1.0");

    axk::RelationshipGraph graph;
    graph.relationships.push_back({
        .key = "edge",
        .source_key = "source",
        .target_key = std::nullopt,
        .candidate_keys = {},
        .type = "TEST",
        .quality = axk::RelationshipQuality::unknown,
        .basis = "test",
        .notes = "",
        .scope_key = "scope",
        .assignment_index = std::nullopt,
        .assignment_name = "",
        .assignment_state = axk::AssignmentState::stored_assignment,
        .receive_selector = std::nullopt,
        .receive_channel_display = "=Smp",
    });
    axk::VolumeExport volume;
    axk::Waveform non_looping;
    non_looping.loop_start = 66U;
    non_looping.loop_length = 0U;
    volume.waveforms.push_back({"waveform", "Waveform", "SMPL/Waveform.wav", non_looping});
    volume.waveforms.back().user_facing_aliases.push_back(
        {"sample", "Sampler-visible Sample", axk::RelationshipQuality::likely});
    auto wide_loop = non_looping;
    wide_loop.loop_start = 23'423U;
    wide_loop.loop_length = 4'294'967'293U;
    volume.waveforms.push_back({"wide", "Wide", "SMPL/Wide.wav", wide_loop});
    volume.sample_banks.push_back({"sample-bank", "Bank", {"known-sample"}, {"known-sample", "likely-sample"}});
    volume.programs.push_back({"source", "001", {"target"}});
    graph.relationships.push_back({
        .key = "program-edge",
        .source_key = "source",
        .target_key = "target",
        .candidate_keys = {},
        .type = "PROG_ASSIGNMENT_TO_SBAC",
        .quality = axk::RelationshipQuality::known,
        .basis = "assignment-kind-0x11+name",
        .notes = "",
        .scope_key = "scope",
        .assignment_index = 0U,
        .assignment_name = "Bank",
        .assignment_state = axk::AssignmentState::source_load_assignment,
        .receive_selector = std::nullopt,
        .receive_channel_display = "unknown",
    });
    const auto volume_graph = export_schema::serialize_volume_graph(volume, graph, "source.iso", "iso");
    ASSERT_TRUE(volume_graph);
    const auto parsed_graph = nlohmann::json::parse(*volume_graph);
    EXPECT_EQ(parsed_graph["source"]["container_kinds"][0], "iso");
    ASSERT_EQ(parsed_graph["objects"]["smpl"][0]["user_facing_aliases"].size(), 1U);
    EXPECT_EQ(parsed_graph["objects"]["smpl"][0]["user_facing_aliases"][0]["display_name"], "Sampler-visible Sample");
    EXPECT_EQ(parsed_graph["objects"]["smpl"][0]["user_facing_aliases"][0]["relationship_quality"], "Likely");
    EXPECT_TRUE(parsed_graph["objects"]["smpl"][1]["user_facing_aliases"].empty());
    EXPECT_TRUE(parsed_graph["objects"]["smpl"][0]["playback"]["loop_end_frame_a4000_ui"].is_null());
    EXPECT_EQ(parsed_graph["objects"]["smpl"][1]["playback"]["loop_end_frame_a4000_ui"], 4'294'990'716ULL);
    EXPECT_TRUE(std::ranges::any_of(parsed_graph["relationships"], [](const auto &row) {
        return row["relationship_type"] == "PROG_ASSIGNMENT_TO_SBAC";
    }));
    EXPECT_EQ(parsed_graph["objects"]["sbac"][0]["members"].size(), 1U);
    EXPECT_EQ(parsed_graph["objects"]["sbac"][0]["relationship_sample_keys"].size(), 2U);
}

TEST(CliSchema, InfoV1KeepsRecursiveOrderNullCountsAndUtf8Paths) {
    info_schema::InfoOutput output;
    info_schema::NodeOutput root{
        .node_id = "root",
        .node_type = "sample_bank",
        .display_name = "B \xc3\xa4",
        .object_key = "",
        .object_type = "SBAC",
        .count = std::nullopt,
        .details = {},
        .quality = "Known",
        .basis = "test",
        .notes = "",
        .selector_path = "B \xc3\xa4",
        .children = {},
    };
    output.trees.push_back({
        .source_path_utf8 = "audio/\xc3\xa4.hds",
        .container_kind = "sfs",
        .detected_format = "sfs",
        .roots = {std::move(root)},
        .issues = {{.code = "TEST",
                    .severity = "warning",
                    .message = "message",
                    .source_path_utf8 = "audio/\xc3\xa4.hds",
                    .sampler_path = "Volume/Bank",
                    .object_key = "p0:sfs1"}},
    });
    output.load_errors.push_back({
        .path_utf8 = "bad.hds",
        .error_code = 100U,
        .message = "missing",
        .original_exception = "axk::Error",
    });

    const auto serialized = info_schema::serialize(output);
    ASSERT_TRUE(serialized);
    const auto parsed = nlohmann::json::parse(*serialized);
    EXPECT_TRUE(parsed["trees"][0]["roots"][0]["count"].is_null());
    EXPECT_EQ(parsed["trees"][0]["issues"][0]["code"], "TEST");
    EXPECT_EQ(parsed["trees"][0]["issues"][0]["object_key"], "p0:sfs1");
    EXPECT_EQ(parsed["load_errors"][0]["error_code"], 100U);
}

TEST(CliSchema, SequenceTempoSeparatesHeaderEffectiveValueAndTimelineMap) {
    axk::CurrentSequence sequence;
    sequence.header_tempo_bpm = 130U;
    sequence.effective_initial_tempo_microseconds_per_quarter_note = 460'122U;
    sequence.tempo_events = {{.tick = 0U, .microseconds_per_quarter_note = 460'122U},
                             {.tick = 384U, .microseconds_per_quarter_note = 400'000U}};

    object_schema::ObjectOutput object;
    object.header.raw_type = "SEQU";
    object.decoded.payload = std::move(sequence);
    object_schema::ObjectsOutput output{
        .shape = object_schema::ContainerShape::media, .container_kind = "object", .objects = {std::move(object)}};

    const auto serialized = object_schema::serialize(output, false);
    ASSERT_TRUE(serialized) << serialized.error().message;
    const auto decoded = nlohmann::json::parse(*serialized)["objects"][0]["decoded"];
    EXPECT_EQ(decoded["header_tempo_bpm"], 130U);
    EXPECT_EQ(decoded["effective_initial_tempo_microseconds_per_quarter_note"], 460'122U);
    ASSERT_EQ(decoded["tempo_events"].size(), 2U);
    EXPECT_EQ(decoded["tempo_events"][1]["tick"], 384U);
    EXPECT_EQ(decoded["tempo_events"][1]["microseconds_per_quarter_note"], 400'000U);
    EXPECT_FALSE(decoded.contains("tempo_bpm"));
}

TEST(CliSchema, AlterationV1DistinguishesNullEmptyAndPresentValues) {
    schema::AlterationOutput output{
        .source_path_utf8 = "source/\xc3\xa4.hds",
        .output_path_utf8 = std::nullopt,
        .applied = false,
        .operations = {},
    };
    output.operations.push_back({
        .id = "op",
        .type = "delete_volume",
        .partition_index = 7,
        .volume_name = "",
        .object_name = "Object",
        .removed_sfs_ids = {},
        .inserted_sfs_ids = {4'294'967'295U},
        .placed_sfs_ids = {65'535U},
        .freed_clusters = std::numeric_limits<std::uint64_t>::max(),
        .allocated_clusters = 0,
        .audio_import = std::nullopt,
    });

    const auto serialized = schema::serialize(output, false);
    ASSERT_TRUE(serialized);
    const auto parsed = nlohmann::json::parse(*serialized);
    EXPECT_TRUE(parsed["output_path"].is_null());
    EXPECT_EQ(parsed["operations"][0]["volume_name"], "");
    EXPECT_TRUE(parsed["operations"][0]["removed_sfs_ids"].empty());
    EXPECT_EQ(parsed["operations"][0]["placed_sfs_ids"], nlohmann::json::array({65'535U}));
    EXPECT_TRUE(parsed["operations"][0]["audio_import"].is_null());
    EXPECT_EQ(parsed["operations"][0]["freed_clusters"], std::numeric_limits<std::uint64_t>::max());
}

TEST(CliSchema, SerializationRejectsInvalidInternalUtf8) {
    schema::AlterationOutput output{
        .source_path_utf8 = std::string{"\xc3\x28", 2U},
        .output_path_utf8 = std::nullopt,
        .applied = false,
        .operations = {},
    };
    const auto serialized = schema::serialize(output, false);
    EXPECT_FALSE(serialized);
}

TEST(CliSchema, PackageV1PreservesTypedKindsNullsAndUnsignedCounts) {
    EXPECT_EQ(package_schema::schema_version, "1.0");
    package_schema::PackageOutput package{
        .path_utf8 = "portable/Sample.axksbnk",
        .package_id = "digest",
        .package_kind = "sbnk",
        .required_extension = ".axksbnk",
        .source_media_kind = "sfs",
        .valid = true,
        .payloads_verified = false,
        .total_payload_bytes = std::numeric_limits<std::uint64_t>::max() - 1U,
        .relationship_count = std::numeric_limits<std::uint64_t>::max(),
        .roots = {{"sbnk", "Sample", {"node"}}},
        .objects = {{"node", "SBNK", "Sample", std::numeric_limits<std::uint64_t>::max() - 1U, "payload", "normalized",
                     std::nullopt, std::nullopt}},
        .issues = {},
    };
    const auto serialized_package = package_schema::serialize(package, false);
    ASSERT_TRUE(serialized_package);
    const auto package_json = nlohmann::json::parse(*serialized_package);
    EXPECT_EQ(package_json["package_kind"], "sbnk");
    EXPECT_EQ(package_json["required_extension"], ".axksbnk");
    EXPECT_FALSE(package_json["payloads_verified"]);
    EXPECT_EQ(package_json["total_payload_bytes"], std::numeric_limits<std::uint64_t>::max() - 1U);
    EXPECT_EQ(package_json["objects"][0]["payload_size_bytes"], std::numeric_limits<std::uint64_t>::max() - 1U);
    EXPECT_TRUE(package_json["objects"][0]["semantic_sha256"].is_null());
    EXPECT_TRUE(package_json["objects"][0]["audio_sha256"].is_null());
    EXPECT_EQ(package_json["relationship_count"], std::numeric_limits<std::uint64_t>::max());

    package_schema::PlanOutput plan{
        .target_path_utf8 = "target.hds",
        .package_paths_utf8 = {"portable/Sample.axksbnk"},
        .plan_id = "plan",
        .target_kind = "sfs",
        .target_snapshot_id = "snapshot",
        .valid = true,
        .warnings = {{"TARGET_SEQUENCE_PRESERVED_OPAQUE", "existing Sequence is preserved", "target", std::nullopt, "",
                      "SEQU", "Opaque Sequence", 0U, "Existing"}},
        .opaque_sequences = {{0U, "sequence-1", "Opaque Sequence", "preserve-unchanged"}},
        .conflicts = {{"TEST", "conflict", std::nullopt, std::nullopt, "digest", "node", std::nullopt, "", "Volume", "",
                       ""}},
        .objects = {{"action",
                     0U,
                     0U,
                     "digest",
                     "node",
                     "SBNK",
                     "Sample",
                     "Sample",
                     0U,
                     "",
                     "Volume",
                     "",
                     "",
                     {"insert"},
                     std::nullopt,
                     std::nullopt,
                     std::nullopt}},
        .program_assignment_adjustments = {{"adjustment", "existing-program", std::nullopt, std::nullopt, "PROG:001",
                                            "001", "Program One", 2U, "SBAC", "Bank", 0U, "", "Volume", "", "",
                                            "UNRESOLVED_PROGRAM_ASSIGNMENT_COLLISION", "clear-assignment"}},
        .program_slot_placements = {{"placement",
                                     0U,
                                     "Volume",
                                     "contiguous",
                                     false,
                                     5U,
                                     4U,
                                     124U,
                                     {{1U, 4U}},
                                     {{1U, 4U}},
                                     {{5U, 8U}},
                                     {{0U, "node", 1U, 5U, true}}}},
        .allocation = {{0U, "Group", "Volume", "Raw Group", "Raw Volume", 1U,  2U,  3U,  4U,  5U,
                        6U, 7U,      8U,       9U,          10U,          11U, 12U, 13U, 14U, 15U}},
        .result = std::nullopt,
    };
    const auto serialized_plan = package_schema::serialize(plan, false);
    ASSERT_TRUE(serialized_plan);
    const auto plan_json = nlohmann::json::parse(*serialized_plan);
    EXPECT_EQ(plan_json["warnings"][0]["origin"], "target");
    EXPECT_TRUE(plan_json["warnings"][0]["package_index"].is_null());
    EXPECT_EQ(plan_json["warnings"][0]["object_name"], "Opaque Sequence");
    EXPECT_EQ(plan_json["opaque_sequences"][0]["action"], "preserve-unchanged");
    EXPECT_TRUE(plan_json["conflicts"][0]["package_index"].is_null());
    EXPECT_TRUE(plan_json["conflicts"][0]["partition_index"].is_null());
    EXPECT_TRUE(plan_json["objects"][0]["canonical_action_id"].is_null());
    EXPECT_TRUE(plan_json["objects"][0]["target_sfs_id"].is_null());
    EXPECT_EQ(plan_json["program_assignment_adjustments"][0]["origin"], "existing-program");
    EXPECT_TRUE(plan_json["program_assignment_adjustments"][0]["package_index"].is_null());
    EXPECT_EQ(plan_json["program_assignment_adjustments"][0]["assignment_ordinal"], 2U);
    EXPECT_EQ(plan_json["program_assignment_adjustments"][0]["disposition"], "clear-assignment");
    EXPECT_EQ(plan_json["program_slot_placements"][0]["mode"], "contiguous");
    EXPECT_EQ(plan_json["program_slot_placements"][0]["suggested_start_slot"], 5U);
    EXPECT_EQ(plan_json["program_slot_placements"][0]["destination_ranges"][0]["last"], 8U);
    EXPECT_TRUE(plan_json["program_slot_placements"][0]["mappings"][0]["requires_user_action"]);
    EXPECT_EQ(plan_json["allocation"][0]["blocked_object_count"], 3U);
    EXPECT_EQ(plan_json["allocation"][0]["directory_growth_clusters"], 8U);
    EXPECT_EQ(plan_json["allocation"][0]["directory_continuation_clusters"], 9U);
    EXPECT_EQ(plan_json["allocation"][0]["infrastructure_clusters"], 10U);
    EXPECT_EQ(plan_json["allocation"][0]["additional_allocated_bytes"], 11U);
    EXPECT_TRUE(plan_json["result"].is_null());
}
