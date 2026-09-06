#include "alteration_manifest_internal.hpp"
#include "alteration_manifest_program.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "axklib/program_parameter_codec.hpp"
#include "axklib/program_parameter_json.hpp"
#include "program_parameter_json_internal.hpp"

namespace axk::detail {
namespace {
using namespace program_json_internal;

bool valid_name(std::string_view name) {
    return !name.empty() && name.size() <= 16U &&
           std::ranges::all_of(name, [](unsigned char value) { return value < 0x80U; });
}

Result<std::string> text(const Json &value, std::string_view field) {
    if (!value.contains(field) || !value[field].is_string())
        return std::unexpected{invalid(std::string{field} + " must be a string")};
    return value[field].get<std::string>();
}

template <typename T> Result<T> integer(const Json &value, std::string_view field) {
    if (!value.contains(field))
        return std::unexpected{invalid(std::string{field} + " is required")};
    std::optional<T> result;
    if (auto parsed = scalar(value[field], result); !parsed)
        return std::unexpected{parsed.error()};
    return *result;
}
} // namespace

Result<void> validate_program_parameter_update(const UpdateProgramParametersOperation &operation) {
    if (operation.model != ASeriesModel::a4000 && operation.model != ASeriesModel::a5000)
        return std::unexpected{invalid("model must be explicitly A4000 or A5000")};
    if (operation.program_number < 1U || operation.program_number > 128U)
        return std::unexpected{invalid("program_number must be 1..128")};
    if (operation.volume_name.empty())
        return std::unexpected{invalid("invalid volume_name")};
    if (!has_program_parameter_values(operation.parameters) && operation.assignments.empty())
        return std::unexpected{invalid("update must contain at least one writable parameter")};
    if (operation.assignments.size() > maximum_stored_program_assignments)
        return std::unexpected{invalid("too many assignment patches")};
    std::set<std::size_t> ordinals;
    for (const auto &row : operation.assignments) {
        if (row.ordinal >= maximum_stored_program_assignments || !ordinals.insert(row.ordinal).second)
            return std::unexpected{invalid("assignment ordinals must be distinct and within 0..998")};
        if ((row.expected_target_kind != "SBNK" && row.expected_target_kind != "SBAC") ||
            !valid_name(row.expected_target_name))
            return std::unexpected{invalid("assignment patch requires an expected Sample or Sample Bank target")};
        if (!has_program_assignment_parameter_values(row.parameters))
            return std::unexpected{invalid("each assignment patch must contain at least one writable parameter")};
    }
    return {};
}

Result<UpdateProgramParametersOperation> parse_program_parameter_update_json(const Json &row,
                                                                             PartitionSelector selector) {
    if (auto valid = fields(row, {"id", "type", "partition_index", "volume_name", "program_number", "model",
                                  "parameters", "assignments"});
        !valid)
        return std::unexpected{valid.error()};
    const auto model = text(row, "model");
    const auto volume = text(row, "volume_name");
    const auto number = integer<std::uint8_t>(row, "program_number");
    if (!model)
        return std::unexpected{model.error()};
    if (!volume)
        return std::unexpected{volume.error()};
    if (!number)
        return std::unexpected{number.error()};
    if (*model != "A4000" && *model != "A5000")
        return std::unexpected{invalid("model must be A4000 or A5000")};
    UpdateProgramParametersOperation result{
        std::move(selector), *volume, *number, *model == "A4000" ? ASeriesModel::a4000 : ASeriesModel::a5000, {}, {}};
    if (auto parsed = child(row, "parameters", result.parameters, parse_program_parameters_json); !parsed)
        return std::unexpected{parsed.error()};
    if (row.contains("assignments")) {
        if (!row["assignments"].is_array() || row["assignments"].size() > maximum_stored_program_assignments)
            return std::unexpected{invalid("assignments must be an array with at most 999 patches")};
        for (const auto &assignment : row["assignments"]) {
            if (auto valid =
                    fields(assignment, {"ordinal", "expected_target_kind", "expected_target_name", "parameters"});
                !valid)
                return std::unexpected{valid.error()};
            const auto ordinal = integer<std::uint16_t>(assignment, "ordinal");
            const auto kind = text(assignment, "expected_target_kind");
            const auto name = text(assignment, "expected_target_name");
            if (!ordinal)
                return std::unexpected{ordinal.error()};
            if (!kind)
                return std::unexpected{kind.error()};
            if (!name)
                return std::unexpected{name.error()};
            ProgramAssignmentParameters parameters;
            if (auto parsed = child(assignment, "parameters", parameters, parse_program_assignment_parameters_json);
                !parsed)
                return std::unexpected{parsed.error()};
            result.assignments.push_back({*ordinal, *kind, *name, std::move(parameters)});
        }
    }
    if (auto valid = validate_program_parameter_update(result); !valid)
        return std::unexpected{valid.error()};
    return result;
}

} // namespace axk::detail
