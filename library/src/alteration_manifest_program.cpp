#include "alteration_manifest_program.hpp"

#include <algorithm>
#include <ranges>
#include <string>

#include <nlohmann/json.hpp>

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

Result<ProgramAssignmentSpec> assignment(const Json &row, std::string_view context) {
    if (!row.is_object() || !row.contains("receive_mode") || !row["receive_mode"].is_string())
        return std::unexpected{invalid(std::string{context} + " has invalid fields")};
    const auto sample_target = row.contains("sample");
    const auto bank_target = row.contains("sample_bank");
    if (sample_target == bank_target)
        return std::unexpected{invalid(std::string{context} + " must contain exactly one target")};
    const auto field = sample_target ? "sample" : "sample_bank";
    const auto mode = row["receive_mode"].get<std::string>();
    const auto midi_mode = mode == "MIDI_CHANNEL";
    if (!midi_mode && mode != "SAMPLE")
        return std::unexpected{invalid(std::string{context} + ".receive_mode is invalid")};
    if (row.size() != (midi_mode ? 3U : 2U) || row.contains("receive_channel") != midi_mode)
        return std::unexpected{invalid(std::string{context} + " has invalid fields for its receive mode")};
    auto target = text(row, field, 16U, context);
    if (!target)
        return std::unexpected{target.error()};
    std::uint8_t channel{};
    if (midi_mode) {
        if (!row["receive_channel"].is_number_integer())
            return std::unexpected{invalid(std::string{context} + ".receive_channel must be an integer")};
        const auto value = row["receive_channel"].get<int>();
        if (value < 1 || value > 16)
            return std::unexpected{invalid(std::string{context} + ".receive_channel must be between 1 and 16")};
        channel = static_cast<std::uint8_t>(value);
    }
    return ProgramAssignmentSpec{sample_target ? "SBNK" : "SBAC", std::move(*target), channel,
                                 midi_mode ? ProgramReceiveMode::midi_channel : ProgramReceiveMode::sample};
}

} // namespace

Result<InsertProgramOperation> parse_insert_program_json(const Json &row, PartitionSelector selector,
                                                         std::string_view context) {
    if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name", "program"}, context); !valid)
        return std::unexpected{valid.error()};
    auto volume = text(row, "volume_name", 16U, context);
    if (!volume)
        return std::unexpected{volume.error()};
    const auto &program = row["program"];
    const auto program_context = std::string{context} + ".program";
    if (auto valid = exact_fields(program, {"number", "name", "assignments"}, program_context); !valid)
        return std::unexpected{valid.error()};
    auto number = program_number(program, program_context);
    auto name = text(program, "name", 8U, program_context);
    if (!number)
        return std::unexpected{number.error()};
    if (!name)
        return std::unexpected{name.error()};
    if (!program["assignments"].is_array())
        return std::unexpected{invalid(program_context + ".assignments must be an array")};
    ProgramSpec spec{*number, std::move(*name), {}};
    for (std::size_t index = 0; index < program["assignments"].size(); ++index) {
        auto parsed = assignment(program["assignments"][index], program_context + ".assignments");
        if (!parsed)
            return std::unexpected{parsed.error()};
        spec.assignments.push_back(std::move(*parsed));
    }
    return InsertProgramOperation{std::move(selector), std::move(*volume), std::move(spec)};
}

} // namespace axk::detail
