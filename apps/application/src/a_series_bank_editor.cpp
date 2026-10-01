#include "a_series_sample_editor.hpp"

#include <algorithm>
#include <cstddef>
#include <set>
#include <span>
#include <string>
#include <variant>

#include "axklib/application/sample_formats.hpp"
#include "axklib/package_archive.hpp"
#include "axklib/sample_bank_overrides.hpp"
#include "axklib/sample_parameter_json.hpp"

namespace axk::app::detail {
nlohmann::json a_series_bank_editor(const ObjectSnapshot &snapshot, std::span<const std::byte> bytes, bool writable,
                                    const nlohmann::json &relationships) {
    using Json = nlohmann::json;
    const auto *bank = std::get_if<CurrentSbac>(&snapshot.object.payload);
    if (!bank || !bank->storage.structurally_valid)
        return nullptr;
    const auto generation = *sample_parameter_generation(bank->storage.format);
    const auto decoded = decode_sample_parameter_block(
        std::span{bank->raw_sample_parameter_block}.first(*bank->storage.parameter_bytes), generation);
    if (!decoded)
        return nullptr;
    auto units = Json::array();
    std::set<std::string> allowed;
    for (const auto &unit : sample_bank_override_units(generation)) {
        auto active = Json::array();
        for (const auto selector : unit.selectors)
            if ((bank->override_enable_words[selector / 32U] & (1U << (selector % 32U))) != 0U)
                active.push_back(selector);
        units.push_back(
            {{"id", unit.id}, {"keys", unit.keys}, {"selectors", unit.selectors}, {"activeSelectors", active}});
        allowed.insert(unit.keys.begin(), unit.keys.end());
    }
    auto capabilities = sample_parameter_capabilities(*bank);
    for (const auto other : {SampleParameterGeneration::a3000, SampleParameterGeneration::a4000_a5000})
        for (const auto &unit : sample_bank_override_units(other))
            allowed.insert(unit.keys.begin(), unit.keys.end());
    auto blocked = Json::array();
    auto reasons = Json::object();
    auto unavailable = Json::object();
    for (auto &[key, value] : capabilities.items()) {
        if (!allowed.contains(key)) {
            blocked.push_back(key);
            reasons[key] = "This parameter cannot be overridden by a Sample Bank.";
        }
        if (!value.at("available").get<bool>())
            unavailable[key] = {{"reason", "FORMAT_UNAVAILABLE"}, {"message", value.at("reason")}};
    }
    auto members = Json::array();
    for (const auto &slot : bank->slots) {
        if (!slot.active)
            continue;
        Json id = nullptr;
        for (const auto &relation : relationships) {
            const auto &target = relation.at("targetObject");
            if (!target.is_null() && target.at("type") == "SBNK" && target.at("name") == slot.name &&
                std::ranges::find(relation.at("selectedObjectRoles"), "SOURCE") !=
                    relation.at("selectedObjectRoles").end()) {
                id = target.at("id");
                break;
            }
        }
        members.push_back({{"name", slot.name}, {"objectId", id}});
    }
    const bool supported = sample_bank_override_state_supported(*bank);
    const bool editable = writable && snapshot.placement.has_value() && supported;
    return {{"profile", "a-series/sample-bank"},
            {"editable", editable},
            {"reason", editable ? ""
                       : !supported
                           ? "This bank has an unsupported override state. Its stored values and flags are preserved."
                           : "This image does not support Sample Bank editing."},
            {"payloadSha256", package_internal::hex_digest(package_internal::sha256(bytes))},
            {"partitionIndex", snapshot.partition.value},
            {"volumeName", snapshot.placement ? snapshot.placement->volume_name : ""},
            {"parameters", axk::detail::sample_parameters_json(decoded->parameters)},
            {"eqCoefficients", decoded->eq_coefficients},
            {"sampleFormat", sample_format_metadata(*bank)},
            {"parameterCapabilities", capabilities},
            {"blockedParameters", blocked},
            {"blockedParameterReasons", reasons},
            {"unavailableParameters", unavailable},
            {"playbackWindow", {{"start_frame", 0}, {"length_frames", 0}}},
            {"canEditPlayback", false},
            {"maximumFrames", 0},
            {"sources", Json::array()},
            {"bankOverrides", {{"units", units}, {"members", members}}}};
}
} // namespace axk::app::detail
