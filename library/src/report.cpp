#include "axklib/report.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <ranges>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "axklib/utf8.hpp"

namespace axk {
namespace {

using OrderedJson = nlohmann::ordered_json;

Error serialization_error(const nlohmann::json::exception &error) {
    return make_error(ErrorCode::internal_invariant, ErrorCategory::internal,
                      "report JSON serialization failed: " + std::string{error.what()});
}

constexpr std::array quality_names{"quality", "extraction_quality", "match_quality",
                                   "organization_relationship_quality"};
constexpr std::array issue_names{"code", "issue_code", "decode_issue_codes"};
constexpr std::array object_ref_names{
    "object_key", "source_key",    "target_key",        "partition_index", "sfs_id",           "payload_offset",
    "raw_offset", "object_offset", "object_offset_hex", "source_path",     "source_container", "image"};

template <std::size_t Size> bool contains(const std::array<const char *, Size> &values, std::string_view value) {
    return std::ranges::find(values, value) != values.end();
}

std::string type_name(const ReportValue &value) {
    if (const auto *item = std::get_if<std::string>(&value.value); item != nullptr && item->empty())
        return "null";
    switch (value.value.index()) {
    case 0:
        return "null";
    case 1:
        return "boolean";
    case 2:
    case 3:
        return "integer";
    case 4:
        return "number";
    case 5:
        return "string";
    case 6:
        return "array";
    case 7:
        return "object";
    default:
        return "null";
    }
}

std::string combined_type(const std::set<std::string> &values) {
    auto concrete = values;
    concrete.erase("null");
    if (concrete.empty())
        return "null";
    if (concrete == std::set<std::string>{"integer", "number"})
        return "number";
    if (concrete.size() == 1U)
        return *concrete.begin();
    return "mixed";
}

const ReportValue *find(const ReportRow &row, std::string_view name) {
    const auto found = std::ranges::find(row, name, &std::pair<std::string, ReportValue>::first);
    return found == row.end() ? nullptr : &found->second;
}

std::string scalar_text(const ReportValue &value) {
    if (std::holds_alternative<std::monostate>(value.value))
        return {};
    if (const auto *item = std::get_if<bool>(&value.value))
        return *item ? "True" : "False";
    if (const auto *item = std::get_if<std::int64_t>(&value.value))
        return std::to_string(*item);
    if (const auto *item = std::get_if<std::uint64_t>(&value.value))
        return std::to_string(*item);
    if (const auto *item = std::get_if<double>(&value.value)) {
        std::array<char, 64> buffer{};
        const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), *item);
        return result.ec == std::errc{} ? std::string{buffer.data(), result.ptr} : std::string{};
    }
    if (const auto *item = std::get_if<std::string>(&value.value))
        return *item;
    return {};
}

OrderedJson json_value(const ReportValue &value) {
    if (std::holds_alternative<std::monostate>(value.value))
        return nullptr;
    if (const auto *item = std::get_if<bool>(&value.value))
        return *item;
    if (const auto *item = std::get_if<std::int64_t>(&value.value))
        return *item;
    if (const auto *item = std::get_if<std::uint64_t>(&value.value))
        return *item;
    if (const auto *item = std::get_if<double>(&value.value))
        return *item;
    if (const auto *item = std::get_if<std::string>(&value.value))
        return *item;
    if (const auto *item = std::get_if<ReportValue::Array>(&value.value)) {
        auto result = OrderedJson::array();
        for (const auto &child : *item)
            result.push_back(json_value(child));
        return result;
    }
    auto result = OrderedJson::object();
    for (const auto &[name, child] : std::get<ReportValue::Object>(value.value)) {
        result[name] = json_value(child);
    }
    return result;
}

OrderedJson json_row(const ReportRow &row) {
    auto result = OrderedJson::object();
    for (const auto &[name, value] : row)
        result[name] = json_value(value);
    return result;
}

std::string csv_field(std::string value) {
    if (value.find_first_of(",\"\r\n") == std::string::npos)
        return value;
    std::string escaped{"\""};
    for (const auto ch : value) {
        if (ch == '"')
            escaped.push_back('"');
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

Result<void> write_atomic(const std::filesystem::path &path, std::string_view text, bool overwrite) {
    std::error_code error;
    if (!overwrite && std::filesystem::exists(path, error)) {
        return std::unexpected{make_error(ErrorCode::io_open_failed, ErrorCategory::io,
                                          "refusing to replace existing report: " + text::path_to_utf8(path))};
    }
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not create report output directory")};
    }
    const auto temporary = text::temporary_sibling(path);
    if (!temporary)
        return std::unexpected{temporary.error()};
    {
        std::ofstream output{*temporary, std::ios::binary | std::ios::trunc};
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!output) {
            std::filesystem::remove(*temporary, error);
            return std::unexpected{
                make_error(ErrorCode::io_read_failed, ErrorCategory::io, "could not write temporary report")};
        }
    }
    if (overwrite)
        std::filesystem::remove(path, error);
    std::filesystem::rename(*temporary, path, error);
    if (error) {
        std::filesystem::remove(*temporary, error);
        return std::unexpected{
            make_error(ErrorCode::io_open_failed, ErrorCategory::io, "could not publish report atomically")};
    }
    return {};
}

