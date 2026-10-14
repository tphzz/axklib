#include "axklib/formats/tx16w/dependencies.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string_view>
#include <variant>

namespace axk::tx16w::detail {
namespace {

std::string symbol(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return character == '_' ? ' ' : static_cast<char>(std::toupper(character));
    });
    while (!value.empty() && value.back() == ' ')
        value.pop_back();
    return value;
}

bool matches(const FileIdentity &expected, const FileIdentity &candidate) {
    return expected.native_slot == candidate.native_slot && symbol(expected.set_name) == symbol(candidate.set_name) &&
           expected.kind == candidate.kind;
}

} // namespace

std::string reference_filename(std::string name, char kind, std::uint8_t slot) {
    name += '.';
    name += kind;
    const auto number = static_cast<unsigned int>(slot) + 1U;
    if (number < 10U)
        name += '0';
    return name + std::to_string(number);
}

std::vector<native::SourceRequirement> source_requirements(std::span<const NativeObject> objects,
                                                           std::span<const NativeFile> files) {
    std::vector<native::SourceRequirement> result;
    const auto require = [&](const FileIdentity &identity, std::optional<std::string> object_id) {
        std::size_t count{};
        const NativeFile *match{};
        for (const auto &file : files)
            if (matches(identity, file.identity)) {
                ++count;
                match = &file;
            }
        if (count == 1U && match->decoded)
            return;
        // Unsupported audio is only a diagnostic candidate, never a resolved
        // dependency and never used for grouping two sources. Its slot is unknown.
        const bool unsupported_candidate = identity.kind == 'W' && std::ranges::any_of(files, [&](const auto &file) {
                                               return file.unsupported && file.identity.kind == 'C' &&
                                                      symbol(file.identity.set_name) == symbol(identity.set_name);
                                           });
        const auto state = count == 0U          ? (unsupported_candidate ? native::DependencyState::unsupported
                                                                         : native::DependencyState::missing)
                           : count > 1U         ? native::DependencyState::ambiguous
                           : match->unsupported ? native::DependencyState::unsupported
                                                : native::DependencyState::invalid;
        auto name = reference_filename(identity.set_name, identity.kind, identity.native_slot);
        if (std::ranges::none_of(result, [&](const auto &item) { return item.name == name && item.state == state; }))
            result.push_back({std::move(name), state, std::move(object_id)});
    };
    for (const auto &file : files) {
        if (!file.decoded || (file.identity.kind != 'S' && file.identity.kind != 'V' && file.identity.kind != 'U'))
            continue;
        auto expected = file.identity;
        expected.kind = expected.kind == 'S' ? 'V' : 'S';
        require(expected, std::nullopt);
    }
    for (const auto &object : objects)
        if (const auto *setup = std::get_if<NativeSetup>(&object.value))
            for (const auto &wave : setup->waves)
                require({'W', {}, wave.name, wave.slot}, object.id);
    return result;
}

bool supplies_requirement(const Document &target, const Document &candidate) {
    for (const auto &requirement : target.summary().source_requirements) {
        if (requirement.state != native::DependencyState::missing &&
            requirement.state != native::DependencyState::unsupported)
            continue;
        const auto expected = identify_file(requirement.name);
        if (expected && std::ranges::any_of(candidate.native_files(),
                                            [&](const auto &file) { return matches(*expected, file.identity); }))
            return true;
    }
    return false;
}

bool supplies_requirement(const native::SourceRequirement &requirement, const native::SourceMember &candidate) {
    const auto expected = identify_file(requirement.name);
    if (!expected || (requirement.state != native::DependencyState::missing &&
                      requirement.state != native::DependencyState::unsupported))
        return false;
    for (const auto &root : candidate.roots)
        for (const auto &entry : root.entries) {
            const auto identity = identify_file(entry.path);
            if (!entry.directory && identity && matches(*expected, *identity))
                return true;
        }
    return false;
}

} // namespace axk::tx16w::detail
