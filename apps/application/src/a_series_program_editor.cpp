#include "a_series_program_editor.hpp"

#include <charconv>
#include <format>
#include <string>
#include <system_error>
#include <variant>

#include "axklib/package_archive.hpp"
#include "axklib/program_parameter_json.hpp"

namespace axk::app::detail {
namespace {
using Json = nlohmann::json;

void flatten(Json &values, const Json &value, const std::string &prefix = {}) {
    if (prefix.ends_with(".receive")) {
        if (value == "inherit")
            values[prefix] = -1;
        else if (value == "basic")
            values[prefix] = 16;
        else if (value.is_object())
            values[prefix] = value.at("channel").get<int>() + (value.at("port") == "a" ? -1 : 16);
    } else if (value.is_object()) {
        for (const auto &[key, child] : value.items())
            flatten(values, child, prefix.empty() ? key : prefix + "." + key);
    } else if (value.is_number_integer() || value.is_boolean())
        values[prefix] = value;
    else if (value == "inherit")
        values[prefix] = -1;
    else if (value == "off" || value == "none")
        values[prefix] = 0;
    else if (value == "on" || value == "rising")
        values[prefix] = 1;
    else if (value == "falling")
        values[prefix] = 2;
    else if (value == "both")
        values[prefix] = 3;
}
} // namespace

Json a_series_program_editor(const ObjectSnapshot &snapshot, std::span<const std::byte> bytes, bool writable) {
    const auto *program = std::get_if<CurrentProg>(&snapshot.object.payload);
    if (!program)
        return nullptr;
    const auto &name = snapshot.object.header.name;
    unsigned number{};
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), number);
    const bool has_slot =
        parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() && number >= 1U && number <= 128U;
    const auto revision = program->layout.version;
    const bool supported = revision == 2U || revision == 4U;
    const bool editable = writable && supported && snapshot.placement.has_value() && has_slot;
    auto values = Json::object();
    flatten(values, axk::detail::program_parameters_json(program->parameters));
    for (std::size_t slot = 0; slot < program->effect_blocks.size(); ++slot) {
        const auto &effect = program->effect_blocks[slot];
        values[std::format("effects.{}.type", slot + 1U)] = effect.type;
        for (std::size_t word = 0; word < effect.parameter_values.size(); ++word)
            values[std::format("effects.{}.words.{}", slot + 1U, word)] = effect.parameter_values[word];
        // The editor owns full word state; do not expose a second independently editable copy.
        for (std::size_t word = 1; word <= 16U; ++word)
            values.erase(std::format("effects.{}.parameters.{}", slot + 1U, word));
    }
    auto assignments = Json::array();
    for (std::size_t ordinal = 0; ordinal < program->assignments.size(); ++ordinal) {
        const auto &row = program->assignments[ordinal];
        const auto kind = row.kind == 0x10U ? "SBNK" : row.kind == 0x11U ? "SBAC" : "UNKNOWN";
        assignments.push_back({{"ordinal", ordinal}, {"kind", kind}, {"name", row.name}, {"targetObjectId", nullptr}});
        flatten(values, axk::detail::program_assignment_parameters_json(row.parameters),
                std::format("assignments.{}", ordinal));
    }
    return {{"profile", "a-series/program"},
            {"editable", editable},
            {"reason", editable     ? ""
                       : !supported ? "Revision-1 Programs are read-only. Convert explicitly to edit parameters."
                       : !writable  ? "This image is read-only."
                                    : "This Program has no writable volume placement or numeric slot."},
            {"payloadSha256", package_internal::hex_digest(package_internal::sha256(bytes))},
            {"partitionIndex", snapshot.partition.value},
            {"volumeName", snapshot.placement ? snapshot.placement->volume_name : ""},
            {"programNumber", has_slot ? Json(number) : Json(nullptr)},
            {"programName", program->program_name},
            {"storageRevision", revision},
            {"model", revision == 4U ? "A5000" : "A3000"},
            {"values", std::move(values)},
            {"assignments", std::move(assignments)},
            {"targets", Json::array()}};
}
} // namespace axk::app::detail