std::string semantic_notes(std::string_view name) {
    if (contains(quality_names, name))
        return "DataQuality marker; exact allowed values are part of the "
               "quality model, not a "
               "free-form status string.";
    if (name == "assignment_row_state")
        return "Program assignment row classification; decoded-row means a "
               "PROG row was decoded "
               "and reported separately from active assignment state.";
    if (name == "active_assignment_state")
        return "Conservative Program assignment classification. "
               "HDA/sampler-authored rows may be "
               "confirmed-active, confirmed-visible-off, or "
               "confirmed-duplicate-not-active; ISO "
               "source-load-assignment rows are matched source links whose "
               "loaded active state is "
               "reported separately from stored HDA active/off bytes.";
    if (name == "assignment_output1_byte_0x1d")
        return "Decoded PROG assignment row +0x1d byte retained as raw per-row "
               "output data; it is "
               "not the Rch Assign display selector by itself.";
    if (name == "assignment_rch_assign_gate_byte_0x28")
        return "Decoded PROG assignment row byte used for active/off "
               "classification where 0xff is "
               "active and 0x00 is visible/off.";
    if (name == "assignment_rch_assign_display")
        return "Conservative visible Rch Assign family: off, =SMP, 01 through "
               "16, BasicRch, B01 "
               "through B16, or unknown.";
    if (name == "basis" || name == "extraction_basis" || name == "notes" || name == "match_notes")
        return "Quality/basis origin field. Do not treat as decoded raw "
               "storage by itself.";
    if (name.starts_with("raw_") || name.ends_with("_offset") || name.ends_with("_offset_hex"))
        return "Raw image/object reference for auditability.";
    if (name == "source_path" || name == "source_container" || name == "image")
        return "Input source path used to produce this row.";
    if (contains(issue_names, name))
        return "Stable diagnostic/validation code where applicable.";
    return {};
}

template <typename Map> std::vector<std::pair<std::string, std::uint64_t>> pairs(const Map &values) {
    return {values.begin(), values.end()};
}

OrderedJson counts_json(const std::vector<std::pair<std::string, std::uint64_t>> &values) {
    auto result = OrderedJson::object();
    for (const auto &[name, count] : values)
        result[name] = count;
    return result;
}

OrderedJson schema_json(const ReportSchemaManifest &manifest) {
    auto columns = OrderedJson::array();
    for (const auto &column : manifest.columns) {
        columns.push_back({{"name", column.name},
                           {"type", column.type},
                           {"required", column.required},
                           {"nullable", column.nullable},
                           {"semantic_notes", column.semantic_notes},
                           {"deprecation_notes", column.deprecation_notes}});
    }
    return {{"report_name", manifest.report_name},
            {"schema_version", manifest.schema_version},
            {"row_count", manifest.row_count},
            {"columns", std::move(columns)},
            {"quality_counts", counts_json(manifest.quality_counts)},
            {"issue_code_counts", counts_json(manifest.issue_code_counts)},
            {"object_type_counts", counts_json(manifest.object_type_counts)},
            {"quality_columns", manifest.quality_columns},
            {"issue_code_columns", manifest.issue_code_columns},
            {"object_ref_columns", manifest.object_ref_columns},
            {"source_command", manifest.source_command},
            {"library_version", manifest.library_version},
            {"semantic_notes", manifest.semantic_notes},
            {"replacement_notes", manifest.replacement_notes}};
}

} // namespace

ReportValue::ReportValue(std::nullptr_t) noexcept : value{std::monostate{}} {}
ReportValue::ReportValue(bool item) noexcept : value{item} {}
ReportValue::ReportValue(std::int64_t item) noexcept : value{item} {}
ReportValue::ReportValue(std::uint64_t item) noexcept : value{item} {}
ReportValue::ReportValue(double item) noexcept : value{item} {}
ReportValue::ReportValue(std::string item) noexcept : value{std::move(item)} {}
ReportValue::ReportValue(const char *item) : value{std::string{item}} {}
ReportValue::ReportValue(Array item) noexcept : value{std::move(item)} {}
ReportValue::ReportValue(Object item) noexcept : value{std::move(item)} {}

