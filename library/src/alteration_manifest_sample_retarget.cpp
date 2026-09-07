#include "alteration_manifest_wave_data.hpp"

#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace axk::detail {
namespace {
Error invalid() {
    return make_error(ErrorCode::manifest_invalid, ErrorCategory::manifest,
                      "Sample retarget requires complete source names and a lowercase payload SHA-256 guard");
}
} // namespace

Result<void> validate_sample_retarget(const RetargetSampleWaveDataOperation &operation) {
    const auto name = [](std::string_view text) {
        return !text.empty() && text.size() <= 16U &&
               std::ranges::all_of(text, [](unsigned char c) { return c >= 32U && c < 127U; });
    };
    if (!name(operation.sample_name) || !name(operation.waveform_name) ||
        (operation.right_waveform_name &&
         (!name(*operation.right_waveform_name) || *operation.right_waveform_name == operation.waveform_name)) ||
        operation.expected_payload_sha256.size() != 64U ||
        !std::ranges::all_of(operation.expected_payload_sha256,
                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return std::unexpected{invalid()};
    return {};
}

Result<RetargetSampleWaveDataOperation> parse_sample_retarget_json(const nlohmann::json &value,
                                                                   PartitionSelector selector) {
    constexpr std::array required{"id",          "type",          "partition_index",        "volume_name",
                                  "sample_name", "waveform_name", "expected_payload_sha256"};
    if (!value.is_object() || !std::ranges::all_of(required, [&](const auto *key) { return value.contains(key); }) ||
        value.size() != required.size() + (value.contains("right_waveform_name") ? 1U : 0U))
        return std::unexpected{invalid()};
    for (const auto *key :
         {"volume_name", "sample_name", "waveform_name", "expected_payload_sha256", "right_waveform_name"})
        if (value.contains(key) && !value[key].is_string())
            return std::unexpected{invalid()};
    RetargetSampleWaveDataOperation result{std::move(selector),
                                           value["volume_name"].get<std::string>(),
                                           value["sample_name"].get<std::string>(),
                                           value["waveform_name"].get<std::string>(),
                                           value["expected_payload_sha256"].get<std::string>(),
                                           {}};
    if (value.contains("right_waveform_name"))
        result.right_waveform_name = value["right_waveform_name"].get<std::string>();
    if (auto valid = validate_sample_retarget(result); !valid)
        return std::unexpected{valid.error()};
    return result;
}
} // namespace axk::detail
