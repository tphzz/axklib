#include "alteration_manifest_duplicate.hpp"

#include "axklib/sample_parameter_json.hpp"
#include <algorithm>
#include <array>
#include <expected>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace axk::detail {
Result<DuplicateSampleOperation> parse_sample_duplicate_json(const nlohmann::json &value, PartitionSelector selector) {
    constexpr std::array required{"id",          "type",     "partition_index", "volume_name",
                                  "sample_name", "new_name", "parameters"};
    if (!value.is_object() || !std::ranges::all_of(required, [&](const auto *key) { return value.contains(key); }) ||
        value.size() != required.size() + (value.contains("playback_window") ? 1U : 0U) +
                            (value.contains("expected_payload_sha256") ? 1U : 0U))
        return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                          "duplicate_sbnk has invalid fields")};
    for (const auto *key : {"volume_name", "sample_name", "new_name", "expected_payload_sha256"})
        if (value.contains(key) && !value[key].is_string())
            return std::unexpected{make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction,
                                              std::string{key} + " must be a string")};
    auto parameters = parse_sample_parameters_json(value["parameters"], "duplicate_sbnk.parameters", false,
                                                   ErrorCode::transaction_rejected, ErrorCategory::transaction);
    if (!parameters)
        return std::unexpected{parameters.error()};
    DuplicateSampleOperation operation{std::move(selector), value["volume_name"].get<std::string>(),
                                       value["sample_name"].get<std::string>(), value["new_name"].get<std::string>(),
                                       std::move(*parameters)};
    if (value.contains("playback_window")) {
        auto window = parse_sample_playback_window_json(value["playback_window"]);
        if (!window)
            return std::unexpected{window.error()};
        operation.playback_window = *window;
    }
    if (value.contains("expected_payload_sha256"))
        operation.expected_payload_sha256 = value["expected_payload_sha256"].get<std::string>();
    return operation;
}
} // namespace axk::detail
