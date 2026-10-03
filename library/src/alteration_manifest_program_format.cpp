#include "alteration_manifest_program_format.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace axk::detail {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest, std::move(message));
}
} // namespace

Result<void> validate_program_format_conversion(const ConvertProgramFormatOperation &operation) {
    if (operation.target_format != ProgramStorageFormat::a3000 &&
        operation.target_format != ProgramStorageFormat::a4000_a5000)
        return std::unexpected{invalid("target_format must be a3000 or a4000_a5000")};
    if (operation.program_number < 1U || operation.program_number > 128U || operation.volume_name.empty())
        return std::unexpected{invalid("Conversion requires a volume and a program_number between 1 and 128")};
    if (operation.expected_payload_sha256.size() != 64U ||
        !std::ranges::all_of(operation.expected_payload_sha256,
                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return std::unexpected{invalid("expected_payload_sha256 must be lowercase SHA-256")};
    return {};
}

Result<ConvertProgramFormatOperation> parse_program_format_conversion_json(const nlohmann::json &value,
                                                                           PartitionSelector selector) {
    constexpr std::array fields{
        "id", "type", "partition_index", "volume_name", "program_number", "target_format", "expected_payload_sha256"};
    if (!value.is_object() || value.size() != fields.size() ||
        !std::ranges::all_of(fields, [&](const auto *key) { return value.contains(key); }))
        return std::unexpected{invalid("convert_prog_format has invalid fields")};
    for (const auto *key : {"volume_name", "target_format", "expected_payload_sha256"})
        if (!value[key].is_string())
            return std::unexpected{invalid(std::string{key} + " must be a string")};
    if (!value["program_number"].is_number_integer() || value["program_number"] < 1 || value["program_number"] > 128)
        return std::unexpected{invalid("program_number must be an integer between 1 and 128")};
    const auto target = value["target_format"] == "a3000"         ? ProgramStorageFormat::a3000
                        : value["target_format"] == "a4000_a5000" ? ProgramStorageFormat::a4000_a5000
                                                                  : ProgramStorageFormat::unknown;
    ConvertProgramFormatOperation result{std::move(selector), value["volume_name"].get<std::string>(),
                                         value["program_number"].get<std::uint8_t>(), target,
                                         value["expected_payload_sha256"].get<std::string>()};
    if (const auto valid = validate_program_format_conversion(result); !valid)
        return std::unexpected{valid.error()};
    return result;
}
} // namespace axk::detail
