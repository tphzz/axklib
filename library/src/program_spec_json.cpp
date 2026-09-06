#include "axklib/program_spec_json.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/program_parameter_json.hpp"
#include "program_parameter_json_internal.hpp"

namespace axk::detail {
namespace {

using namespace program_json_internal;

Result<std::string> name(const Json &value, std::string_view key, std::size_t maximum) {
    if (!value.contains(key) || !value[key].is_string())
        return std::unexpected{invalid(std::string{key} + " must be a string")};
    auto result = value[key].get<std::string>();
    if (result.empty() || result.size() > maximum || result.front() == ' ' || result.back() == ' ' ||
        !std::ranges::all_of(result, [](unsigned char c) { return c >= 0x20U && c <= 0x7eU; }))
        return std::unexpected{invalid(std::string{key} + " must be a nonempty, unpadded ASCII name")};
    return result;
}

Result<ProgramAssignmentSpec> assignment(const Json &value) {
    if (auto valid = fields(value, {"sample", "sample_bank", "parameters"}); !valid)
        return std::unexpected{valid.error()};
    if (value.contains("sample") == value.contains("sample_bank"))
        return std::unexpected{invalid("assignment must contain exactly one target")};
    const auto bank = value.contains("sample_bank");
    auto target = name(value, bank ? "sample_bank" : "sample", 16U);
    if (!target)
        return std::unexpected{target.error()};
    ProgramAssignmentSpec result{bank ? "SBAC" : "SBNK", std::move(*target)};
    if (auto valid = child(value, "parameters", result.parameters, parse_program_assignment_parameters_json); !valid)
        return std::unexpected{valid.error()};
    return result;
}

} // namespace

Result<ProgramSpec> parse_program_spec_json(const Json &value) {
    if (auto valid = fields(value, {"number", "name", "model", "parameters", "assignments"}); !valid)
        return std::unexpected{valid.error()};
    std::optional<std::uint8_t> number;
    if (auto valid = read(value, "number", number); !valid)
        return std::unexpected{valid.error()};
    if (!number || *number < 1U || *number > 128U)
        return std::unexpected{invalid("number must be between 1 and 128")};
    auto label = name(value, "name", 8U);
    if (!label)
        return std::unexpected{label.error()};
    ProgramSpec result{*number, std::move(*label), {}};
    if (value.contains("model")) {
        if (value["model"] == "A5000")
            result.model = ASeriesModel::a5000;
        else if (value["model"] != "A4000")
            return std::unexpected{invalid("model must be A4000 or A5000")};
    }
    if (auto valid = child(value, "parameters", result.parameters, parse_program_parameters_json); !valid)
        return std::unexpected{valid.error()};
    if (!value.contains("assignments") || !value["assignments"].is_array() || value["assignments"].empty() ||
        value["assignments"].size() > maximum_program_assignments)
        return std::unexpected{invalid("assignments must contain 1..16 rows")};
    for (const auto &row : value["assignments"]) {
        auto parsed = assignment(row);
        if (!parsed)
            return std::unexpected{parsed.error()};
        result.assignments.push_back(std::move(*parsed));
    }
    return result;
}

Json program_spec_json(const ProgramSpec &value) {
    if (value.model != ASeriesModel::a4000 && value.model != ASeriesModel::a5000)
        throw std::invalid_argument("Program model must be A4000 or A5000");
    Json result = {{"number", value.number},
                   {"name", value.name},
                   {"model", value.model == ASeriesModel::a5000 ? "A5000" : "A4000"},
                   {"assignments", Json::array()}};
    write_group(result, "parameters", program_parameters_json(value.parameters));
    for (const auto &source : value.assignments) {
        if (source.target_kind != "SBAC" && source.target_kind != "SBNK")
            throw std::invalid_argument("Program assignment target must be SBAC or SBNK");
        Json row = {{source.target_kind == "SBAC" ? "sample_bank" : "sample", source.target_name}};
        write_group(row, "parameters", program_assignment_parameters_json(source.parameters));
        result["assignments"].push_back(std::move(row));
    }
    return result;
}

} // namespace axk::detail
