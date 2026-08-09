#include "image_sessions_internal.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using axk::app::ImageProgramGenerationCandidate;
using axk::app::ImageProgramGenerationInspection;
using axk::app::ImageProgramGenerationNotice;

bool ascii_digit(char value) { return value >= '0' && value <= '9'; }

int natural_compare(std::string_view left, std::string_view right) {
    std::size_t left_index{};
    std::size_t right_index{};
    while (left_index < left.size() && right_index < right.size()) {
        if (ascii_digit(left[left_index]) && ascii_digit(right[right_index])) {
            const auto left_begin = left_index;
            const auto right_begin = right_index;
            while (left_index < left.size() && ascii_digit(left[left_index]))
                ++left_index;
            while (right_index < right.size() && ascii_digit(right[right_index]))
                ++right_index;
            auto left_significant = left_begin;
            auto right_significant = right_begin;
            while (left_significant < left_index && left[left_significant] == '0')
                ++left_significant;
            while (right_significant < right_index && right[right_significant] == '0')
                ++right_significant;
            const auto left_digits = left_index - left_significant;
            const auto right_digits = right_index - right_significant;
            if (left_digits != right_digits)
                return left_digits < right_digits ? -1 : 1;
            const auto left_number = left.substr(left_significant, left_digits);
            const auto right_number = right.substr(right_significant, right_digits);
            if (left_number != right_number)
                return left_number < right_number ? -1 : 1;
            const auto left_width = left_index - left_begin;
            const auto right_width = right_index - right_begin;
            if (left_width != right_width)
                return left_width < right_width ? -1 : 1;
            continue;
        }
        const auto left_folded = static_cast<char>(std::tolower(static_cast<unsigned char>(left[left_index])));
        const auto right_folded = static_cast<char>(std::tolower(static_cast<unsigned char>(right[right_index])));
        if (left_folded != right_folded)
            return left_folded < right_folded ? -1 : 1;
        ++left_index;
        ++right_index;
    }
    if (left_index != left.size() || right_index != right.size())
        return left_index == left.size() ? -1 : 1;
    if (left == right)
        return 0;
    return left < right ? -1 : 1;
}

std::optional<std::uint8_t> program_number(const axk::app::ImageObjectItem &item) {
    const auto &value = item.entry_name.empty() ? item.name : item.entry_name;
    unsigned int number{};
    const auto converted = std::from_chars(value.data(), value.data() + value.size(), number);
    if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() || number < 1U || number > 128U)
        return std::nullopt;
    return static_cast<std::uint8_t>(number);
}

std::optional<std::string> generated_name(std::string_view source) {
    while (!source.empty() && source.front() == ' ')
        source.remove_prefix(1U);
    while (!source.empty() && source.back() == ' ')
        source.remove_suffix(1U);
    source = source.substr(0U, std::min<std::size_t>(8U, source.size()));
    while (!source.empty() && source.back() == ' ')
        source.remove_suffix(1U);
    if (source.empty() ||
        !std::ranges::all_of(source, [](unsigned char value) { return value >= 0x20U && value <= 0x7eU; }))
        return std::nullopt;
    return std::string{source};
}

bool valid_program_name(std::string_view name) {
    return !name.empty() && name.size() <= 8U && name.front() != ' ' && name.back() != ' ' &&
           std::ranges::all_of(name, [](unsigned char value) { return value >= 0x20U && value <= 0x7eU; });
}

void notice(std::vector<ImageProgramGenerationNotice> &notices, std::string code, std::string message,
            std::vector<std::string> object_ids) {
    notices.push_back({std::move(code), std::move(message), std::move(object_ids)});
}