ReportSchemaManifest make_report_schema(std::string report_name, std::span<const ReportRow> rows,
                                        ReportSchemaOptions options) {
    ReportSchemaManifest result;
    result.report_name = std::move(report_name);
    result.row_count = rows.size();
    result.source_command = std::move(options.source_command);
    result.library_version = std::move(options.library_version);
    result.semantic_notes = std::move(options.semantic_notes);
    result.replacement_notes = std::move(options.replacement_notes);
    std::set<std::string> names;
    for (const auto &row : rows) {
        for (const auto &[name, value] : row) {
            static_cast<void>(value);
            names.insert(name);
        }
    }
    std::map<std::string, std::uint64_t> quality_counts;
    std::map<std::string, std::uint64_t> issue_counts;
    std::map<std::string, std::uint64_t> type_counts;
    for (const auto &name : names) {
        std::size_t present{};
        std::set<std::string> types;
        for (const auto &row : rows) {
            if (const auto *value = find(row, name)) {
                ++present;
                types.insert(type_name(*value));
            }
        }
        result.columns.push_back({name,
                                  combined_type(types),
                                  !rows.empty() && present == rows.size(),
                                  types.contains("null") || present < rows.size(),
                                  semantic_notes(name),
                                  {}});
        if (contains(quality_names, name))
            result.quality_columns.push_back(name);
        if (contains(issue_names, name))
            result.issue_code_columns.push_back(name);
        if (contains(object_ref_names, name))
            result.object_ref_columns.push_back(name);
    }
    for (const auto &row : rows) {
        for (const auto name : quality_names) {
            if (const auto *value = find(row, name); value != nullptr && !scalar_text(*value).empty())
                ++quality_counts[scalar_text(*value)];
        }
        for (const auto name : std::array{"code", "issue_code"}) {
            if (const auto *value = find(row, name); value != nullptr && !scalar_text(*value).empty())
                ++issue_counts[scalar_text(*value)];
        }
        for (const auto name : std::array{"object_type", "type", "matched_target_type"}) {
            if (const auto *value = find(row, name); value != nullptr && !scalar_text(*value).empty()) {
                ++type_counts[scalar_text(*value)];
                break;
            }
        }
    }
    result.quality_counts = pairs(quality_counts);
    result.issue_code_counts = pairs(issue_counts);
    result.object_type_counts = pairs(type_counts);
    return result;
}

Result<void> write_report_json(const std::filesystem::path &path, std::span<const ReportRow> rows, bool overwrite) {
    try {
        auto value = OrderedJson::array();
        for (const auto &row : rows)
            value.push_back(json_row(row));
        return write_atomic(path, value.dump(2) + "\n", overwrite);
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{serialization_error(error)};
    }
}

Result<void> write_report_object(const std::filesystem::path &path, const ReportRow &row, bool overwrite) {
    try {
        return write_atomic(path, json_row(row).dump(2) + "\n", overwrite);
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{serialization_error(error)};
    }
}

Result<void> write_report_csv(const std::filesystem::path &path, std::span<const ReportRow> rows,
                              std::span<const std::string> empty_columns, bool overwrite) {
    std::vector<std::string> columns;
    if (!rows.empty()) {
        for (const auto &[name, value] : rows.front()) {
            static_cast<void>(value);
            columns.push_back(name);
        }
        std::set<std::string> seen{columns.begin(), columns.end()};
        for (const auto &row : rows) {
            for (const auto &[name, value] : row) {
                static_cast<void>(value);
                if (seen.insert(name).second)
                    columns.push_back(name);
            }
        }
    } else {
        columns.assign(empty_columns.begin(), empty_columns.end());
    }
    std::ostringstream output;
    if (!columns.empty()) {
        for (std::size_t index = 0; index < columns.size(); ++index) {
            if (index != 0U)
                output << ',';
            output << csv_field(columns[index]);
        }
        output << "\r\n";
        for (const auto &row : rows) {
            for (std::size_t index = 0; index < columns.size(); ++index) {
                if (index != 0U)
                    output << ',';
                const auto *value = find(row, columns[index]);
                if (value != nullptr)
                    output << csv_field(scalar_text(*value));
            }
            output << "\r\n";
        }
    }
    return write_atomic(path, output.str(), overwrite);
}

Result<void> write_report_schema(const std::filesystem::path &path, const ReportSchemaManifest &manifest,
                                 bool overwrite) {
    try {
        return write_atomic(path, schema_json(manifest).dump(2) + "\n", overwrite);
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{serialization_error(error)};
    }
}

Result<void> write_report_schema_index(const std::filesystem::path &path,
                                       std::span<const ReportSchemaManifest> manifests, bool overwrite) {
    try {
        auto reports = OrderedJson::array();
        for (const auto &manifest : manifests) {
            reports.push_back({{"report_name", manifest.report_name},
                               {"row_count", manifest.row_count},
                               {"column_count", manifest.columns.size()},
                               {"quality_counts", counts_json(manifest.quality_counts)},
                               {"issue_code_counts", counts_json(manifest.issue_code_counts)},
                               {"object_type_counts", counts_json(manifest.object_type_counts)},
                               {"quality_columns", manifest.quality_columns},
                               {"issue_code_columns", manifest.issue_code_columns},
                               {"object_ref_columns", manifest.object_ref_columns},
                               {"source_command", manifest.source_command},
                               {"library_version", manifest.library_version}});
        }
        const auto value =
            OrderedJson{{"schema_version", "1.0"}, {"report_count", manifests.size()}, {"reports", std::move(reports)}};
        return write_atomic(path, value.dump(2) + "\n", overwrite);
    } catch (const nlohmann::json::exception &error) {
        return std::unexpected{serialization_error(error)};
    }
}

} // namespace axk
