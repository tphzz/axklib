#include "axklib/application/sample_formats.hpp"

#include <algorithm>
#include <string>

#include "axklib/package_archive.hpp"
#include "axklib/sample_format_conversion.hpp"
#include "axklib/sample_format_conversion_internal.hpp"

namespace axk::app {
namespace {
using Json = nlohmann::json;
Json issues_json(const std::vector<SampleParameterIssue> &issues) {
    auto result = Json::array();
    for (const auto &issue : issues)
        result.push_back({{"key", issue.key},
                          {"message", issue.message},
                          {"storedValue", issue.stored_value ? Json(*issue.stored_value) : Json(nullptr)}});
    return result;
}
Json domain(const std::optional<SampleParameterLocation> &location) {
    if (!location)
        return nullptr;
    return {{"minimum", location->minimum},
            {"maximum", location->maximum},
            {"extraValue", location->extra_value ? Json(*location->extra_value) : Json(nullptr)},
            {"offset", location->offset},
            {"mask", location->mask}};
}
Json format_metadata(const SampleStorageInfo &storage, std::span<const std::byte> block) {
    auto issues = Json::array();
    bool a5000 = false;
    Json extension_differs = nullptr;
    if (storage.structurally_valid) {
        const auto generation = sample_parameter_generation(storage.format);
        block = block.first(*storage.parameter_bytes);
        issues = issues_json(assess_sample_parameter_block(block, *generation));
        if (storage.format == SampleStorageFormat::a4000_a5000_224) {
            for (const auto &rule : sample_parameter_rules())
                if (rule.a4000_a5000 && rule.a4000_a5000->a5000_minimum) {
                    const auto value = read_sample_parameter_value(block, *rule.a4000_a5000);
                    a5000 = a5000 || (value && sample_parameter_value_allowed(*rule.a4000_a5000, *value) &&
                                      *value >= *rule.a4000_a5000->a5000_minimum);
                }
            const auto extended = axk::detail::extend_sample_parameter_prefix(block.first(188U));
            if (extended)
                extension_differs = !std::ranges::equal(block.subspan(188U), std::span{*extended}.subspan(188U));
        }
    }
    return {{"format", sample_storage_format_name(storage.format)},
            {"structurallyValid", storage.structurally_valid},
            {"parameterBytes", storage.parameter_bytes ? Json(*storage.parameter_bytes) : Json(nullptr)},
            {"headerRevision", storage.header_revision},
            {"olderBodyBytes", storage.older_body_bytes},
            {"laterBodyBytes", storage.later_body_bytes},
            {"diagnostics", storage.diagnostics},
            {"parameterIssues", issues},
            {"requiresA5000", a5000},
            {"extensionDiffersFromPrefixDefaults", extension_differs}};
}
} // namespace

Json sample_format_metadata(const CurrentSbnk &sample) {
    return format_metadata(sample.storage, sample.raw_parameter_window);
}

Json sample_format_metadata(const CurrentSbac &bank) {
    return format_metadata(bank.storage, bank.raw_sample_parameter_block);
}

Json sample_parameter_capabilities(const CurrentSbnk &sample) {
    auto result = Json::object();
    const bool native = sample.storage.format == SampleStorageFormat::a3000_188;
    for (const auto &rule : sample_parameter_rules()) {
        const auto active = sample.storage.structurally_valid ? (native ? rule.a3000 : rule.a4000_a5000) : std::nullopt;
        const auto value = active ? read_sample_parameter_value(sample.raw_parameter_window, *active) : std::nullopt;
        const bool valid = value && sample_parameter_value_allowed(*active, *value);
        const auto reason = !sample.storage.structurally_valid ? "This stored Sample format is not recognized."
                            : !active ? (native ? "Convert explicitly to a4k/a5k format to edit this setting."
                                                : "This A3000 switch is replaced by independent crossfade widths.")
                            : !valid  ? "The stored value is outside this format's supported range. Choose a supported "
                                        "value to replace it; other edits preserve it."
                                      : "";
        result[rule.key] = {{"available", active.has_value()},
                            {"editable", active.has_value()},
                            {"valid", valid},
                            {"reason", reason},
                            {"a3000", domain(rule.a3000)},
                            {"a4000A5000", domain(rule.a4000_a5000)},
                            {"storedValue", value ? Json(*value) : Json(nullptr)},
                            {"a5000Minimum", rule.a4000_a5000 && rule.a4000_a5000->a5000_minimum
                                                 ? Json(*rule.a4000_a5000->a5000_minimum)
                                                 : Json(nullptr)}};
    }
    return result;
}

Json sample_format_conversion_previews(std::span<const std::byte> payload, bool bank) {
    auto result = Json::array();
    const auto source = (bank ? inspect_sample_bank_storage(payload) : inspect_sample_storage(payload)).format;
    for (const auto target : {SampleStorageFormat::a3000_188, SampleStorageFormat::a4000_a5000_224}) {
        if (target == source)
            continue;
        const auto plan =
            bank ? plan_sample_bank_format_conversion(payload, target) : plan_sample_format_conversion(payload, target);
        result.push_back({{"targetFormat", sample_storage_format_name(target)},
                          {"allowed", plan.allowed()},
                          {"changes", plan.changes},
                          {"blockers", issues_json(plan.blockers)}});
    }
    return result;
}

Json object_format_conversion(const ObjectSnapshot &snapshot, std::span<const std::byte> payload, bool writable) {
    const auto *bank = std::get_if<CurrentSbac>(&snapshot.object.payload);
    const auto *sample = std::get_if<CurrentSbnk>(&snapshot.object.payload);
    if (!bank && !sample)
        return nullptr;
    const auto &storage = bank ? bank->storage : sample->storage;
    const bool supported = writable && snapshot.placement.has_value() && storage.structurally_valid;
    return {{"payloadSha256", package_internal::hex_digest(package_internal::sha256(payload))},
            {"partitionIndex", snapshot.partition.value},
            {"volumeName", snapshot.placement ? snapshot.placement->volume_name : ""},
            {"sampleFormat", bank ? sample_format_metadata(*bank) : sample_format_metadata(*sample)},
            {"canConvertFormat", supported},
            {"reason", supported ? "" : "This object's storage format or image does not support conversion."},
            {"formatConversions", sample_format_conversion_previews(payload, bank != nullptr)}};
}
} // namespace axk::app