template <typename Session>
axk::app::Result<ImageProgramGenerationInspection>
inspect_session(const Session &session, std::uint64_t expected_revision, std::string_view content_scope_id) {
    if (session.revision != expected_revision)
        return std::unexpected(session_error("image_revision_stale", "image session revision changed", true));
    if (session.format != "sfs" || !session.media)
        return std::unexpected(
            session_error("image_mutation_unsupported", "Program generation requires an SFS image session"));
    const auto content_item = std::ranges::find(session.content, content_scope_id, &axk::app::ImageContentItem::id);
    if (content_item == session.content.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));
    if (content_item->kind != "volume" || !content_item->partition_index)
        return std::unexpected(session_error("content_scope_invalid", "Program generation requires a volume"));
    const auto scoped = session.object_indices_by_content_scope.find(std::string{content_scope_id});
    if (scoped == session.object_indices_by_content_scope.end())
        return std::unexpected(session_error("content_not_found", "content scope does not exist"));

    ImageProgramGenerationInspection result{.image_id = session.image_id,
                                            .revision = expected_revision,
                                            .content_scope_id = std::string{content_scope_id},
                                            .available_program_numbers = {},
                                            .candidates = {},
                                            .notices = {}};
    std::unordered_set<std::string> scope_ids;
    std::unordered_map<std::string, const axk::app::ImageObjectItem *> objects;
    std::unordered_map<std::string, std::vector<std::string>> names_by_type;
    std::set<std::uint8_t> occupied_program_numbers;
    for (const auto index : scoped->second) {
        if (index >= session.objects.size())
            return std::unexpected(
                session_error("image_session_invalid", "content scope references an invalid object"));
        const auto &item = session.objects[index];
        scope_ids.insert(item.id);
        objects.emplace(item.id, &item);
        if (item.type == "PROG") {
            if (const auto number = program_number(item))
                occupied_program_numbers.insert(*number);
            else
                notice(result.notices, "PROGRAM_SLOT_UNREADABLE",
                       "An existing Program has an unreadable slot number; generation excludes no additional slot",
                       {item.id});
        } else if (item.type == "SBAC" || item.type == "SBNK") {
            names_by_type[item.type + "\n" + item.name].push_back(item.id);
        }
    }
    for (std::uint16_t number = 1U; number <= 128U; ++number) {
        if (!occupied_program_numbers.contains(static_cast<std::uint8_t>(number)))
            result.available_program_numbers.push_back(static_cast<std::uint8_t>(number));
    }

    std::unordered_set<std::string> referenced_banks;
    std::unordered_set<std::string> referenced_samples;
    for (const auto &relationship : session.relationships) {
        const auto source = session.object_indices_by_id.find(relationship.source_object_id);
        if (source == session.object_indices_by_id.end() || source->second >= session.objects.size() ||
            session.objects[source->second].type != "PROG") {
            continue;
        }
        const auto mark = [&](const std::string &id) {
            const auto object = objects.find(id);
            if (object == objects.end())
                return;
            if (object->second->type == "SBAC")
                referenced_banks.insert(id);
            else if (object->second->type == "SBNK")
                referenced_samples.insert(id);
        };
        if (relationship.target_object_id)
            mark(*relationship.target_object_id);
        for (const auto &candidate : relationship.candidate_object_ids)
            mark(candidate);
    }
    for (const auto &relationship : session.relationships) {
        if (relationship.type != "SBAC_SLOT_TO_SBNK" || !referenced_banks.contains(relationship.source_object_id))
            continue;
        if (relationship.target_object_id && scope_ids.contains(*relationship.target_object_id))
            referenced_samples.insert(*relationship.target_object_id);
        for (const auto &candidate : relationship.candidate_object_ids) {
            if (scope_ids.contains(candidate))
                referenced_samples.insert(candidate);
        }
    }
    for (const auto &[id, object] : objects) {
        if (object->type != "SBNK")
            continue;
        const auto snapshot = session.snapshots_by_id.find(id);
        if (snapshot == session.snapshots_by_id.end())
            continue;
        const auto *sample = std::get_if<axk::CurrentSbnk>(&snapshot->second.object.payload);
        if (sample != nullptr && !sample->linked_program_numbers.empty())
            referenced_samples.insert(id);
    }

    struct EligibleBank {
        const axk::app::ImageObjectItem *object{};
        std::vector<std::string> members;
    };
    std::vector<EligibleBank> eligible_banks;
    for (const auto &[id, object] : objects) {
        if (object->type != "SBAC" || referenced_banks.contains(id))
            continue;
        if (names_by_type.at("SBAC\n" + object->name).size() != 1U) {
            notice(result.notices, "DUPLICATE_SAMPLE_BANK_NAME",
                   "A same-name Sample Bank is ambiguous and was not offered",
                   names_by_type.at("SBAC\n" + object->name));
            continue;
        }
        const auto snapshot = session.snapshots_by_id.find(id);
        const auto *bank = snapshot == session.snapshots_by_id.end()
                               ? nullptr
                               : std::get_if<axk::CurrentSbac>(&snapshot->second.object.payload);
        if (bank == nullptr) {
            notice(result.notices, "SAMPLE_BANK_UNREADABLE", "A Sample Bank could not be decoded and was not offered",
                   {id});
            continue;
        }
        const auto active_slots = static_cast<std::size_t>(
            std::ranges::count_if(bank->slots, [](const auto &slot) { return !slot.name.empty(); }));
        std::vector<std::string> members;
        bool exact = active_slots != 0U;
        for (const auto &relationship : session.relationships) {
            if (relationship.source_object_id != id || relationship.type != "SBAC_SLOT_TO_SBNK")
                continue;
            exact = exact && relationship.quality == "KNOWN" && relationship.target_object_id.has_value();
            if (!relationship.target_object_id)
                continue;
            const auto target = objects.find(*relationship.target_object_id);
            exact = exact && target != objects.end() && target->second->type == "SBNK";
            exact = exact && std::ranges::all_of(relationship.candidate_object_ids, [&](const auto &candidate) {
                        return candidate == *relationship.target_object_id;
                    });
            members.push_back(*relationship.target_object_id);
        }
        std::ranges::sort(members);
        const auto duplicate = std::ranges::adjacent_find(members) != members.end();
        exact = exact && members.size() == active_slots && !duplicate;
        if (!exact) {
            notice(result.notices, "SAMPLE_BANK_MEMBERSHIP_UNCERTAIN",
                   "A Sample Bank has incomplete, ambiguous, or cross-volume membership and was not offered", {id});
            continue;
        }
        if (std::ranges::any_of(members, [&](const auto &member) { return referenced_samples.contains(member); })) {
            notice(result.notices, "SAMPLE_BANK_MEMBER_REFERENCED",
                   "A Sample Bank contains a Sample already referenced by a Program and was not offered", {id});
            continue;
        }
        eligible_banks.push_back({object, std::move(members)});
    }

    std::unordered_map<std::string, std::size_t> membership_count;
    for (const auto &bank : eligible_banks) {
        for (const auto &member : bank.members)
            ++membership_count[member];
    }
    std::unordered_set<std::string> shared_samples;
    std::unordered_set<std::string> covered_samples;
    for (const auto &[id, count] : membership_count) {
        if (count > 1U)
            shared_samples.insert(id);
    }
    std::vector<EligibleBank> disjoint_banks;
    for (auto &bank : eligible_banks) {
        if (std::ranges::any_of(bank.members, [&](const auto &member) { return shared_samples.contains(member); })) {
            notice(result.notices, "SHARED_SAMPLE_MEMBERSHIP",
                   "A Sample belongs to multiple unreferenced Sample Banks; the overlapping objects were not offered",
                   {bank.object->id});
            continue;
        }
        disjoint_banks.push_back(std::move(bank));
    }
    const auto bank_less = [](const auto &left, const auto &right) {
        const auto compared = natural_compare(left.object->name, right.object->name);
        return compared != 0 ? compared < 0 : left.object->id < right.object->id;
    };
    std::ranges::sort(disjoint_banks, bank_less);
    for (const auto &bank : disjoint_banks) {
        const auto name = generated_name(bank.object->name);
        if (!name) {
            notice(result.notices, "PROGRAM_NAME_UNAVAILABLE",
                   "A Sample Bank name cannot be represented as a Program name and was not offered", {bank.object->id});
            continue;
        }
        covered_samples.insert(bank.members.begin(), bank.members.end());
        result.candidates.push_back({.target_object_id = bank.object->id,
                                     .target_object_type = bank.object->type,
                                     .target_object_name = bank.object->name,
                                     .default_program_name = *name,
                                     .program_number = std::nullopt,
                                     .default_selected = false});
    }

    std::vector<const axk::app::ImageObjectItem *> samples;
    for (const auto &[id, object] : objects) {
        if (object->type != "SBNK" || referenced_samples.contains(id) || covered_samples.contains(id) ||
            shared_samples.contains(id)) {
            continue;
        }
        if (names_by_type.at("SBNK\n" + object->name).size() != 1U) {
            notice(result.notices, "DUPLICATE_SAMPLE_NAME", "A same-name Sample is ambiguous and was not offered",
                   names_by_type.at("SBNK\n" + object->name));
            continue;
        }
        const auto snapshot = session.snapshots_by_id.find(id);
        if (snapshot == session.snapshots_by_id.end() ||
            std::get_if<axk::CurrentSbnk>(&snapshot->second.object.payload) == nullptr) {
            notice(result.notices, "SAMPLE_UNREADABLE", "A Sample could not be decoded and was not offered", {id});
            continue;
        }
        samples.push_back(object);
    }
    std::ranges::sort(samples, [](const auto *left, const auto *right) {
        const auto compared = natural_compare(left->name, right->name);
        return compared != 0 ? compared < 0 : left->id < right->id;
    });
    for (const auto *sample : samples) {
        const auto name = generated_name(sample->name);
        if (!name) {
            notice(result.notices, "PROGRAM_NAME_UNAVAILABLE",
                   "A Sample name cannot be represented as a Program name and was not offered", {sample->id});
            continue;
        }
        result.candidates.push_back({.target_object_id = sample->id,
                                     .target_object_type = sample->type,
                                     .target_object_name = sample->name,
                                     .default_program_name = *name,
                                     .program_number = std::nullopt,
                                     .default_selected = false});
    }
    for (std::size_t index = 0U; index < result.candidates.size() && index < result.available_program_numbers.size();
         ++index) {
        result.candidates[index].program_number = result.available_program_numbers[index];
        result.candidates[index].default_selected = true;
    }
    return result;
}

} // namespace

