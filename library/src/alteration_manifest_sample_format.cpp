#include "alteration_manifest_sample_format.hpp"

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

Result<void> validate_sample_format_conversion(const ConvertSampleBankFormatOperation &operation) {
    return validate_sample_format_conversion(
        ConvertSampleFormatOperation{operation.partition, operation.volume_name, operation.sample_bank_name,
                                     operation.target_format, operation.expected_payload_sha256});
}

Result<void> validate_sample_format_conversion(const ConvertSampleFormatOperation &operation) {
    if (operation.target_format != SampleStorageFormat::a3000_188 &&
        operation.target_format != SampleStorageFormat::a4000_a5000_224)
        return std::unexpected{invalid("target_format must be a3000_188 or a4000_a5000_224")};
    if (operation.sample_name.empty() || operation.sample_name.size() > 16U || operation.volume_name.empty() ||
        !std::ranges::all_of(operation.sample_name, [](unsigned char c) { return c < 128U; }))
        return std::unexpected{invalid("Conversion requires a volume and a 1..16 byte ASCII sample_name")};
    if (operation.expected_payload_sha256.size() != 64U ||
        !std::ranges::all_of(operation.expected_payload_sha256,
                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return std::unexpected{invalid("expected_payload_sha256 must be lowercase SHA-256")};
    return {};
}

Result<ConvertSampleFormatOperation> parse_sample_format_conversion_json(const nlohmann::json &value,
                                                                         PartitionSelector selector) {
    constexpr std::array fields{"id",          "type",          "partition_index",        "volume_name",
                                "sample_name", "target_format", "expected_payload_sha256"};
    if (!value.is_object() || value.size() != fields.size() ||
        !std::ranges::all_of(fields, [&](const auto *key) { return value.contains(key); }))
        return std::unexpected{invalid("convert_sbnk_format has invalid fields")};
    for (const auto *key : {"volume_name", "sample_name", "target_format", "expected_payload_sha256"})
        if (!value[key].is_string())
            return std::unexpected{invalid(std::string{key} + " must be a string")};
    const auto target = value["target_format"] == "a3000_188"         ? SampleStorageFormat::a3000_188
                        : value["target_format"] == "a4000_a5000_224" ? SampleStorageFormat::a4000_a5000_224
                                                                      : SampleStorageFormat::unknown;
    ConvertSampleFormatOperation result{std::move(selector), value["volume_name"].get<std::string>(),
                                        value["sample_name"].get<std::string>(), target,
                                        value["expected_payload_sha256"].get<std::string>()};
    if (const auto valid = validate_sample_format_conversion(result); !valid)
        return std::unexpected{valid.error()};
    return result;
}

Result<ConvertSampleBankFormatOperation> parse_sample_bank_format_conversion_json(const nlohmann::json &value,
                                                                                  PartitionSelector selector) {
    if (!value.is_object() || !value.contains("sample_bank_name") || value.contains("sample_name"))
        return std::unexpected{invalid("convert_sbac_format requires sample_bank_name, not sample_name")};
    auto fields = value;
    fields["sample_name"] = fields["sample_bank_name"];
    fields.erase("sample_bank_name");
    const auto parsed = parse_sample_format_conversion_json(fields, std::move(selector));
    if (!parsed)
        return std::unexpected{parsed.error()};
    return ConvertSampleBankFormatOperation{parsed->partition, parsed->volume_name, parsed->sample_name,
                                            parsed->target_format, parsed->expected_payload_sha256};
}
} // namespace axk::detail
