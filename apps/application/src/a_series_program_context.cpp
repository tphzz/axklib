#include "a_series_program_context.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <tuple>
#include <variant>

#include "axklib/sample_bank_overrides.hpp"
#include "axklib/sample_parameter_json.hpp"

namespace axk::app::detail {
namespace {
using Json = nlohmann::ordered_json;
Json preview_values(const SampleParameters &parameters) {
    const auto source = axk::detail::sample_parameters_json(parameters);
    auto result = Json::object();
    for (const auto *key : {"root_key", "key_low", "key_high", "velocity_low", "velocity_high"})
        if (source.contains(key))
            result[key] = source.at(key);
    if (source.contains("aeg"))
        for (const auto &[key, value] : source.at("aeg").items())
            if (value.is_number_integer() || value.is_boolean())
                result["aeg." + key] = value;
    return result;
}
bool same_volume(const ObjectSnapshot &a, const ObjectSnapshot &b) {
    if (a.partition.value != b.partition.value)
        return false;
    if (a.placement && b.placement)
        return a.placement->volume_directory.value == b.placement->volume_directory.value;
    return !a.placement && !b.placement && a.scope_key == b.scope_key;
}
} // namespace

void add_program_editing_context(Json &editing, std::string_view program_id,
                                 const std::unordered_map<std::string, ObjectSnapshot> &objects,
                                 std::span<const ImageRelationshipItem> relationships) {
    if (editing.is_null())
        return;
    const auto &program = objects.at(std::string(program_id));
    std::set<std::string> members;
    for (const auto &relation : relationships) {
        if (relation.type == "SBAC_SLOT_TO_SBNK" && relation.target_object_id)
            members.insert(*relation.target_object_id);
        if (relation.source_object_id == program_id && relation.type.starts_with("PROG_ASSIGNMENT_TO_") &&
            relation.assignment_index && *relation.assignment_index < editing.at("assignments").size())
            editing["assignments"][*relation.assignment_index]["targetObjectId"] =
                relation.target_object_id ? Json(*relation.target_object_id) : Json(nullptr);
    }
    auto targets = Json::array();
    for (const auto &[id, object] : objects) {
        if (!same_volume(program, object))
            continue;
        const auto *sample = std::get_if<CurrentSbnk>(&object.object.payload);
        const auto *bank = std::get_if<CurrentSbac>(&object.object.payload);
        if (!sample && !bank)
            continue;
        Json target{{"objectId", id},
                    {"kind", sample ? "SBNK" : "SBAC"},
                    {"name", object.object.header.name},
                    {"assignable", true},
                    {"reason", ""},
                    {"available", false},
                    {"values", Json::object()},
                    {"overrideKeys", Json::array()},
                    {"members", Json::array()}};
        const auto &storage = sample ? sample->storage : bank->storage;
        const auto generation = sample_parameter_generation(storage.format);
        if (storage.structurally_valid && generation) {
            const auto bytes =
                sample ? std::span<const std::byte>(sample->raw_parameter_window)
                       : std::span<const std::byte>(bank->raw_sample_parameter_block).first(*storage.parameter_bytes);
            if (const auto decoded = decode_sample_parameter_block(bytes, *generation)) {
                target["available"] = !bank || sample_bank_override_state_supported(*bank);
                target["values"] = preview_values(decoded->parameters);
            }
            if (bank)
                for (const auto &unit : sample_bank_override_units(*generation))
                    if (std::ranges::any_of(unit.selectors, [&](auto selector) {
                            return (bank->override_enable_words[selector / 32U] & (1U << (selector % 32U))) != 0U;
                        }))
                        for (const auto &key : unit.keys)
                            target["overrideKeys"].push_back(key);
        }
        if (sample && ((sample->sample_flags & 1U) != 0U || members.contains(id))) {
            target["assignable"] = false;
            target["reason"] = "Sample Bank member; assign its bank.";
        }
        if (bank)
            for (const auto &relation : relationships)
                if (relation.source_object_id == id && relation.type == "SBAC_SLOT_TO_SBNK")
                    target["members"].push_back(
                        {{"name", relation.target_object_id && objects.contains(*relation.target_object_id)
                                      ? objects.at(*relation.target_object_id).object.header.name
                                      : relation.assignment_name},
                         {"objectId", relation.target_object_id ? Json(*relation.target_object_id) : Json(nullptr)}});
        targets.push_back(std::move(target));
    }
    std::sort(targets.begin(), targets.end(), [](const Json &a, const Json &b) {
        return std::tuple(a.at("kind"), a.at("name"), a.at("objectId")) <
               std::tuple(b.at("kind"), b.at("name"), b.at("objectId"));
    });
    editing["targets"] = std::move(targets);
}
} // namespace axk::app::detail
