#include "alteration_manifest_internal.hpp"

#include <algorithm>
#include <ranges>
#include <string>

#include <nlohmann/json.hpp>

#include "axklib/sample_parameter_json.hpp"

namespace axk::detail {
namespace {

using Json = nlohmann::json;

Error manifest_error(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}

Result<void> exact_fields(const Json &row, std::initializer_list<std::string_view> expected, std::string_view context) {
    if (!row.is_object() || row.size() != expected.size())
        return std::unexpected{manifest_error(std::string{context} + " has invalid fields")};
    for (const auto field : expected) {
        if (!row.contains(field)) {
            return std::unexpected{manifest_error(std::string{context} + " is missing field " + std::string{field})};
        }
    }
    return {};
}

Result<void> fields(const Json &row, std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional, std::string_view context) {
    if (!row.is_object())
        return std::unexpected{manifest_error(std::string{context} + " must be an object")};
    for (const auto field : required) {
        if (!row.contains(field))
            return std::unexpected{manifest_error(std::string{context} + " is missing field " + std::string{field})};
    }
    for (const auto &[field, ignored] : row.items()) {
        static_cast<void>(ignored);
        if (!std::ranges::contains(required, field) && !std::ranges::contains(optional, field))
            return std::unexpected{manifest_error(std::string{context} + " has invalid field " + field)};
    }
    return {};
}

Result<std::string> object_name(const Json &row, std::string_view field, std::string_view context) {
    if (!row.contains(field) || !row[field].is_string() || row[field].get_ref<const std::string &>().empty()) {
        return std::unexpected{
            manifest_error(std::string{context} + "." + std::string{field} + " must be a non-empty string")};
    }
    auto result = row[field].get<std::string>();
    const auto ascii = std::ranges::all_of(result, [](unsigned char value) { return value < 0x80U; });
    if (result.size() > 16U || !ascii) {
        return std::unexpected{
            manifest_error(std::string{context} + "." + std::string{field} + " must fit 16 ASCII bytes")};
    }
    return result;
}

Result<SampleParameters> parameter_overrides(const Json &row, std::string_view context) {
    return parse_sample_parameters_json(row, context, true, ErrorCode::transaction_rejected,
                                        ErrorCategory::transaction);
}

} // namespace

Result<InsertSampleBankOperation> parse_insert_sample_bank_json(const Json &row, PartitionSelector selector,
                                                                std::string_view context) {
    if (auto valid = exact_fields(row, {"id", "type", "partition_index", "volume_name", "sample_bank"}, context);
        !valid) {
        return std::unexpected{valid.error()};
    }
    if (!row["volume_name"].is_string() || row["volume_name"].get_ref<const std::string &>().empty())
        return std::unexpected{manifest_error(std::string{context} + ".volume_name must be a non-empty string")};
    const auto &sample_bank = row["sample_bank"];
    const auto sample_bank_context = std::string{context} + ".sample_bank";
    if (auto valid = fields(sample_bank, {"name", "member_samples"}, {"parameter_overrides"}, sample_bank_context);
        !valid) {
        return std::unexpected{valid.error()};
    }
    auto name = object_name(sample_bank, "name", sample_bank_context);
    if (!name)
        return std::unexpected{name.error()};
    if (!sample_bank["member_samples"].is_array() || sample_bank["member_samples"].empty() ||
        sample_bank["member_samples"].size() > maximum_sample_bank_members) {
        return std::unexpected{manifest_error("member_samples must contain 1..127 names")};
    }
    SampleBankSpec spec;
    spec.name = *name;
    for (const auto &member_value : sample_bank["member_samples"]) {
        Json wrapper{{"name", member_value}};
        auto member = object_name(wrapper, "name", sample_bank_context + ".member_samples");
        if (!member)
            return std::unexpected{member.error()};
        if (std::ranges::contains(spec.member_samples, *member))
            return std::unexpected{manifest_error("member_samples must be distinct")};
        spec.member_samples.push_back(std::move(*member));
    }
    if (sample_bank.contains("parameter_overrides")) {
        auto overrides =
            parameter_overrides(sample_bank["parameter_overrides"], sample_bank_context + ".parameter_overrides");
        if (!overrides)
            return std::unexpected{overrides.error()};
        spec.parameter_overrides = std::move(*overrides);
    }
    return InsertSampleBankOperation{std::move(selector), row["volume_name"].get<std::string>(), std::move(spec)};
}

Result<AlterationOperationData> parse_sample_bank_assignment_json(const Json &row, PartitionSelector selector,
                                                                  std::string_view context) {
    if (auto valid = exact_fields(
            row, {"id", "type", "partition_index", "volume_name", "sample_bank_name", "sample_names"}, context);
        !valid) {
        return std::unexpected{valid.error()};
    }
    if (!row["volume_name"].is_string() || row["volume_name"].get_ref<const std::string &>().empty()) {
        return std::unexpected{manifest_error(std::string{context} + ".volume_name must be a non-empty string")};
    }
    auto sample_bank_name = object_name(row, "sample_bank_name", context);
    if (!sample_bank_name)
        return std::unexpected{sample_bank_name.error()};
    if (!row["sample_names"].is_array() || row["sample_names"].empty() ||
        row["sample_names"].size() > maximum_sample_bank_members) {
        return std::unexpected{manifest_error("sample_names must contain 1..127 names")};
    }
    std::vector<std::string> sample_names;
    sample_names.reserve(row["sample_names"].size());
    for (const auto &value : row["sample_names"]) {
        Json wrapper{{"name", value}};
        auto name = object_name(wrapper, "name", std::string{context} + ".sample_names");
        if (!name)
            return std::unexpected{name.error()};
        if (std::ranges::contains(sample_names, *name))
            return std::unexpected{manifest_error("sample_names must be distinct")};
        sample_names.push_back(std::move(*name));
    }
    return AssignSampleBankMembersOperation{std::move(selector), row["volume_name"].get<std::string>(),
                                            std::move(*sample_bank_name), std::move(sample_names)};
}

} // namespace axk::detail