axk::app::Result<axk::app::ImageProgramGenerationInspection>
axk::app::ImageSessionManager::inspect_program_generation(std::string_view image_id, std::string_view owner_id,
                                                          std::uint64_t expected_revision,
                                                          std::string_view content_scope_id) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    return inspect_session(**session, expected_revision, content_scope_id);
}

axk::app::Result<axk::app::ImageProgramGenerationPlan> axk::app::ImageSessionManager::plan_program_generation(
    std::string_view image_id, std::string_view owner_id, std::uint64_t expected_revision,
    std::string_view content_scope_id, const std::vector<ImageProgramGenerationSelection> &selections) {
    const auto session = implementation_->owned(image_id, owner_id);
    if (!session)
        return std::unexpected(session.error());
    const std::scoped_lock access{(*session)->access_mutex};
    auto inspection = inspect_session(**session, expected_revision, content_scope_id);
    if (!inspection)
        return std::unexpected(inspection.error());
    const auto content_item = std::ranges::find((*session)->content, content_scope_id, &ImageContentItem::id);
    if (content_item == (*session)->content.end() || !content_item->partition_index)
        return std::unexpected(session_error("content_scope_invalid", "Program generation requires a volume"));

    std::unordered_map<std::string, const ImageProgramGenerationCandidate *> candidates;
    for (const auto &candidate : inspection->candidates)
        candidates.emplace(candidate.target_object_id, &candidate);
    std::unordered_map<std::string, const ImageProgramGenerationSelection *> selected;
    std::set<std::uint8_t> selected_numbers;
    for (const auto &selection : selections) {
        if (!candidates.contains(selection.target_object_id))
            return std::unexpected(session_error(
                "program_generation_stale", "a selected object is no longer eligible for Program generation", true));
        if (!selected.emplace(selection.target_object_id, &selection).second ||
            !selected_numbers.insert(selection.program_number).second) {
            return std::unexpected(
                session_error("program_generation_invalid", "Program generation selections must be unique"));
        }
        if (!valid_program_name(selection.program_name))
            return std::unexpected(session_error(
                "program_generation_invalid", "generated Program names must contain 1-8 printable ASCII characters"));
    }
    if (selections.empty())
        return std::unexpected(session_error("program_generation_invalid", "select at least one Program to generate"));
    if (selections.size() > inspection->available_program_numbers.size())
        return std::unexpected(
            session_error("program_generation_capacity", "the volume has too few free Program slots"));

    AlterationManifest manifest{std::string{alteration_manifest_schema_version}, {}};
    std::vector<ImageProgramGenerationSelection> ordered;
    for (const auto &candidate : inspection->candidates) {
        const auto found = selected.find(candidate.target_object_id);
        if (found == selected.end())
            continue;
        const auto expected_number = inspection->available_program_numbers[ordered.size()];
        if (found->second->program_number != expected_number)
            return std::unexpected(session_error("program_generation_stale",
                                                 "generated Program slots no longer match the reviewed free-slot plan",
                                                 true));
        ordered.push_back(*found->second);
        ProgramAssignmentSpec assignment{candidate.target_object_type, candidate.target_object_name, 0U,
                                         ProgramReceiveMode::sample};
        ProgramSpec program{expected_number, found->second->program_name, {std::move(assignment)}};
        manifest.operations.push_back({std::format("generate-program-{:03}", expected_number),
                                       InsertProgramOperation{PartitionIndex{*content_item->partition_index},
                                                              content_item->name, std::move(program)}});
    }
    return ImageProgramGenerationPlan{std::move(*inspection), std::move(ordered), std::move(manifest)};
}
