#include "alteration_manifest_program.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "axklib/program_spec_json.hpp"
#include "axklib/writer_internal.hpp"

namespace axk::detail {
namespace {
using Json = nlohmann::json;
Error invalid(std::string message) {
    return make_error(ErrorCode::transaction_rejected, ErrorCategory::transaction, std::move(message));
}
} // namespace

Result<void> validate_program_assignment_replacement(const ReplaceProgramAssignmentsOperation &operation) {
    if (operation.program_number < 1U || operation.program_number > 128U ||
        (operation.model != ASeriesModel::a4000 && operation.model != ASeriesModel::a5000) ||
        operation.assignments.size() > maximum_program_assignments)
        return std::unexpected{invalid("Program replacement requires a valid model, slot and 0..999 assignments")};
    if (operation.expected_payload_sha256.size() != 64U ||
        !std::ranges::all_of(operation.expected_payload_sha256,
                             [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))
        return std::unexpected{invalid("expected_payload_sha256 must contain 64 lowercase hexadecimal characters")};
    std::set<std::size_t> retained;
    ProgramSpec authored{operation.program_number, "Edit", {}};
    authored.model = operation.model;
    for (const auto &entry : operation.assignments) {
        if (!entry.retain_ordinal && !entry.assignment)
            return std::unexpected{invalid("Assignment requires retain_ordinal or a target")};
        if (entry.retain_ordinal &&
            (*entry.retain_ordinal >= maximum_program_assignments || !retained.insert(*entry.retain_ordinal).second))
            return std::unexpected{invalid("Retained ordinals must be distinct and within 0..998")};
        if (entry.assignment)
            authored.assignments.push_back(*entry.assignment);
    }
    if (auto payload = prepare_prog_payload(authored); !payload)
        return std::unexpected{payload.error()};
    return {};
}

Result<ReplaceProgramAssignmentsOperation> parse_program_assignment_replacement_json(const Json &row,
                                                                                     PartitionSelector selector) {
    constexpr std::array fields{
        "id",         "type", "partition_index", "volume_name", "program_number", "model", "expected_payload_sha256",
        "assignments"};
    if (!row.is_object() || row.size() != fields.size() ||
        !std::ranges::all_of(fields, [&](const char *field) { return row.contains(field); }) ||
        !row["volume_name"].is_string() || !row["expected_payload_sha256"].is_string() ||
        !row["assignments"].is_array() || row["assignments"].size() > maximum_program_assignments)
        return std::unexpected{invalid("Invalid Program assignment replacement fields")};
    const Json header{
        {"number", row["program_number"]}, {"name", "Edit"}, {"model", row["model"]}, {"assignments", Json::array()}};
    auto spec = parse_program_spec_json(header);
    if (!spec)
        return std::unexpected{spec.error()};
    ReplaceProgramAssignmentsOperation result{std::move(selector),
                                              row["volume_name"].get<std::string>(),
                                              spec->number,
                                              spec->model,
                                              row["expected_payload_sha256"].get<std::string>(),
                                              {}};
    for (auto entry : row["assignments"]) {
        if (!entry.is_object())
            return std::unexpected{invalid("Program assignment entry must be an object")};
        ProgramAssignmentEdit edit;
        if (entry.contains("retain_ordinal")) {
            const auto &ordinal = entry["retain_ordinal"];
            if (!ordinal.is_number_integer() || ordinal < 0 || ordinal >= maximum_program_assignments)
                return std::unexpected{invalid("retain_ordinal must be an integer within 0..998")};
            edit.retain_ordinal = ordinal.get<std::size_t>();
            entry.erase("retain_ordinal");
        }
        if (!entry.empty()) {
            auto single = header;
            single["assignments"].push_back(entry);
            auto parsed = parse_program_spec_json(single);
            if (!parsed)
                return std::unexpected{parsed.error()};
            edit.assignment = std::move(parsed->assignments.front());
        }
        result.assignments.push_back(std::move(edit));
    }
    if (auto valid = validate_program_assignment_replacement(result); !valid)
        return std::unexpected{valid.error()};
    return result;
}

} // namespace axk::detail
