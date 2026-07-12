#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "schema/export_v1.hpp"
#include "schema/info_v1.hpp"
#include "schema/objects_v1.hpp"
#include "schema/operations_v1.hpp"
#include "schema/semantic_v1.hpp"

namespace schema = axk::cli::schema::operations_v1;
namespace info_schema = axk::cli::schema::info_v1;
namespace export_schema = axk::cli::schema::export_v1;
namespace object_schema = axk::cli::schema::objects_v1;
namespace semantic_schema = axk::cli::schema::semantic_v1;

TEST(CliSchema, ObjectRelationshipTreeAndExportSummarySchemasStayParseable) {
  EXPECT_EQ(info_schema::schema_version, "compat-v1");
  EXPECT_EQ(schema::alteration_schema_version, "compat-v1");
  EXPECT_EQ(schema::preview_schema_version, "1.0");
  EXPECT_EQ(object_schema::schema_version, "1.0");
  EXPECT_EQ(semantic_schema::schema_version, "1.0");
  EXPECT_EQ(export_schema::schema_version, "1.0");
  EXPECT_EQ(export_schema::volume_graph_schema_version, "axklib.volume_graph.v1");
  object_schema::ObjectsOutput objects{
      .shape = object_schema::ContainerShape::sfs,
      .container_kind = {},
      .objects = {},
  };
  object_schema::ObjectOutput object;
  object.partition_index = 7U;
  object.sfs_id = 4'294'967'295U;
  object.header.raw_type = "TEST";
  object.decoded.header = object.header;
  objects.objects.push_back(std::move(object));
  const auto object_json = object_schema::serialize(objects, false);
  ASSERT_TRUE(object_json);
  EXPECT_EQ(nlohmann::json::parse(*object_json)["objects"][0]["sfs_id"], 4'294'967'295U);

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
      .assignment_state = axk::AssignmentState::active,
      .receive_channel_display = "off",
  });
  const auto relationships =
      semantic_schema::serialize(semantic_schema::project_relationships(graph), false);
  ASSERT_TRUE(relationships);
  EXPECT_TRUE(nlohmann::json::parse(*relationships)["relationships"][0]["target_key"].is_null());

  const std::vector summaries{export_schema::VolumeSummaryOutput{
      .path_utf8 = "partition/\xc3\xa4",
      .graph_path_utf8 = "partition/\xc3\xa4/volume.axklib.json",
      .waveform_count = 1U,
      .sample_bank_count = 2U,
  }};
  const auto summary = export_schema::serialize(summaries, false);
  ASSERT_TRUE(summary);
  EXPECT_EQ(nlohmann::json::parse(*summary)["volumes"][0]["sample_bank_count"], 2U);
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
  EXPECT_EQ(parsed["trees"][0]["issues"], nlohmann::json::array());
  EXPECT_EQ(parsed["load_errors"][0]["error_code"], 100U);
}

TEST(CliSchema, PreviewV1HasStableCompactAndPrettyOutput) {
  const schema::PreviewOutput output{
      .object_key = "wave \"\xc3\xa4\"",
      .frame_count = std::numeric_limits<std::uint64_t>::max(),
      .bins = {{-32'768, 32'767}},
  };
  const auto compact = schema::serialize(output, false);
  ASSERT_TRUE(compact);
  EXPECT_EQ(*compact, "{\"schema_version\":\"1.0\",\"object_key\":\"wave \\\"\xc3\xa4\\\"\","
                      "\"frame_count\":18446744073709551615,\"bins\":[[-32768,32767]]}");
  EXPECT_NO_THROW([[maybe_unused]] const auto parsed = nlohmann::json::parse(*compact));

  const auto pretty = schema::serialize(output, true);
  ASSERT_TRUE(pretty);
  EXPECT_EQ(pretty->substr(0U, 28U), "{\n  \"schema_version\": \"1.0\",");
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
  EXPECT_TRUE(parsed["operations"][0]["audio_import"].is_null());
  EXPECT_EQ(parsed["operations"][0]["freed_clusters"], std::numeric_limits<std::uint64_t>::max());
}

TEST(CliSchema, SerializationRejectsInvalidInternalUtf8) {
  schema::PreviewOutput output{
      .object_key = std::string{"\xc3\x28", 2U}, .frame_count = 0U, .bins = {}};
  const auto serialized = schema::serialize(output, false);
  EXPECT_FALSE(serialized);
}
