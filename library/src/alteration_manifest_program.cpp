#include "alteration_manifest_program.hpp"

#include <algorithm>
#include <ranges>
#include <string>

#include <nlohmann/json.hpp>

#include "axklib/object.hpp"
#include "axklib/program_spec_json.hpp"

namespace axk::detail {
namespace {

using Json = nlohmann::json;

Error invalid(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

Result<void> exact_fields(const Json &row, std::initializer_list<std::string_view> expected, std::string_view context) {
    if (!row.is_object() || row.size() != expected.size())
        return std::unexpected{invalid(std::string{context} + " has invalid fields")};
    for (const auto field : expected) {
        if (!row.contains(field))
            return std::unexpected{invalid(std::string{context} + " is missing field " + std::string{field})};
    }
    return {};
}

Result<std::string> text(const Json &row, std::string_view field, std::size_t maximum, std::string_view context) {
    if (!row.contains(field) || !row[field].is_string())
        return std::unexpected{invalid(std::string{context} + "." + std::string{field} + " must be a string")};
    auto value = row[field].get<std::string>();
    const auto printable =
        std::ranges::all_of(value, [](unsigned char character) { return character >= 0x20U && character <= 0x7eU; });
    if (value.empty() || value.size() > maximum || !printable || value.front() == ' ' || value.back() == ' ')
        return std::unexpected{invalid(std::string{context} + "." + std::string{field} + " is invalid")};
    return value;
}

Result<std::uint8_t> program_number(const Json &row, std::string_view context) {
    if (!row.contains("number") || !row["number"].is_number_integer())
        return std::unexpected{invalid(std::string{context} + ".number must be an integer")};
    const auto value = row["number"].get<int>();
    if (value < 1 || value > 128)
        return std::unexpected{invalid(std::string{context} + ".number must be between 1 and 128")};
    return static_cast<std::uint8_t>(value);
}

} // namespace

Result<InsertProgramOperation> parse_insert_program_json(const Json &row, PartitionSelector selector,
                                                         std::string_view context) {
    if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name", "program"}, context); !valid)
        return std::unexpected{valid.error()};
    auto volume = text(row, "volume_name", 16U, context);
    if (!volume)
        return std::unexpected{volume.error()};
    auto spec = parse_program_spec_json(row["program"]);
    if (!spec)
        return std::unexpected{invalid(std::string{context} + ".program: " + spec.error().message)};
    return InsertProgramOperation{std::move(selector), std::move(*volume), std::move(*spec)};
}

Result<ClearProgramAssignmentsOperation>
parse_clear_program_assignments_json(const Json &row, PartitionSelector selector, std::string_view context) {
    if (auto valid = exact_fields(
            row, {"id", "type", "partition_index", "volume_name", "program_number", "assignment_ordinals"}, context);
        !valid) {
        return std::unexpected{valid.error()};
    }
    auto volume = text(row, "volume_name", 16U, context);
    if (!volume)
        return std::unexpected{volume.error()};
    Json program{{"number", row["program_number"]}};
    auto number = program_number(program, context);
    if (!number)
        return std::unexpected{number.error()};
    if (!row["assignment_ordinals"].is_array())
        return std::unexpected{invalid(std::string{context} + ".assignment_ordinals must be an array")};
    std::vector<std::uint16_t> ordinals;
    ordinals.reserve(row["assignment_ordinals"].size());
    for (const auto &value : row["assignment_ordinals"]) {
        if (!value.is_number_integer()) {
            return std::unexpected{invalid(std::string{context} + ".assignment_ordinals entries must be integers")};
        }
        if (value < 0 || value >= maximum_stored_program_assignments) {
            return std::unexpected{
                invalid(std::string{context} + ".assignment_ordinals entries must be between 0 and 998")};
        }
        ordinals.push_back(value.get<std::uint16_t>());
    }
    return ClearProgramAssignmentsOperation{std::move(selector), std::move(*volume), *number, std::move(ordinals)};
}

} // namespace axk::detail
