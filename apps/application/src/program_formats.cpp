#include "axklib/application/program_formats.hpp"

#include <charconv>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include "axklib/package_archive.hpp"
#include "axklib/program_format_conversion.hpp"

namespace axk::app {
using Json = nlohmann::json;

Json program_format_metadata(const DecodedObject &object) {
    if (object.header.type != ObjectType::prog)
        return nullptr;
    const auto *program = std::get_if<CurrentProg>(&object.payload);
    if (!program)
        return {{"format", "unknown"},
                {"structurallyValid", false},
                {"headerRevision", object.header.unknown_0x14},
                {"logicalSize", nullptr},
                {"storedAssignmentCount", nullptr},
                {"assignmentCapacity", nullptr},
                {"parameterTailBytes", nullptr}};
    const auto &layout = program->layout;
    return {{"format", program_storage_format_name(layout.parameter_tail_offset ? ProgramStorageFormat::a4000_a5000
                                                                                : ProgramStorageFormat::a3000)},
            {"structurallyValid", true},
            {"headerRevision", layout.version},
            {"logicalSize", layout.logical_size},
            {"storedAssignmentCount", layout.stored_assignment_count},
            {"assignmentCapacity", layout.assignment_capacity},
            {"parameterTailBytes", layout.parameter_tail_offset ? 176U : 0U}};
}

Json program_format_conversion(const ObjectSnapshot &snapshot, std::span<const std::byte> payload, bool writable) {
    const auto *program = std::get_if<CurrentProg>(&snapshot.object.payload);
    if (!program)
        return nullptr;
    const auto storage = inspect_program_storage(payload);
    const auto &name = snapshot.object.header.name;
    unsigned number{};
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), number);
    const bool has_slot =
        parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size() && number >= 1U && number <= 128U;
    const bool supported = writable && snapshot.placement.has_value() && storage.structurally_valid && has_slot;
    auto previews = Json::array();
    for (const auto target : {ProgramStorageFormat::a3000, ProgramStorageFormat::a4000_a5000}) {
        if (target == storage.format)
            continue;
        const auto plan = plan_program_format_conversion(payload, target);
        auto blockers = Json::array();
        for (const auto &issue : plan.blockers)
            blockers.push_back({{"key", issue.key},
                                {"message", issue.message},
                                {"storedValue", issue.stored_value ? Json(*issue.stored_value) : Json(nullptr)}});
        previews.push_back({{"targetFormat", program_storage_format_name(target)},
                            {"allowed", plan.allowed()},
                            {"changes", plan.changes},
                            {"blockers", std::move(blockers)}});
    }
    return {{"kind", "program"},
            {"payloadSha256", package_internal::hex_digest(package_internal::sha256(payload))},
            {"partitionIndex", snapshot.partition.value},
            {"volumeName", snapshot.placement ? snapshot.placement->volume_name : ""},
            {"programNumber", has_slot ? Json(number) : Json(nullptr)},
            {"programName", program->program_name},
            {"programFormat", program_format_metadata(snapshot.object)},
            {"canConvertFormat", supported},
            {"reason", supported ? "" : "This Program's storage, slot or image does not support conversion."},
            {"formatConversions", std::move(previews)}};
}
} // namespace axk::app
