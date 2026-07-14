#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "axklib/report.hpp"

namespace {

std::string read(const std::filesystem::path &path) {
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

} // namespace

TEST(ReportSchema, MatchesStableColumnCountAndClassificationRules) {
    const std::vector<axk::ReportRow> rows{
        {{"object_type", "SMPL"},
         {"quality", "Known"},
         {"code", "OK"},
         {"raw_offset", std::uint64_t{12}},
         {"optional", nullptr}},
        {{"object_type", "SBNK"},
         {"quality", "Likely"},
         {"code", "WARN"},
         {"raw_offset", nullptr}},
    };
    axk::ReportSchemaOptions options;
    options.source_command = "axklib inventory";
    options.library_version = "test";
    const auto schema =
        axk::make_report_schema("example", rows, std::move(options));
    EXPECT_EQ(schema.row_count, 2U);
    EXPECT_EQ(schema.quality_counts,
              (std::vector<std::pair<std::string, std::uint64_t>>{
                  {"Known", 1}, {"Likely", 1}}));
    EXPECT_EQ(schema.issue_code_counts,
              (std::vector<std::pair<std::string, std::uint64_t>>{
                  {"OK", 1}, {"WARN", 1}}));
    EXPECT_EQ(schema.object_type_counts,
              (std::vector<std::pair<std::string, std::uint64_t>>{
                  {"SBNK", 1}, {"SMPL", 1}}));
    const auto raw = std::ranges::find(schema.columns, "raw_offset",
                                       &axk::ReportColumnSchema::name);
    ASSERT_NE(raw, schema.columns.end());
    EXPECT_EQ(raw->type, "integer");
    EXPECT_TRUE(raw->nullable);
    EXPECT_FALSE(raw->semantic_notes.empty());
}

TEST(ReportWriter,
     PreservesJsonOrderAndPythonCsvEscapingWithoutPartialOverwrite) {
    const std::vector<axk::ReportRow> rows{
        {{"name", "A, \"quoted\""},
         {"count", std::int64_t{-2}},
         {"active", true}},
        {{"name", "line\nbreak"},
         {"count", std::uint64_t{3}},
         {"active", false}},
    };
    const auto root =
        std::filesystem::temp_directory_path() / "axklib-report-test";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_TRUE(axk::write_report_json(root / "rows.json", rows));
    ASSERT_TRUE(axk::write_report_csv(root / "rows.csv", rows));
    EXPECT_EQ(read(root / "rows.csv"),
              "name,count,active\r\n\"A, "
              "\"\"quoted\"\"\",-2,True\r\n\"line\nbreak\",3,False\r\n");
    const auto json = read(root / "rows.json");
    EXPECT_LT(json.find("\"name\""), json.find("\"count\""));
    const auto conflict = axk::write_report_json(root / "rows.json", rows);
    ASSERT_FALSE(conflict);
    EXPECT_EQ(conflict.error().code, axk::ErrorCode::io_open_failed);
    std::filesystem::remove_all(root, error);
}

TEST(ReportWriter, ContainsJsonExceptionsAndPublishesNothingForInvalidUtf8) {
    const auto root =
        std::filesystem::temp_directory_path() / "axklib-report-invalid-utf8";
    const auto output = root / "rows.json";
    std::error_code error;
    std::filesystem::remove_all(root, error);
    const std::string invalid{"\xc3\x28", 2U};
    const std::vector<axk::ReportRow> rows{{{"name", invalid}}};
    const auto written = axk::write_report_json(output, rows);
    ASSERT_FALSE(written);
    EXPECT_EQ(written.error().code, axk::ErrorCode::internal_invariant);
    EXPECT_FALSE(std::filesystem::exists(output));
    std::filesystem::remove_all(root, error);
}
