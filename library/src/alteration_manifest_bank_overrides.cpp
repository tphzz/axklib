#include "alteration_manifest_bank_overrides.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "axklib/sample_parameter_codec.hpp"
#include "axklib/sample_parameter_json.hpp"

namespace axk::detail {
namespace {
Error invalid(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}
} // namespace
Result<void> validate_bank_overrides_operation(const UpdateSampleBankOverridesOperation &operation) {
    if (operation.sample_bank_name.empty() || operation.sample_bank_name.size() > 16U ||
        operation.expected_payload_sha256.size() != 64U ||
        !std::ranges::all_of(operation.expected_payload_sha256,
                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return std::unexpected{invalid("A bank name and lowercase baseline SHA-256 are required")};
    std::set<std::uint8_t> ids;
    for (const auto &list : {operation.overrides.enable, operation.overrides.disable})
        for (const auto id : list)
            if (id > 88U || !ids.insert(id).second)
                return std::unexpected{invalid("Override units must be unique and disjoint")};
    if (ids.empty() && !has_sample_parameter_values(operation.overrides.parameters))
        return std::unexpected{invalid("No bank override edit was supplied")};
    return validate_sample_parameter_patch(operation.overrides.parameters);
}
Result<UpdateSampleBankOverridesOperation> parse_bank_overrides_json(const nlohmann::json &row,
                                                                     PartitionSelector selector) {
    constexpr std::array fields{
        "id",     "type",    "partition_index",        "volume_name", "sample_bank_name", "parameters",
        "enable", "disable", "expected_payload_sha256"};
    if (row.size() != fields.size() || !std::ranges::all_of(fields, [&](auto key) { return row.contains(key); }))
        return std::unexpected{invalid("Invalid bank override operation fields")};
    for (const auto key : {"volume_name", "sample_bank_name", "expected_payload_sha256"})
        if (!row[key].is_string() || row[key].get_ref<const std::string &>().empty())
            return std::unexpected{invalid("Bank override identity must contain nonempty strings")};
    auto parameters = parse_sample_parameters_json(row["parameters"], "parameters", false,
                                                   ErrorCode::transaction_rejected, ErrorCategory::transaction);
    if (!parameters)
        return std::unexpected{parameters.error()};
    UpdateSampleBankOverridesOperation result{std::move(selector),
                                              row["volume_name"].get<std::string>(),
                                              row["sample_bank_name"].get<std::string>(),
                                              {std::move(*parameters), {}, {}},
                                              row["expected_payload_sha256"].get<std::string>()};
    for (const auto key : {"enable", "disable"}) {
        if (!row[key].is_array())
            return std::unexpected{invalid("Override units must be arrays")};
        auto &out = std::string_view{key} == "enable" ? result.overrides.enable : result.overrides.disable;
        for (const auto &value : row[key]) {
            if (!value.is_number_integer() || value < 0 || value > 88)
                return std::unexpected{invalid("Invalid override unit")};
            out.push_back(value.get<std::uint8_t>());
        }
    }
    if (auto valid = validate_bank_overrides_operation(result); !valid)
        return std::unexpected{valid.error()};
    return result;
}
} // namespace axk::detail
