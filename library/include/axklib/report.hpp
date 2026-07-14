#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "axklib/error.hpp"
#include "axklib/export.hpp"

namespace axk {

struct ReportValue {
    using Array = std::vector<ReportValue>;
    using Object = std::vector<std::pair<std::string, ReportValue>>;
    using Storage = std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, Array, Object>;

    Storage value;

    ReportValue() = default;
    ReportValue(std::nullptr_t) noexcept;
    ReportValue(bool item) noexcept;
    ReportValue(std::int64_t item) noexcept;
    ReportValue(std::uint64_t item) noexcept;
    ReportValue(double item) noexcept;
    ReportValue(std::string item) noexcept;
    ReportValue(const char *item);
    ReportValue(Array item) noexcept;
    ReportValue(Object item) noexcept;
};

using ReportRow = std::vector<std::pair<std::string, ReportValue>>;

struct ReportColumnSchema {
    std::string name;
    std::string type;
    bool required{};
    bool nullable{};
    std::string semantic_notes;
    std::string deprecation_notes;
};

struct ReportSchemaManifest {
    std::string report_name;
    std::string schema_version{"1.0"};
    std::size_t row_count{};
    std::vector<ReportColumnSchema> columns;
    std::vector<std::pair<std::string, std::uint64_t>> quality_counts;
    std::vector<std::pair<std::string, std::uint64_t>> issue_code_counts;
    std::vector<std::pair<std::string, std::uint64_t>> object_type_counts;
    std::vector<std::string> quality_columns;
    std::vector<std::string> issue_code_columns;
    std::vector<std::string> object_ref_columns;
    std::string source_command;
    std::string library_version;
    std::string semantic_notes;
    std::string replacement_notes;
};

struct ReportSchemaOptions {
    std::string source_command;
    std::string library_version;
    std::string semantic_notes;
    std::string replacement_notes;
};

AXK_API ReportSchemaManifest make_report_schema(std::string report_name, std::span<const ReportRow> rows,
                                                ReportSchemaOptions options = {});
AXK_API Result<void> write_report_json(const std::filesystem::path &path, std::span<const ReportRow> rows,
                                       bool overwrite = false);
AXK_API Result<void> write_report_object(const std::filesystem::path &path, const ReportRow &row,
                                         bool overwrite = false);
AXK_API Result<void> write_report_csv(const std::filesystem::path &path, std::span<const ReportRow> rows,
                                      std::span<const std::string> empty_columns = {}, bool overwrite = false);
AXK_API Result<void> write_report_schema(const std::filesystem::path &path, const ReportSchemaManifest &manifest,
                                         bool overwrite = false);
AXK_API Result<void> write_report_schema_index(const std::filesystem::path &path,
                                               std::span<const ReportSchemaManifest> manifests, bool overwrite = false);

} // namespace axk
